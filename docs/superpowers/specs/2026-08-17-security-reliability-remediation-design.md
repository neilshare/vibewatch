# VibeWatch Security and Reliability Remediation Design

## Purpose

This design resolves every finding from the 2026-08-17 architecture review without rewriting the product UI. It upgrades approval handling from an uncorrelated button event into an authenticated transaction, moves all firmware state mutation onto the Arduino main loop, removes misleading telemetry defaults, unifies host behavior, and adds automated plus real-device verification.

The implementation must preserve the existing three-card UI, touch gestures, HID agent controls, quota display, sound, haptics, power management, and persisted settings unless this document explicitly changes their behavior.

## Scope

The remediation is divided into three independently testable delivery stages:

1. Secure approval and BLE protocol v2.
2. Firmware ownership boundaries and deterministic device state.
3. Host tools, tests, CI, repository hygiene, and protocol documentation.

The following work is intentionally out of scope:

- redesigning the watch UI;
- changing supported hardware;
- adding cloud services or account credentials to the watch;
- replacing JSON with a binary serialization format;
- creating a general-purpose event bus for every firmware subsystem.

One targeted readability correction is in scope: the `1` through `6` labels inside the six agent circles are currently too small to read reliably and will be rendered at twice their current character scale.

## Compatibility Policy

Approval protocol v2 is the default and authoritative path. It returns a decision correlated to a unique request identifier.

The existing v1 approval payload and generic `ACT07`/`ACT08` responses remain available for one release cycle through an explicit `--legacy-approval` host option. The legacy payload is accepted only on the new encrypted approval-request characteristic; the old mixed quota/approval write path no longer accepts approvals. Host tools never select legacy mode unless the option is present. A v2 decision must never also emit a legacy action, because duplicate delivery could execute an operation twice.

All existing non-approval HID controls retain their current report values and card tagging.

## Architecture

### Component boundaries

The firmware will introduce focused modules while retaining `src/main.cpp` as the composition root:

- `ApprovalProtocol`: transport-independent request/decision types, validation limits, and JSON encoding/decoding.
- `ApprovalController`: the single-pending-request state machine, duplicate handling, expiry, decision correlation, and optional legacy adapter.
- `BleIngress`: NimBLE callbacks that perform only connection checks, byte-length checks, copying, and bounded queue submission.
- `DeviceState`: quota, card, approval, and UI-visible state owned exclusively by the Arduino main loop.
- Existing rendering, input, power, sound, and haptic code remains in `main.cpp` initially, but consumes `DeviceState` and `ApprovalController` rather than callback-mutated globals.

The Swift companion will be split into a reusable library target and a small executable target:

- `AppServerClient`: reads and validates Codex rate-limit snapshots.
- `BLETransport`: discovers only an explicitly bound device for real writes and exposes approval write/result operations.
- `ProtocolModels`: Codable quota and approval messages shared by CLI behavior and tests.
- `CLI`: validates arguments, selects demo or real mode, prints structured results, and maps errors to nonzero exit status.

The Python script will be a compatibility wrapper around a normalized request model. It may call the Swift companion first and use Bleak only as an explicit supported fallback, but both backends must receive the same validated values.

### Runtime data flow

1. The host creates an `ApprovalRequestV2` and connects to the exact `--device-id`.
2. The host subscribes to the encrypted approval-result indication characteristic.
3. The host writes the request with an ATT response to the encrypted approval-request characteristic.
4. The NimBLE callback confirms encryption and bonding, copies the bounded payload into the FreeRTOS ingress queue, and returns without parsing JSON or touching hardware.
5. The Arduino main loop parses and validates the message, then asks `ApprovalController` to accept it.
6. The controller accepts a new request, treats the same identifier as an idempotent retry, or rejects a different request with `busy` while one remains pending.
7. Accepted requests update `DeviceState`; the main loop wakes the screen and triggers sound and vibration.
8. A touch or physical-button decision is passed to `ApprovalController`, which atomically closes the matching request and creates `ApprovalDecisionV2`.
9. The decision is sent as an encrypted indication. The Swift companion waits for that exact `request_id`, prints a machine-readable result, and exits.
10. Disconnect, timeout, or expiry closes the pending operation with an explicit status; none of these conditions are reported as approval.

## BLE Security Model

The existing quota service remains discoverable, but approval and quota traffic no longer share a writable characteristic.

The service exposes:

- quota snapshot write characteristic: encrypted write with response;
- approval request characteristic: encrypted and authenticated write with response;
- approval result characteristic: encrypted indication;
- a one-release legacy adapter on the encrypted approval-request characteristic.

Every callback verifies the current connection is encrypted before enqueueing a message. Approval additionally requires an established bond. Characteristic permissions reject unencrypted writes before the callback; a callback that cannot confirm the expected bonded peer disconnects it without triggering UI, sound, vibration, quota updates, or queue allocation.

Real host operations require `--device-id`. Name- or service-based discovery without a pinned identifier is allowed only for `--demo`, whose output must clearly state that it is discovery/demo mode. A real operation never falls back from a missing bound device to another same-name peripheral.

The service UUIDs and characteristic UUIDs are documented in `docs/COMPANION_PROTOCOL.md`. Existing UUIDs are retained only where doing so does not weaken the permission boundary.

## Approval Protocol v2

### Request

The canonical JSON shape is:

```json
{
  "version": 2,
  "kind": "approval_request",
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "card": "codex",
  "agent_id": 0,
  "operation_type": "EXEC",
  "summary": "Run the firmware test suite",
  "ttl_ms": 30000
}
```

Validation rules:

- `version` must equal `2` and `kind` must equal `approval_request`.
- `request_id` must be a canonical UUID string and is the idempotency key.
- `card` must be exactly `codex`, `workbuddy`, or `antigravity` after lowercase normalization.
- `agent_id` must be in `0...5`.
- `operation_type` must be 1 to 23 UTF-8 bytes.
- `summary` must be 1 to 95 UTF-8 bytes and must end on a valid UTF-8 boundary.
- `ttl_ms` must be between 5,000 and 120,000 milliseconds.
- The complete payload must not exceed 512 bytes.

The firmware records `received_at_ms = millis()` and derives expiry from the validated TTL. It does not depend on wall-clock synchronization.

### Decision

The canonical result is:

```json
{
  "version": 2,
  "kind": "approval_decision",
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "decision": "approve",
  "decided_at_ms": 184230
}
```

`decision` is one of `approve`, `reject`, `expired`, or `cancelled`. Transport and validation failures use a separate error result and are never mapped to a user decision.

### State machine

The controller states are `idle`, `pending`, and `decided` during result delivery.

- `idle + valid new request` becomes `pending`.
- `pending + same request_id` returns an idempotent acknowledgement without replaying sound or vibration.
- `pending + different request_id` returns `busy` and preserves the visible request.
- `pending + matching user decision` becomes `decided`, emits exactly one result, then returns to `idle` after indication acknowledgement or a bounded delivery timeout.
- `pending + TTL expiry` emits `expired`, clears the modal, and returns to `idle` after result handling.
- Disconnect clears a pending request as `cancelled`; reconnect does not silently restore it.

## Firmware Ownership and Queueing

All UI-visible and hardware-related state is main-loop owned. NimBLE callbacks may update only callback-local data and thread-safe queues.

The ingress queue uses fixed-size items rather than per-message `malloc`. Each item contains message kind, connection handle, payload length, and a 512-byte payload buffer. Queue capacity is bounded and overflow produces a logged/returned `queue_full` error without replacing earlier messages.

The main loop processes a bounded number of ingress items per iteration so touch, BLE notifications, haptics, and rendering remain responsive. JSON parsing, card selection, quota mutation, approval mutation, CPU-frequency changes, speaker calls, vibration calls, and display calls occur only in the main loop.

The existing HID RPC reassembly must become connection-scoped or explicitly single-connection. Chunks from different connection handles must never share one receive buffer.

## Telemetry Semantics

Every quota starts with `available=false`. The watch displays a waiting/not-synced state until it receives a valid snapshot.

A valid snapshot records `receivedAtMs`. When it exceeds the existing stale interval, the last real value may remain visible but must be labeled stale. Failed syncs never fabricate a percentage, credit balance, or reset time.

Credit values and percentages are validated before state mutation. An invalid update leaves the previous valid snapshot unchanged and produces an explicit host error when the transport supports responses.

## Agent Label Readability

In the normal agent layer, the six numeric labels use the existing `Orbitron_Light_32` font with text scale increased from `1.0` to `2.0`. The circle radius, orbit positions, selected/pressed outlines, colors, animations, and touch targets remain unchanged.

The larger label remains centered on each circle and uses the existing luminance-based black/white contrast selection. The change does not apply to the action layer labels or glyphs (`FAST`, `OK`, `NG`, `PLAN`, and `AI`). Rendering must explicitly set the numeric text scale before drawing and restore the expected scale afterward so later quota and status text is unaffected.

## Host Behavior

### Swift companion

Non-demo quota and approval writes require a valid `--device-id`. Approval mode generates a UUID unless `--request-id` is supplied for retry/testing, subscribes to the result characteristic, sends the request, and waits up to the request TTL plus a five-second transport grace period.

Human-readable progress goes to stderr. Final machine-readable JSON goes to stdout. Exit status is zero only for a completed transport operation with a valid result; rejection remains a successfully delivered decision represented in JSON, while timeout, malformed response, missing device, and authorization failure are nonzero transport errors.

CLI values are validated before starting App Server or Bluetooth. Unknown cards, negative credits, nonpositive total credits, oversized text, incompatible mode flags, and missing real-device binding fail immediately.

### Python wrapper

`--auto` explicitly selects App Server quota discovery. Manual `--remaining` and `--reset` select a manual quota snapshot and must not be replaced by an automatic Swift read. `--card`, credits, totals, request identifiers, TTL, and device identifier are carried through every backend.

The old broad exception handling and synthetic 85% fallback are removed. Failures are printed to stderr and returned through a nonzero exit code. Bleak fallback is not automatic after an authentication or validation failure; it may be selected explicitly or used only for unsupported-platform transport initialization failures.

## Error Handling and Observability

Errors have stable codes shared by documentation and host tests:

- `unauthorized`: connection is not encrypted/bonded;
- `invalid_payload`: JSON or field validation failed;
- `unsupported_version`: protocol version is not supported;
- `busy`: another request is pending;
- `queue_full`: ingress queue cannot accept the message;
- `expired`: request TTL elapsed;
- `device_not_found`: pinned CoreBluetooth device is unavailable;
- `transport_error`: write, indication, disconnect, or timeout failure.

Serial logs include event code and truncated request ID, but never print the complete approval summary by default. Verbose host logs may include the bound CoreBluetooth UUID only when the user requested verbose mode. No account token or Codex credential crosses BLE.

## Testing Strategy

### Pure C++ tests

Host-native tests cover:

- valid request acceptance;
- canonical field validation and UTF-8 boundaries;
- same-ID idempotent retry;
- different-ID busy rejection;
- approve/reject correlation;
- expiry and disconnect cancellation;
- exactly-once decision emission;
- legacy mode selected only by the explicit host option and accepted only on the encrypted characteristic;
- quota unavailable/fresh/stale transitions;
- queue-capacity behavior and connection-scoped reassembly.

Firmware build and real-device checks additionally verify that the numeric label scale is `2.0` only in the normal agent-layer draw path and that subsequent UI text retains its intended scale.

### Swift tests

Swift Package tests cover:

- current and legacy App Server rate-limit fixtures;
- missing/malformed App Server data;
- quota and approval Codable round trips;
- every CLI mode and invalid combination;
- required `--device-id` for real writes;
- demo discovery without a pinned identifier;
- matching, mismatched, duplicate, and timed-out decision indications;
- stable JSON output and exit-status mapping through injectable transports.

### Python tests

Pytest with mocked subprocess and Bleak adapters covers:

- automatic versus manual quota modes;
- complete argument forwarding to Swift;
- card preservation in approval and quota payloads;
- explicit fallback selection;
- App Server protocol parsing;
- missing device/write failure propagation;
- removal of fabricated quota defaults;
- success and failure exit codes.

### CI

Linux CI runs repository hygiene checks, Python tests, host-native C++ tests, and the ESP32 firmware build. macOS CI runs `swift build` and `swift test`. CI must not publish a firmware artifact unless all required jobs for the commit succeed.

Generated `__pycache__`, `*.pyc`, PlatformIO, Swift, and local wrapper artifacts are ignored and removed from version control.

### Real-device acceptance

Using one M5Stack StopWatch and one macOS machine, the release candidate must pass a recorded checklist:

1. Fresh boot shows quota as not synchronized.
2. An unpaired write cannot change quota or open an approval modal.
3. Pairing and pinned-device discovery succeed.
4. Real quota and Workbuddy credit updates target the requested card.
5. Approve and reject return the matching request ID exactly once.
6. Duplicate request IDs do not replay alerts.
7. A concurrent different request receives `busy` and does not replace the modal.
8. Expired requests close without generating approval.
9. Disconnect during approval produces cancellation and no stale decision after reconnect.
10. Sleep wake-up, touch decision, physical-button decision, card gestures, microphone, sound, vibration, and battery display still work.
11. Legacy approval fails by default and succeeds only with the explicit compatibility option.
12. Repeated disconnect/reconnect and at least 100 approval cycles show no queue leak, heap degradation, crash, or stuck modal.
13. Labels `1` through `6` render at twice the previous character size, remain centered and fully inside every circle in default, selected, pressed, and animated states, and do not change action-layer typography.

The checklist records firmware commit, companion commit, macOS version, device identifier suffix, pass/fail status, and sanitized log excerpts.

## Documentation and Release

`docs/COMPANION_PROTOCOL.md` becomes the authoritative protocol reference. Root English and Chinese READMEs use pinned-device examples for real writes and distinguish demo discovery. `companion/README.md` is corrected to describe only implemented behavior.

Release notes identify protocol v2, the explicit legacy option, the removal schedule after one release cycle, and the security reason for the change. Firmware version and companion version are incremented together so support reports can identify compatible pairs.

## Acceptance Criteria

The remediation is complete only when:

- unencrypted or unbound clients cannot mutate watch state;
- every v2 approval decision is correlated to exactly one request ID;
- a pending request cannot be silently replaced;
- NimBLE callbacks perform no JSON parsing, UI mutation, or hardware operation;
- the watch never presents fabricated telemetry as fresh;
- manual and automatic CLI modes preserve all user-supplied values and failures;
- firmware, native C++, Python, and Swift tests pass in CI;
- the real-device acceptance checklist passes in full;
- protocol and user documentation match the shipped implementation;
- generated Python bytecode is absent from version control.
- all six agent-circle numbers are twice their previous character size without clipping or changing hit targets.

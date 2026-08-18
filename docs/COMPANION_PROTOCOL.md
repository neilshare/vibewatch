# VibeWatch Companion Protocol v2

This document is the authoritative wire and command-line contract for VibeWatch
firmware `v1.01` and companion protocol `2`. Protocol v2 is transactional: an
approval request and its decision share one canonical `request_id`.

## Transport and trust boundary

The firmware advertises the private service below in addition to BLE HID.

| Role | UUID | GATT behavior | Maximum value |
| --- | --- | --- | --- |
| Private service | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01` | Service discovery | — |
| Quota write | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02` | Write with response; encrypted ATT permission | 512 bytes |
| Approval request | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c03` | Write with response; encrypted ATT permission; firmware also requires a bonded peer | 512 bytes |
| Approval result | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c04` | Indicate; encrypted read permission | 512 bytes |

The StopWatch has no keyboard or display passkey exchange. It is configured for
bonding and Secure Connections with no man-in-the-middle authentication. The
approval callback rejects an unencrypted connection and disconnects an
encrypted but unbonded peer. The quota characteristic is encryption-gated but
does not add the approval callback's explicit bond check.

For every non-demo operation, the host must use the exact CoreBluetooth UUID
selected by the operator with `--device-id`. The Swift and Python transports
retrieve or scan for that identifier and reject other peripherals. A name such
as `Vibe Watch #1` is not an authorization boundary. Demo is the only mode that
may scan broadly by service or name, and it writes synthetic quota data.

All GATT payloads are UTF-8 JSON. A payload over 512 bytes is rejected before
JSON processing. Hosts must use writes with response; there is no
write-without-response downgrade.

## Quota snapshot (`.02`)

The companion writes a single JSON object:

```json
{
  "card": "codex",
  "remaining_percent": 59.0,
  "reset_in_seconds": 3600
}
```

Optional credit fields are sent as a pair:

```json
{
  "card": "workbuddy",
  "credits": 1250.0,
  "remaining_percent": 83.3333333333,
  "reset_in_seconds": 0,
  "total_credits": 1500.0
}
```

The supported contract is:

- `card`: optional lowercase `codex`, `workbuddy`, or `antigravity`; omission
  targets the card currently shown on the watch;
- `remaining_percent`: required finite number from `0` through `100`;
- `reset_in_seconds`: nonnegative integer seconds; companion payloads always
  include it;
- `credits` and `total_credits`: optional finite numbers that must appear
  together, with `credits >= 0`, `total_credits > 0`, and
  `credits <= total_credits`.

Invalid snapshots do not replace the last valid value. Until the first valid
write, the watch displays quota as unavailable. A valid snapshot becomes stale
after 180 seconds without refresh; stale values remain visible but are marked
as stale.

Quota writes are acknowledged at ATT level. They do not produce a `.04`
indication.

## Approval request (`.03`)

Canonical request:

```json
{
  "agent_id": 2,
  "card": "codex",
  "kind": "approval_request",
  "operation_type": "EXEC",
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "summary": "Run the test suite",
  "ttl_ms": 30000,
  "version": 2
}
```

Every field is required. Validation is byte-based where noted:

| Field | Contract |
| --- | --- |
| `version` | Integer `2` |
| `kind` | Exact string `approval_request` |
| `request_id` | Canonical lowercase UUID: 36 ASCII bytes, including hyphens at positions 9, 14, 19, and 24 |
| `card` | `codex`, `workbuddy`, or `antigravity`; firmware accepts case-insensitive spelling, while supported hosts emit lowercase |
| `agent_id` | Integer `0...5`, displayed as label `1...6` |
| `operation_type` | Valid UTF-8, no embedded NUL, `1...23` bytes |
| `summary` | Valid UTF-8, no embedded NUL, `1...95` bytes |
| `ttl_ms` | Integer `5000...120000` |

Unknown fields are ignored by firmware but are not part of the compatibility
contract.

### Required transaction order

1. Connect to the exact `--device-id` peripheral.
2. Discover service `.01` and characteristics `.03` and `.04`.
3. Enable `.04` indications.
4. Wait until CoreBluetooth confirms the subscription.
5. Write the request to `.03` with response.
6. Wait for the ATT write acknowledgement and one matching `.04` result.
7. Accept only an indication whose nonempty `request_id` exactly matches the
   in-flight request. The host deadline is `ttl_ms / 1000 + 5` seconds.

The supported companions implement this order. They do not write first and do
not fall back to write without response.

## Approval result (`.04`)

A user decision or firmware expiry is an indication:

```json
{
  "decided_at_ms": 184230,
  "decision": "approve",
  "kind": "approval_decision",
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "version": 2
}
```

- `version` is integer `2`, and `kind` is exact string `approval_decision`;
- `decision` is one of `approve`, `reject`, `expired`, or `cancelled`;
- `decided_at_ms` is the firmware's unsigned 32-bit monotonic millisecond
  timestamp, not wall-clock time;
- `request_id` is always the accepted request's exact canonical UUID.

Approve, reject, expired, and a delivered cancelled result are protocol
outcomes, not command failures. A physical disconnect cancels the request in
firmware and closes the modal, but an indication cannot be delivered after the
link is gone; the current host therefore reports a transport failure. No
decision from that cancelled request is retained or replayed after reconnect.

Protocol and transport errors use:

```json
{
  "code": "busy",
  "kind": "error",
  "message": "another request is pending",
  "request_id": "7d444840-9dc0-11d1-b245-5ffdce74fad2",
  "version": 2
}
```

Stable firmware codes are:

| Code | Meaning and correlation |
| --- | --- |
| `invalid_payload` | JSON or field validation failed; `request_id` is empty when firmware could not safely recover it |
| `unsupported_version` | A versioned request was not v2; `request_id` is empty |
| `busy` | A different request is pending; contains the rejected request's ID |
| `queue_full` | The bounded approval ingress queue is full; `request_id` is empty |
| `transport_error` | The approval result path is unavailable or failed |
| `unauthorized` | Reserved stable code for an authorization rejection; ATT security or disconnect may reject before an indication can be sent |

`request_id` is a required JSON string in firmware error indications. It is
either a canonical lowercase UUID or `""`. Because each supported companion
allows only one in-flight approval per session, it associates an empty-ID
error only with that unique transaction. A nonempty mismatched ID is ignored.
Malformed indications are ignored and cannot approve a request.

For every firmware error, `version` is integer `2`, `kind` is exact string
`error`, `code` is one of the stable strings above, and `message` is valid
UTF-8 with no embedded NUL and `1...95` bytes. Supported hosts preserve a
received firmware code and message in their machine error output, which omits
`request_id` because the command has already correlated the indication.

The error encoder also defines `queue_full`; there is no unbounded queue. An
indication that does not complete within five seconds makes the firmware
disconnect that connection instead of accepting another decision on an
ambiguous transport.

## State-machine semantics

- One v2 approval may be pending at a time.
- Retrying the same `request_id` while it is pending is idempotent: firmware
  keeps the original modal and does not replay its alert.
- A different `request_id` while one is pending receives `busy` and cannot
  replace the modal.
- TTL starts when the main loop accepts the request. At expiry, the modal
  closes and `expired` is indicated; expiry never implies approval.
- Approve and reject each produce at most one matching decision.
- Disconnect cancels the active request, clears its UI state, and prevents a
  stale decision after reconnect.
- A v2 decision never emits the legacy generic HID action, preventing one user
  gesture from executing twice.

## Command-line results

Use `--device-id COREBLUETOOTH_UUID` for every real write. `--demo` is the only
unpinned Bluetooth mode and writes synthetic data to a broadly discovered
device.

| Result | Swift companion | Python native backend | Python Bleak backend |
| --- | --- | --- | --- |
| Approval decision | One sorted-key JSON object on stdout, exit `0` | Preserves Swift behavior | One sorted-key JSON object on stdout, exit `0` |
| Quota success | One quota JSON object on stdout, exit `0` | Preserves Swift behavior | One quota JSON object on stdout, exit `0` |
| Legacy ATT acknowledgement | No machine result, exit `0` | Preserves Swift behavior | No machine result, exit `0` |
| Transport/protocol failure | Stable v2 error JSON on stdout, exit `1` | Preserves child streams and exit status | Stable v2 error JSON on stderr, exit `1` |
| Usage failure | Stable `usage_error` JSON on stdout, exit `2` | Wrapper validation prints a human-readable error on stderr, exit `2` | Human-readable usage error on stderr, exit `2` |

Human progress and verbose discovery go to stderr. In `--watch` mode, each
successful quota snapshot is streamed as one line on stdout. Reject, expired,
and cancelled are decisions and therefore exit `0` when their result was
delivered; callers must inspect `decision` rather than treating exit `0` as
approval.

## Legacy approval migration

Firmware `v1.01` accepts the v1 shape only on encrypted, bonded approval
characteristic `.03`:

```json
{
  "method": "v.oai.approval_req",
  "params": {
    "active": true,
    "card": "codex",
    "summary": "Run Command",
    "type": "EXEC"
  }
}
```

Legacy mode is selected only by `--approval --legacy-approval`; hosts never
downgrade automatically. It ends after the `.03` ATT acknowledgement and does
not subscribe to `.04`. A later physical decision follows the legacy HID
action path (`ACT07`/`ACT08`) and does not also emit a v2 decision. Legacy
approval is not accepted through the HID JSON-RPC or quota `.02` paths.

The legacy decoder requires exact method `v.oai.approval_req`, object `params`,
boolean `active: true`, and valid `type`/`summary` strings with the same 23-byte
and 95-byte maxima as v2. `card` is optional and presentation stays on the
currently selected card when it is omitted;
`agentId` (or the older `agent` alias) is optional and must be `0...5`. The
supported host emits `card` but no agent field. Legacy has no correlated
`request_id`, result indication, or supported TTL contract.

This adapter exists for the `v1.01` release cycle only and is scheduled for
removal in the next firmware/companion release. New integrations must use v2.

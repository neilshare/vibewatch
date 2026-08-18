# Protocol v2 real-device acceptance record

Use this document for one release-candidate commit, one M5Stack StopWatch, and
one macOS host. Do not enter a full CoreBluetooth UUID, username, home path,
Bluetooth address, bond key, or unsanitized log in this file. Replace every
`<...>` field during execution; use `PASS`, `FAIL`, or `BLOCKED` rather than
leaving a required result blank.

The protocol and expected outcomes are defined in
[`COMPANION_PROTOCOL.md`](COMPANION_PROTOCOL.md). Any failed case requires a
separate red/green production fix and then a complete rerun from a clean
worktree.

## Immutable test inputs

| Field | Recorded value |
| --- | --- |
| Firmware source commit | `a848d0936d77648972e3cb9340d39a84e7d7a896` |
| Companion source commit | `a848d0936d77648972e3cb9340d39a84e7d7a896` |
| Firmware version / protocol | `v1.01 / 2` |
| macOS version/build | `macOS 26.5.2 (25F84)` |
| Swift version | `Apple Swift 6.3.3 (swiftlang-6.3.3.1.3 clang-2100.1.1.101)` |
| PlatformIO version | `Core 6.1.19` |
| Firmware SHA-256 | `7a6604c5b52174dbb1e2fcb929ceacacfbce89539adb554d4d95f199d008e278` |
| Companion SHA-256 | `e3de8af3b5515e1e7b5f58669f9fe44282fb859b28a5f6c6e0c59cc9a683152a` |
| Device identifier suffix | `966a93` |
| Test start, local time + zone | `2026-08-18 10:23 CST` |
| Test end, local time + zone | `2026-08-18 11:17 CST` |
| Operator | `Codex automation + device owner visual/physical confirmation` |

Keep the full CoreBluetooth identifier only in the current shell:

```sh
DEVICE_ID='<full UUID; do not paste into this document>'
COMPANION='./companion/.build/release/codex-watch-companion'
```

## Fresh build and evidence setup

Start from the repository root with no unrelated changes:

```sh
git status --short
git rev-parse HEAD
sw_vers
swift --version
./.venv/bin/platformio --version
./.venv/bin/platformio test -e native
./.venv/bin/platformio run -e m5stack-stopwatch
swift build --package-path companion -c release
companion/.build/release/VibeWatchCompanionCoreVerification
./.venv/bin/pytest -q
git diff --check
shasum -a 256 .pio/build/m5stack-stopwatch/firmware.bin
shasum -a 256 companion/.build/release/codex-watch-companion
```

Upload only the just-hashed firmware:

```sh
./.venv/bin/platformio run -e m5stack-stopwatch --target upload
```

Open a serial monitor in a second terminal and retain the raw transcript only
in a private temporary location. Copy only short sanitized excerpts into the
evidence fields below. Firmware emits bounded, identifier-free samples in this
format at boot and after each v2 terminal state:

```text
VW_HEAP_DIAG sample=1 event=boot free_bytes=<bytes>
```

```sh
./.venv/bin/platformio device monitor -e m5stack-stopwatch
```

## Operator commands

### Demo discovery — synthetic data only

This is the sole broad-discovery mode. It selects a nearby matching device and
writes a synthetic quota snapshot; it must never be used as evidence of a real
account quota. With `--verbose`, record the selected CoreBluetooth UUID from
stderr in the private `DEVICE_ID` shell variable above, then record only its
last six characters in this document.

```sh
"$COMPANION" --demo --verbose
```

### Pinned quota routing

```sh
"$COMPANION" --auto --card codex --device-id "$DEVICE_ID" --verbose
"$COMPANION" --remaining 50 --reset 3600 --card workbuddy \
  --credits 500 --total-credits 1000 --device-id "$DEVICE_ID" --verbose
```

Stop all quota writers and wait more than 180 seconds to verify the stale
presentation.

### Correlated v2 approve and reject

Use the right screen half for the first request and the left physical button
for the second. Each command must print exactly one decision with the same ID.

```sh
"$COMPANION" --approval \
  --request-id 550e8400-e29b-41d4-a716-446655440000 \
  --card codex --agent-id 0 --type WRITE --summary 'Approve by touch' \
  --ttl-ms 30000 --device-id "$DEVICE_ID" --verbose

"$COMPANION" --approval \
  --request-id 7d444840-9dc0-11d1-b245-5ffdce74fad2 \
  --card workbuddy --agent-id 5 --type EXEC --summary 'Reject by button' \
  --ttl-ms 30000 --device-id "$DEVICE_ID" --verbose
```

### Duplicate and concurrent busy

In terminal A, start the first command and leave its modal open:

```sh
"$COMPANION" --approval \
  --request-id aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee \
  --card codex --agent-id 1 --type EXEC --summary 'Original pending request' \
  --ttl-ms 120000 --device-id "$DEVICE_ID" --verbose
```

While it remains open, run the same command in terminal B and leave that retry
waiting. It must not alert a second time or replay a decision. With both A and
B still pending, run this different ID in terminal C:

```sh
"$COMPANION" --approval \
  --request-id bbbbbbbb-cccc-4ddd-8eee-ffffffffffff \
  --card antigravity --agent-id 2 --type WRITE --summary 'Must receive busy' \
  --ttl-ms 30000 --device-id "$DEVICE_ID" --verbose
```

Terminal C must return a v2 `busy` error for the different ID and must not
replace the first modal. After recording that result, interrupt terminal B with
Control-C; it is expected to have received neither a decision nor a second
alert. Finally, reject the original modal and verify that only terminal A
receives its one matching decision. Leaving B uninterrupted is also valid, but
it must end only with the documented transport timeout, never a replay.

### Short TTL expiry

Do not touch the watch:

```sh
"$COMPANION" --approval \
  --request-id cccccccc-dddd-4eee-8fff-000000000000 \
  --card codex --agent-id 3 --type EXEC --summary 'Allow this to expire' \
  --ttl-ms 5000 --device-id "$DEVICE_ID" --verbose
```

Expected: one matching `expired` decision, no approval HID action, and the
modal closes.

### Disconnect cancellation

Start the request, then disconnect or power off the watch while its modal is
open:

```sh
"$COMPANION" --approval \
  --request-id dddddddd-eeee-4fff-8000-111111111111 \
  --card codex --agent-id 4 --type EXEC --summary 'Disconnect while pending' \
  --ttl-ms 120000 --device-id "$DEVICE_ID" --verbose
```

Expected: the firmware cancels the pending request and clears the modal; the
host reports a disconnect/`transport_error` because the dead link cannot carry
the cancellation indication. After reconnect, there must be no stale decision
or restored modal for this ID.

### Explicit one-release legacy adapter

First send a normal v2 request and verify that the host receives `.04` JSON and
no legacy `ACT07`/`ACT08`. Then run the compatibility command:

```sh
"$COMPANION" --approval --legacy-approval \
  --card codex --type EXEC --summary 'Explicit legacy compatibility check' \
  --device-id "$DEVICE_ID" --verbose
```

Expected: legacy mode exists only with `--legacy-approval`, exits after the
`.03` ATT acknowledgement, and a later watch decision emits only the legacy
HID action. It must not produce a v2 decision indication.

### 100-cycle soak

Record the boot `VW_HEAP_DIAG` free-heap value from the serial monitor. The soak
denominator is exactly 100 terminal protocol-v2 CLI attempts, each with a new
`request_id` and the same pinned `DEVICE_ID`. For every non-disconnect cycle,
approve odd cycle numbers and reject even cycle numbers. At cycles 25, 50, 75,
and 100, disconnect while the modal is pending instead of deciding, wait for
the expected host `transport_error`, reconnect the same pinned device, and
continue. Full request UUIDs stay in the temporary transcript only.

```sh
for CYCLE in $(seq 1 100); do
  REQUEST_ID=$(uuidgen | tr '[:upper:]' '[:lower:]')
  if [ $((CYCLE % 25)) -eq 0 ]; then
    EXPECTED='disconnect -> transport_error'
  elif [ $((CYCLE % 2)) -eq 1 ]; then
    EXPECTED='approve'
  else
    EXPECTED='reject'
  fi
  printf 'cycle=%03d expected=%s\n' "$CYCLE" "$EXPECTED"
  "$COMPANION" --approval --request-id "$REQUEST_ID" \
    --card codex --agent-id "$(( (CYCLE - 1) % 6 ))" \
    --type SOAK --summary "Approval soak cycle $CYCLE" \
    --ttl-ms 120000 --device-id "$DEVICE_ID" --verbose
done 2>&1 | tee /tmp/vibewatch-approval-soak.txt
```

Record the final `VW_HEAP_DIAG` sample after attempt 100 terminates. The exact
PASS count is 48 delivered `approve` decisions + 48 delivered `reject`
decisions + 4 expected disconnect `transport_error` failures = 100 terminal v2
attempts, with 0 unexpected failures. PASS also requires no incorrect request
correlation, replay, stuck modal, crash, queue growth, or material monotonic
free-heap loss. Delete or securely retain the unsanitized temporary transcript
according to local policy; do not commit it.

## Thirteen required acceptance cases

### 1. Fresh boot starts unsynchronized

- Result: `PASS`
- Evidence: `After the release image was uploaded to the explicitly selected USB port and the device was reset without a host sync, the operator visually confirmed that all three cards showed waiting/not-synced and that no 86/85/78 sample quota or sample credits appeared.`
- Sanitized excerpt/photo reference: `Operator visual confirmation at 2026-08-18 10:28 CST; no photo retained.`

### 2. Unpaired writes cannot mutate state

Forget the bond through macOS Bluetooth settings and use the supported watch
pairing/settings flow. Attempt both a pinned quota write and a pinned approval
write before pairing again. Cancel any new macOS pairing prompt during these
negative attempts; accepting it would create a new encrypted/bonded session
rather than test an unpaired write.

- Result: `PASS`
- Evidence: `After forgetting the device, two clean pinned negative attempts were run separately while the operator cancelled every pairing prompt and did not touch the watch. Both the .02 quota write and the .03 approval write exited 1 with ATT security rejection; the operator confirmed no target quota/card mutation, approval modal, sound, or vibration. A prior attempt in which pairing was accepted was discarded as invalid evidence.`
- Sanitized excerpt: `Both clean attempts: ATT write failed with CBATTErrorDomain code 15. The Workbuddy sentinel values 17% and 123/999 never appeared. A pre-existing Codex synthetic demo value was explicitly excluded from this negative target check.`

### 3. Pairing and pinned-device selection succeed

- Result: `PASS`
- Evidence: `The operator paired Vibe Watch #1 through the normal macOS Bluetooth UI. Subsequent real quota and manual-credit writes used the same pinned CoreBluetooth identifier discovered in demo mode and completed with ATT responses.`
- Device suffix only: `966a93`

### 4. Real quota and credits route to the requested card

- Result: `PASS`
- Evidence: `Pinned App Server writes populated only the Codex card (6% in the first run; 2% after the later reboot) and pinned manual writes populated only Workbuddy with 50% and 500/1000 credits. Antigravity remained waiting/not-synced and no value crossed cards. After all writers stopped for more than 180 seconds, the operator confirmed Codex and Workbuddy were visibly marked stale while retaining their last values, and Antigravity remained waiting.`

### 5. Approve and reject correlate exactly once

- Result: `PASS`
- Evidence: `Touch approval produced exactly one stdout decision for request suffix 440000 with decision approve and exit 0. Left-button rejection produced exactly one stdout decision for request suffix 74fad2 with decision reject and exit 0. No duplicate or stale object followed either result.`

### 6. Duplicate IDs do not replay alerts

- Result: `PASS`
- Evidence: `Two clients sent the same pending request suffix eeeeee. The operator observed one original modal and no second alert. The duplicate client was interrupted with no decision output; only the original client later received one matching reject decision.`

### 7. A concurrent different request receives `busy`

- Result: `PASS`
- Evidence: `While the original request was pending, a different request suffix ffffff exited 1 with code busy and message another request is pending. The operator confirmed the original modal was not replaced.`

### 8. Expired requests close without approval

- Result: `PASS`
- Evidence: `A 5000 ms request ending 000000 produced exactly one matching expired decision after about five seconds and exited 0. The operator confirmed the modal closed automatically with no approval action, sound, or vibration.`

### 9. Disconnect cancels without stale replay

- Result: `PASS`
- Evidence: `Bluetooth was disabled while request suffix 111111 was pending. The host exited 1 with transport_error and message Peripheral disconnected before completion. After Bluetooth was restored, the operator confirmed the old modal was absent, no stale decision appeared, and normal cards returned.`

### 10. Existing watch interactions still work

- Result: `PASS`
- Evidence: `The operator confirmed screen sleep and touch wake, touch approval, physical-button rejection, horizontal card gestures, user-observed center PTT feedback after a one-second hold/release, sound, vibration, and visible battery status. The contemporaneous serial slice did not contain ACT10/ACT11, so no serial PTT claim is made. Workbuddy and Antigravity each showed local selection animation, sound, and vibration; no matching external host consumers were running for those two card names.`

The PTT check covers the HID control event only; this firmware does not stream
microphone audio.

### 11. Legacy behavior is explicit and isolated

- Result: `PASS`
- Evidence: `A normal v2 request ended with one correlated expired decision; serial showed approval accepted and a v2 expired heap diagnostic, with no ACT07/ACT08. A request explicitly using --legacy-approval exited 0 immediately after the .03 ATT acknowledgement; the later operator decision emitted only ACT08 DOWN/UP on serial, with no v2 decision or v2 terminal heap event. The compatibility path is documented as deprecated for removal after v1.01.`

### 12. Disconnect/reconnect and 100-cycle soak remain stable

- Result: `BLOCKED — NOT RUN at the user's explicit request`
- Start free heap: `242476 bytes at boot sample 1, captured 2026-08-18 10:58 CST`
- End free heap: `Not captured; the required 100-cycle soak was not started`
- Outcome counts: `Not applicable; no 100-cycle denominator or 48/48/4 result is claimed`
- Evidence: `Single-request lifecycle diagnostics were present and sanitized, including sample 2 expired at 242316 bytes. This does not substitute for soak evidence. Stability, queue growth, and monotonic heap retention across 100 attempts remain unverified.`

### 13. Agent labels are 2× and controls are unaffected

Inspect all six circles on each of the three cards in default, selected,
pressed, and animated states.

- Result: `PASS`
- Evidence: `The operator inspected labels 1–6 on Codex, Workbuddy, and Antigravity in default, selected, pressed, and animated states and confirmed they are approximately twice the former character height, centered, fully inside their circles, and clearly contrasted.`
- Action-layer evidence: `The operator confirmed FAST/OK/NG/PLAN/AI typography and touch targets were unchanged. Circle 1 on all three cards produced local selection animation, sound, and vibration. Workbuddy and Antigravity had no external host-side effect because no corresponding host consumers were running; their watch-side controls remained functional and emitted through the same shared firmware path.`
- Photo/video references: `Operator visual and interaction confirmation; no media retained.`

## Final result

| Gate | Result |
| --- | --- |
| All 13 hardware cases complete and passing | `BLOCKED — case 12 soak not run by user request` |
| Automated gate rerun after hardware testing | `BLOCKED — Python 42/42, firmware core 27/27, shared companion 37 groups, release builds and hygiene passed; local swift test cannot run because this Command Line Tools host has no XCTest runtime` |
| `git status --short` contains only this evidence edit | `PASS before evidence commit` |
| Full UUIDs and raw logs absent from tracked files | `PASS` |

Release decision and notes: `BLOCKED for release: the user explicitly skipped the required 100-cycle soak, local XCTest requires full Xcode or macOS CI, and GitHub Actions status must be confirmed. Evidence recorded by Codex automation with device-owner confirmation on 2026-08-18 CST.`

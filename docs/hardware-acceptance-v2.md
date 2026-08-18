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
| Firmware source commit | `<git rev-parse HEAD>` |
| Companion source commit | `<git rev-parse HEAD>` |
| Firmware version / protocol | `v1.01 / 2` |
| macOS version/build | `<sw_vers output>` |
| Swift version | `<swift --version>` |
| PlatformIO version | `<./.venv/bin/platformio --version>` |
| Firmware SHA-256 | `<sha256>` |
| Companion SHA-256 | `<sha256>` |
| Device identifier suffix | `<last 6 characters only>` |
| Test start, local time + zone | `<YYYY-MM-DD HH:MM TZ>` |
| Test end, local time + zone | `<YYYY-MM-DD HH:MM TZ>` |
| Operator | `<initials>` |

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

While it remains open, run the same command in terminal B. It must not alert a
second time or replay a decision to the retry. Then, still before deciding the
original, run this different ID in terminal B:

```sh
"$COMPANION" --approval \
  --request-id bbbbbbbb-cccc-4ddd-8eee-ffffffffffff \
  --card antigravity --agent-id 2 --type WRITE --summary 'Must receive busy' \
  --ttl-ms 30000 --device-id "$DEVICE_ID" --verbose
```

The second ID must return a v2 `busy` error and must not replace the first
modal. Reject the original to finish terminal A. A retry process that receives
no replay may end with its documented transport timeout.

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

Record the boot `VW_HEAP_DIAG` free-heap value from the serial monitor. Then run 100
unique approvals, alternating approve and reject on the watch. At cycles 25,
50, 75, and 100, disconnect while the modal is pending, reconnect, and continue
with a new UUID. Full UUIDs stay in the temporary transcript only.

```sh
for CYCLE in $(seq 1 100); do
  REQUEST_ID=$(uuidgen | tr '[:upper:]' '[:lower:]')
  "$COMPANION" --approval --request-id "$REQUEST_ID" \
    --card codex --agent-id "$(( (CYCLE - 1) % 6 ))" \
    --type SOAK --summary "Approval soak cycle $CYCLE" \
    --ttl-ms 120000 --device-id "$DEVICE_ID" --verbose
done 2>&1 | tee /tmp/vibewatch-approval-soak.txt
```

Record the final `VW_HEAP_DIAG` sample after the last terminal outcome. PASS requires 100 terminal outcomes,
no incorrect request correlation, no lost non-disconnect result, no replay,
no stuck modal, no crash, no queue growth, and no material monotonic free-heap
loss. Delete or securely retain the unsanitized temporary transcript according
to local policy; do not commit it.

## Thirteen required acceptance cases

### 1. Fresh boot starts unsynchronized

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<all three cards show waiting/not-synced; no sample 86/85/78 or credits>`
- Sanitized excerpt/photo reference: `<reference>`

### 2. Unpaired writes cannot mutate state

Forget the bond through macOS Bluetooth settings and use the supported watch
pairing/settings flow. Attempt both a pinned quota write and a pinned approval
write before pairing again. Cancel any new macOS pairing prompt during these
negative attempts; accepting it would create a new encrypted/bonded session
rather than test an unpaired write.

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<ATT security failure; no quota change, modal, sound, or vibration>`
- Sanitized excerpt: `<reference>`

### 3. Pairing and pinned-device selection succeed

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<normal UI pairing; selected UUID suffix matches later real writes>`
- Device suffix only: `<6 chars>`

### 4. Real quota and credits route to the requested card

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<Codex quota and Workbuddy credits appear only on requested cards; stale state after >180 s>`

### 5. Approve and reject correlate exactly once

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<touch approve ID/decision; button reject ID/decision; one stdout object each>`

### 6. Duplicate IDs do not replay alerts

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<one modal/alert for duplicate ID; original result only>`

### 7. A concurrent different request receives `busy`

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<second ID and exact busy JSON; original modal unchanged>`

### 8. Expired requests close without approval

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<matching expired JSON; modal closed; no approval HID action>`

### 9. Disconnect cancels without stale replay

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<modal closed; host transport failure; no result/modal for old ID after reconnect>`

### 10. Existing watch interactions still work

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<sleep/wake, touch and physical decisions, card gestures, PTT control event, sound, vibration, battery display>`

The PTT check covers the HID control event only; this firmware does not stream
microphone audio.

### 11. Legacy behavior is explicit and isolated

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<v2 emits no ACT07/ACT08; legacy requires flag and emits only legacy action>`

### 12. Disconnect/reconnect and 100-cycle soak remain stable

- Result: `<PASS|FAIL|BLOCKED>`
- Start free heap: `<bytes and sanitized serial timestamp>`
- End free heap: `<bytes and sanitized serial timestamp>`
- Outcome counts: `<approve / reject / expected disconnect / unexpected failure>`
- Evidence: `<no queue leak, material heap degradation, crash, or stuck modal>`

### 13. Agent labels are 2× and controls are unaffected

Inspect all six circles on each of the three cards in default, selected,
pressed, and animated states.

- Result: `<PASS|FAIL|BLOCKED>`
- Evidence: `<labels 1–6 about twice former character height, centered, fully inside circles, adequate contrast>`
- Action-layer evidence: `<FAST/OK/NG/PLAN/AI typography and touch targets unchanged>`
- Photo/video references: `<references>`

## Final result

| Gate | Result |
| --- | --- |
| All 13 hardware cases complete and passing | `<PASS|FAIL|BLOCKED>` |
| Automated gate rerun after hardware testing | `<PASS|FAIL|BLOCKED>` |
| `git status --short` contains only this evidence edit | `<PASS|FAIL|BLOCKED>` |
| Full UUIDs and raw logs absent from tracked files | `<PASS|FAIL|BLOCKED>` |

Release decision and notes: `<decision, approver, timestamp>`

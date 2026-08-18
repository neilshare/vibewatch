# macOS VibeWatch companion

The Swift companion reads the current Codex rate-limit window from a local
Codex App Server, writes quota snapshots, and performs correlated approval
transactions with one explicitly bound StopWatch over the project's private
BLE service.

It uses the user's existing local Codex/ChatGPT sign-in context. It does not
scrape the UI, use a cloud relay, read credentials directly, require an OpenAI
API key, or put an account token on the watch.

## Build from source

Requirements:

- macOS 13 or newer;
- Swift 5.10 or newer (Xcode 15.3 Command Line Tools or newer);
- a locally available Codex executable or an explicit `--codex-path`.

```sh
swift build -c release
```

No prebuilt companion binary is distributed by this project.

## Bind a StopWatch safely

After pairing through the watch settings UI and macOS Bluetooth settings, run
demo discovery. This is the only broad-discovery mode: it selects a nearby
matching device and writes synthetic quota data. With `--verbose`, the selected
CoreBluetooth UUID assigned by this Mac is reported on stderr:

```sh
.build/release/codex-watch-companion --demo --verbose
```

Keep that full UUID private and use it for every real write:

```sh
.build/release/codex-watch-companion \
  --auto --device-id YOUR_COREBLUETOOTH_UUID --verbose
```

For continuous refreshes while the terminal remains open:

```sh
.build/release/codex-watch-companion \
  --device-id YOUR_COREBLUETOOTH_UUID --watch --interval 60
```

Keep this `--watch` process running whenever automatic startup is not installed;
otherwise the dashboard will correctly mark quota sync stale.

Pass `--codex-path /absolute/path/to/codex` when automatic executable discovery
does not select the intended local Codex installation.

Useful diagnostics:

```sh
# Verify App Server parsing without using Bluetooth.
.build/release/codex-watch-companion --json-only

# Broadly discover a nearby watch and write a synthetic snapshot.
.build/release/codex-watch-companion --demo --verbose
```

Demo output is never evidence of a real account quota. Non-demo operations
retrieve or scan only for the exact `--device-id`; they do not authorize a
peripheral by its display name.

## Manual quota and credits

```sh
.build/release/codex-watch-companion \
  --remaining 50 --reset 3600 --card codex \
  --device-id YOUR_COREBLUETOOTH_UUID

.build/release/codex-watch-companion \
  --remaining 83.3 --reset 0 --card workbuddy \
  --credits 1250 --total-credits 1500 \
  --device-id YOUR_COREBLUETOOTH_UUID
```

Manual quota requires both `--remaining` (`0...100`) and nonnegative `--reset`
seconds. Credit fields must be supplied together, with nonnegative credits and
a positive total. The watch preserves its prior valid snapshot when a payload
is invalid and marks valid data stale after 180 seconds without refresh.

## Transactional approvals

Protocol v2 subscribes to the approval-result indication before sending a
write-with-response request. The result is accepted only when its canonical
request UUID matches exactly:

```sh
.build/release/codex-watch-companion --approval \
  --request-id 550e8400-e29b-41d4-a716-446655440000 \
  --card codex --agent-id 0 --type WRITE \
  --summary 'Update the selected file' --ttl-ms 30000 \
  --device-id YOUR_COREBLUETOOTH_UUID --verbose
```

If `--request-id` is omitted, the companion creates one. `--agent-id` is
`0...5`; type and summary limits are 23 and 95 UTF-8 bytes; TTL is
`5000...120000` milliseconds. Approve, reject, expired, and delivered cancelled
outcomes print one sorted-key JSON object and exit `0`. Inspect `decision`—exit
`0` does not itself mean approval. Protocol/transport failures exit `1`, and
usage failures exit `2`. Machine output is on stdout; human progress is on
stderr.

For one v1.01 compatibility cycle only, an old integration may opt in with:

```sh
.build/release/codex-watch-companion --approval --legacy-approval \
  --card codex --type EXEC --summary 'Legacy compatibility check' \
  --device-id YOUR_COREBLUETOOTH_UUID
```

There is no automatic downgrade. Legacy mode ends after the encrypted `.03`
ATT acknowledgement and produces only the legacy HID action after the watch
decision; it does not also produce a v2 result. The adapter is scheduled for
removal in the next release.

## Optional local background installation

For unattended CoreBluetooth access, Codex can wrap the locally built binary in
a small background `.app` using [`app/Info.plist`](app/Info.plist), ad-hoc sign
that local wrapper, and create a per-user LaunchAgent from
[`launchd/io.github.codex-micro-stopwatch.companion.plist.example`](launchd/io.github.codex-micro-stopwatch.companion.plist.example).

This is a local installation step, not a distributed package. The generated app
belongs under `companion/.local/` or another local application directory. The
generated LaunchAgent belongs under the user's `~/Library/LaunchAgents/`.

Before loading the LaunchAgent, replace every placeholder with an absolute local
value:

- `__EXECUTABLE_PATH__`: executable inside the locally built app wrapper;
- `__CODEX_PATH__`: intended local Codex executable;
- `__DEVICE_UUID__`: UUID printed by demo discovery on this Mac;
- `__LOG_DIRECTORY__`: a private local log directory.

Codex should lint the generated plist, verify the ad-hoc-signed app with
`codesign --verify --deep --strict`, load it only after user approval, and use
`launchctl print gui/$UID/io.github.codex-micro-stopwatch.companion` plus one
real quota/reset update as the health check. To uninstall, boot out that exact
per-user LaunchAgent before removing only the generated local app, plist, and
logs; never delete or edit the tracked templates.

Do not edit the tracked example in place. Never commit the generated plist,
app, UUID, usernames, home paths, or logs. The root README contains the
recommended prompt for asking Codex to perform and verify this installation.

The first Bluetooth access may trigger a macOS permission prompt. Demo scans by
the private service or supported device names. Real writes require the exact
bound CoreBluetooth UUID, so another same-name peripheral is ignored.

## Data boundary

The complete schemas, UUIDs, permissions, decision semantics, errors, and
migration schedule are in
[`docs/COMPANION_PROTOCOL.md`](../docs/COMPANION_PROTOCOL.md). Agent status,
button events, push-to-talk control, and analog directions stay on the BLE HID
channel. This firmware and companion do not capture or stream audio.

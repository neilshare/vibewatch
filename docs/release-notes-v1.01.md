# VibeWatch v1.01

VibeWatch v1.01 is a security and reliability release for the M5Stack
StopWatch firmware and macOS companion. The compatible pair reports firmware
`v1.01` and companion protocol `2`.

## Highlights

- Transactional approval protocol v2 correlates every approve, reject, expiry,
  and cancellation outcome with a canonical request UUID.
- The private BLE service now separates encrypted quota writes, bonded
  approval requests, and indicated approval results.
- Supported host tools pin every real write to an operator-selected
  CoreBluetooth UUID. Broad name/service discovery is isolated to synthetic
  demo mode.
- Approval clients subscribe to results before writing, use ATT writes with
  response, and reject mismatched or malformed indications.
- Bounded ingress queues, main-loop JSON processing, connection generations,
  indication timeouts, and disconnect cleanup remove callback-thread and stale
  message hazards.
- Startup and stale quota displays are truthful; the watch no longer presents
  fabricated quota or credit values as live data.
- The six outer agent labels `1` through `6` render at twice their former
  character scale while action-layer typography remains unchanged.
- The Swift companion core and Python compatibility CLI now have deterministic
  request validation, machine-readable outcomes, and real failure propagation.

## Security compatibility change

Approval requests no longer share the quota or HID JSON-RPC ingress path. New
integrations must use protocol v2 characteristics `.03` and `.04`, preserve the
request UUID, and bind real operations with `--device-id`. This prevents a
same-name peripheral, stale decision, or unrelated response from being treated
as authorization.

The v1 approval shape is available for this release cycle only when the host is
started with the explicit `--legacy-approval` flag. It is accepted only on the
encrypted, bonded approval-request characteristic and never selected as an
automatic fallback. A legacy watch decision produces only its legacy HID
action; a v2 decision never produces that action as well.

The legacy adapter is scheduled for removal in the release after v1.01.

## Operator action

1. Build and flash the v1.01 firmware.
2. Pair through the watch settings UI.
3. Run synthetic demo discovery to identify the intended device, then retain
   its CoreBluetooth UUID privately.
4. Supply that exact UUID with `--device-id` for every real quota or approval
   command.
5. Update integrations to protocol v2 before the next release.

See the [protocol reference](COMPANION_PROTOCOL.md),
[companion guide](../companion/README.md), and
[hardware acceptance record](hardware-acceptance-v2.md).

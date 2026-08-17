# VibeWatch Security and Reliability Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make VibeWatch's BLE approval path encrypted, bonded, transactional, main-loop-owned, and fully tested while correcting telemetry truthfulness, host CLI behavior, CI coverage, documentation drift, and the unreadable `1`–`6` agent labels.

**Architecture:** A small platform-independent `vibe_core` library owns approval validation and state transitions. NimBLE callbacks become bounded queue producers; the Arduino main loop becomes the only owner of UI-visible state and hardware calls. The Swift companion is split into a testable library and executable, while Python becomes a thin, validated compatibility wrapper over the same protocol contract.

**Tech Stack:** ESP32-S3 Arduino, M5Unified, NimBLE-Arduino 2.5.1, ArduinoJson 7.4.3, FreeRTOS queues, C++17 host-native tests with Unity/PlatformIO, Swift 5.10/CoreBluetooth/XCTest, Python 3.11+/pytest, GitHub Actions on Ubuntu and macOS.

## Global Constraints

- Preserve the three-card UI, gestures, non-approval HID report values, microphone behavior, sound, haptics, power management, and persisted settings.
- Protocol v2 is authoritative; legacy approval is available for one release cycle only through explicit `--legacy-approval` selection.
- Real quota and approval writes require an exact `--device-id`; name-based discovery is demo-only.
- Use encrypted GATT permissions and verify `isEncrypted()` plus `isBonded()` in approval callbacks; do not use `WRITE_AUTHEN` with the current `NO_INPUT_OUTPUT` pairing model.
- NimBLE callbacks must not parse JSON, mutate UI state, change CPU frequency, draw, play sound, or vibrate.
- A v2 request carries a canonical UUID, card, agent ID, operation type, summary, and TTL; a decision must echo the same UUID exactly once.
- At most one approval is pending; same-ID retry is idempotent and a different ID returns `busy` without replacing the modal.
- Quota starts unavailable and failed synchronization never fabricates a percentage or credit balance.
- Agent labels `1` through `6` use `Orbitron_Light_32` at text scale `2.0`, remain centered, and do not change hit targets or action-layer typography.
- Every production change follows red-green-refactor where a host test is possible; every task ends with focused tests and a dedicated commit.

---

## File Structure

### New firmware/core files

- `lib/vibe_core/library.json` — declares the platform-independent library and ArduinoJson dependency.
- `lib/vibe_core/src/vibe_approval.h` — fixed-size approval types, limits, result enums, and controller interface.
- `lib/vibe_core/src/vibe_approval.cpp` — validation-independent approval state machine.
- `lib/vibe_core/src/vibe_protocol.h` — protocol v2 JSON decode/encode interface and stable error codes.
- `lib/vibe_core/src/vibe_protocol.cpp` — ArduinoJson implementation shared by ESP32 and native tests.
- `lib/vibe_core/src/vibe_quota.h` — quota freshness model with unavailable/fresh/stale states.
- `lib/vibe_core/src/vibe_quota.cpp` — quota validation and elapsed-time calculation.
- `lib/vibe_core/src/vibe_ingress_core.h` — portable fixed-size ingress buffer and connection-scoped assembly interface.
- `lib/vibe_core/src/vibe_ingress_core.cpp` — portable ingress capacity and reassembly implementation.
- `include/vibe_ingress.h` — separate fixed-size FreeRTOS control-message and HID-chunk queue declarations.
- `src/vibe_ingress.cpp` — queue creation, push/pop, NimBLE callback adapters, and main-loop-owned connection-scoped HID assembly.
- `test/test_vibe_core/test_main.cpp` — native approval, protocol, quota, and UTF-8 tests.

### Swift files

- `companion/Sources/VibeWatchCompanionCore/ProtocolModels.swift` — Codable quota/approval/error models.
- `companion/Sources/VibeWatchCompanionCore/Options.swift` — parsed and validated CLI modes.
- `companion/Sources/VibeWatchCompanionCore/AppServerClient.swift` — Codex rate-limit process client.
- `companion/Sources/VibeWatchCompanionCore/BLETransport.swift` — transport protocol, CoreBluetooth implementation, and device pinning.
- `companion/Sources/VibeWatchCompanionCore/Runner.swift` — dependency-injected command execution and result formatting.
- `companion/Sources/CodexWatchCompanion/main.swift` — executable entry point only.
- `companion/Tests/VibeWatchCompanionCoreTests/*.swift` — XCTest fixtures, fakes, CLI tests, and approval result tests.

### Python, CI, and documentation

- `scripts/sync_watch.py` — normalized request parsing and explicit Swift/Bleak backends.
- `tests/test_sync_watch.py` — actual production-function tests with mocks.
- `.github/workflows/firmware.yml` — Linux firmware, native C++, Python, hygiene, and artifact gating.
- `.github/workflows/companion.yml` — macOS Swift build/test job.
- `docs/COMPANION_PROTOCOL.md` — authoritative UUID, permissions, schemas, errors, and migration reference.
- `docs/hardware-acceptance-v2.md` — reproducible real-device test record.
- `README.md`, `README.zh-CN.md`, `companion/README.md`, `docs/release-notes-v1.01.md` — corrected user and release guidance.

---

### Task 1: Add the platform-independent approval and quota core

**Files:**
- Create: `lib/vibe_core/library.json`
- Create: `lib/vibe_core/src/vibe_approval.h`
- Create: `lib/vibe_core/src/vibe_approval.cpp`
- Create: `lib/vibe_core/src/vibe_protocol.h`
- Create: `lib/vibe_core/src/vibe_protocol.cpp`
- Create: `lib/vibe_core/src/vibe_quota.h`
- Create: `lib/vibe_core/src/vibe_quota.cpp`
- Create: `test/test_vibe_core/test_main.cpp`
- Modify: `platformio.ini`

**Interfaces:**
- Produces: `vibe::ApprovalRequestV2`, `vibe::ApprovalDecisionV2`, `vibe::ApprovalController`, `vibe::decodeApprovalRequest()`, `vibe::encodeApprovalDecision()`, `vibe::QuotaSnapshot::apply()`.
- Consumes: ArduinoJson 7.4.3 only in `vibe_protocol.cpp`; all state-machine and quota types remain standard C++.

- [ ] **Step 1: Add a native PlatformIO test environment and library manifest**

Append this exact environment to `platformio.ini`:

```ini
[env:native]
platform = native
test_framework = unity
test_build_src = no
build_flags =
    -std=gnu++17
lib_deps =
    bblanchon/ArduinoJson @ 7.4.3
```

Create `lib/vibe_core/library.json`:

```json
{
  "name": "vibe_core",
  "version": "1.1.0",
  "build": { "flags": "-std=gnu++17" },
  "dependencies": { "bblanchon/ArduinoJson": "7.4.3" },
  "frameworks": "*",
  "platforms": "*"
}
```

- [ ] **Step 2: Write failing approval state-machine tests**

Create `test/test_vibe_core/test_main.cpp` with Unity tests that construct a valid request and assert these exact transitions:

```cpp
#include <unity.h>
#include "vibe_approval.h"
#include "vibe_protocol.h"
#include "vibe_quota.h"

using namespace vibe;

static ApprovalRequestV2 request(const char* id, std::uint32_t ttl = 30000) {
    ApprovalRequestV2 value{};
    std::strncpy(value.requestId, id, sizeof(value.requestId) - 1);
    value.card = AgentCardId::Codex;
    value.agentId = 0;
    std::strcpy(value.operationType, "EXEC");
    std::strcpy(value.summary, "Run firmware tests");
    value.ttlMs = ttl;
    return value;
}

void test_accept_duplicate_busy_and_decide() {
    ApprovalController controller;
    const auto first = request("550e8400-e29b-41d4-a716-446655440000");
    const auto second = request("d9428888-122b-11e1-b85c-61cd3cbb3210");
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalAcceptResult::Accepted),
                            static_cast<std::uint8_t>(controller.accept(first, 100)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalAcceptResult::Duplicate),
                            static_cast<std::uint8_t>(controller.accept(first, 120)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalAcceptResult::Busy),
                            static_cast<std::uint8_t>(controller.accept(second, 130)));
    const auto decision = controller.decide(ApprovalChoice::Approve, 200);
    TEST_ASSERT_TRUE(decision.hasValue);
    TEST_ASSERT_EQUAL_STRING(first.requestId, decision.value.requestId);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalChoice::Approve),
                            static_cast<std::uint8_t>(decision.value.choice));
    TEST_ASSERT_FALSE(controller.decide(ApprovalChoice::Reject, 201).hasValue);
}

void test_expiry_and_disconnect_are_not_approval() {
    ApprovalController controller;
    const auto value = request("550e8400-e29b-41d4-a716-446655440000", 5000);
    controller.accept(value, 1000);
    const auto expired = controller.expireIfNeeded(6000);
    TEST_ASSERT_TRUE(expired.hasValue);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalChoice::Expired),
                            static_cast<std::uint8_t>(expired.value.choice));
    controller.accept(value, 7000);
    const auto cancelled = controller.cancel(7100);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalChoice::Cancelled),
                            static_cast<std::uint8_t>(cancelled.value.choice));
}
```

Register every test in `setup()` and leave `loop()` empty for native Unity.

- [ ] **Step 3: Run the tests and verify they fail because the interfaces do not exist**

Run: `./.venv/bin/platformio test -e native`

Expected: FAIL with a missing `vibe_approval.h` or undefined `ApprovalController` error.

- [ ] **Step 4: Implement fixed-size types and the state machine**

Define these public contracts in `vibe_approval.h`:

```cpp
namespace vibe {
constexpr std::size_t kRequestIdLength = 37;
constexpr std::size_t kOperationTypeLength = 24;
constexpr std::size_t kApprovalSummaryLength = 96;
constexpr std::uint32_t kMinApprovalTtlMs = 5000;
constexpr std::uint32_t kMaxApprovalTtlMs = 120000;

enum class AgentCardId : std::uint8_t { Codex, Workbuddy, Antigravity };
enum class ApprovalChoice : std::uint8_t { Approve, Reject, Expired, Cancelled };
enum class ApprovalAcceptResult : std::uint8_t { Accepted, Duplicate, Busy };

struct ApprovalRequestV2 {
    char requestId[kRequestIdLength]{};
    AgentCardId card{AgentCardId::Codex};
    std::uint8_t agentId{0};
    char operationType[kOperationTypeLength]{};
    char summary[kApprovalSummaryLength]{};
    std::uint32_t ttlMs{0};
};

struct ApprovalDecisionV2 {
    char requestId[kRequestIdLength]{};
    ApprovalChoice choice{ApprovalChoice::Cancelled};
    std::uint32_t decidedAtMs{0};
};

template <typename T> struct OptionalValue { bool hasValue{false}; T value{}; };

class ApprovalController {
  public:
    ApprovalAcceptResult accept(const ApprovalRequestV2&, std::uint32_t nowMs);
    OptionalValue<ApprovalDecisionV2> decide(ApprovalChoice, std::uint32_t nowMs);
    OptionalValue<ApprovalDecisionV2> expireIfNeeded(std::uint32_t nowMs);
    OptionalValue<ApprovalDecisionV2> cancel(std::uint32_t nowMs);
    bool pending() const;
    const ApprovalRequestV2* current() const;
  private:
    bool pending_{false};
    ApprovalRequestV2 current_{};
    std::uint32_t receivedAtMs_{0};
};
}
```

Implement wrap-safe elapsed comparisons with `static_cast<std::uint32_t>(nowMs - receivedAtMs_)`, byte-for-byte request-ID comparison, and exactly-once clearing inside `decide`, `expireIfNeeded`, and `cancel`.

- [ ] **Step 5: Add failing protocol validation and quota truth tests**

Add tests for canonical UUID syntax, allowed cards, agent `0...5`, UTF-8 boundary correctness, operation/summary byte limits, TTL limits, payload limit, decision JSON, default unavailable quota, valid apply, invalid update preservation, and stale transition. Use the canonical JSON examples from the approved design spec verbatim.

Run: `./.venv/bin/platformio test -e native`

Expected: state-machine tests PASS; new protocol/quota tests FAIL because decoders and quota methods are missing.

- [ ] **Step 6: Implement protocol and quota functions minimally**

Expose these signatures:

```cpp
enum class ProtocolError : std::uint8_t {
    None, InvalidPayload, UnsupportedVersion, InvalidRequestId,
    InvalidCard, InvalidAgentId, InvalidOperationType, InvalidSummary, InvalidTtl
};

struct ApprovalDecodeResult {
    ProtocolError error{ProtocolError::InvalidPayload};
    ApprovalRequestV2 request{};
    bool legacy{false};
};

ApprovalDecodeResult decodeApprovalRequest(const std::uint8_t* data, std::size_t length,
                                            bool allowLegacy);
bool encodeApprovalDecision(const ApprovalDecisionV2&, char* output,
                            std::size_t capacity, std::size_t& written);

enum class QuotaFreshness : std::uint8_t { Unavailable, Fresh, Stale };
struct QuotaSnapshot {
    float remainingPercent{0};
    float credits{0};
    float totalCredits{0};
    std::uint32_t resetInSeconds{0};
    std::uint32_t receivedAtMs{0};
    bool available{false};
    bool hasCredits{false};
    bool apply(float remaining, std::uint32_t reset, float creditsValue,
               float totalValue, std::uint32_t nowMs);
    QuotaFreshness freshness(std::uint32_t nowMs, std::uint32_t staleAfterMs) const;
};
```

Reject over-512-byte input before parsing. Decode into a temporary result and mutate no caller state on error. Legacy decoding accepts the current `method/params` shape only when `allowLegacy=true`.

- [ ] **Step 7: Run the native suite and firmware build**

Run:

```bash
./.venv/bin/platformio test -e native
./.venv/bin/platformio run -e m5stack-stopwatch
```

Expected: all native tests PASS and firmware build SUCCESS.

- [ ] **Step 8: Commit the core**

```bash
git add platformio.ini lib/vibe_core test/test_vibe_core
git commit -m "feat: add transactional approval core"
```

---

### Task 2: Secure GATT characteristics and callback-to-main-loop ingress

**Files:**
- Create: `include/vibe_ingress.h`
- Create: `src/vibe_ingress.cpp`
- Create: `lib/vibe_core/src/vibe_ingress_core.h`
- Create: `lib/vibe_core/src/vibe_ingress_core.cpp`
- Modify: `src/main.cpp:100-277,687-873,2432-2458`
- Modify: `include/vibe_hid.h`
- Test: `test/test_vibe_core/test_main.cpp`

**Interfaces:**
- Consumes: `decodeApprovalRequest`, `ApprovalController`, and protocol limits from Task 1.
- Produces: `vibe::IngressMessage`, `initializeIngressQueue()`, `enqueueGattWrite()`, `dequeueIngress()`, secure characteristic UUID constants, and connection-scoped HID assembly.

- [ ] **Step 1: Write failing queue and connection-isolation tests**

Add native tests for a platform-neutral `IngressBuffer<6, 512>` control helper: six pushes succeed, the seventh returns `QueueFull`, FIFO order is preserved, and a 513-byte message returns `PayloadTooLarge`. Add `HidRpcAssembler` tests proving that chunks from connection handles `1` and `2` never share a buffer and that JSON completion is detected only when parsing occurs in the main-loop-facing `consume()` method.

Run: `./.venv/bin/platformio test -e native`

Expected: FAIL because `vibe_ingress_core.h` and `IngressBuffer` do not exist.

- [ ] **Step 2: Define fixed ingress messages without heap allocation**

Create a portable helper in `lib/vibe_core/src/vibe_ingress_core.h/.cpp` and the FreeRTOS adapter in `include/vibe_ingress.h`:

```cpp
enum class IngressKind : std::uint8_t { Quota, Approval, Disconnect };
struct IngressMessage {
    IngressKind kind{IngressKind::Quota};
    std::uint16_t connectionHandle{0};
    std::uint16_t length{0};
    std::array<std::uint8_t, 512> payload{};
};
struct HidChunk {
    std::uint16_t connectionHandle{0};
    std::uint8_t length{0};
    std::array<std::uint8_t, 61> payload{};
};
enum class EnqueueResult : std::uint8_t { Accepted, PayloadTooLarge, QueueFull, Unauthorized };
```

`src/vibe_ingress.cpp` owns a six-entry 512-byte control queue and a twelve-entry 61-byte HID-chunk queue. Callbacks receive queue references but no callback performs `malloc`, `String` concatenation, JSON parsing, message-completion detection, or hardware calls. `HidRpcAssembler` is called only by the Arduino main loop and keeps one bounded 2,048-byte assembly buffer per active connection handle.

- [ ] **Step 3: Replace the mixed writable characteristic with three permission-specific characteristics**

Keep the service UUID and quota UUID, then add:

```cpp
constexpr char kQuotaServiceUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01";
constexpr char kQuotaWriteUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02";
constexpr char kApprovalWriteUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c03";
constexpr char kApprovalResultUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c04";
```

Create characteristics with these exact properties and maximum lengths:

```cpp
quotaService->createCharacteristic(
    kQuotaWriteUuid,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,
    512);
quotaService->createCharacteristic(
    kApprovalWriteUuid,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,
    512);
quotaService->createCharacteristic(
    kApprovalResultUuid,
    NIMBLE_PROPERTY::INDICATE | NIMBLE_PROPERTY::READ_ENC,
    512);
```

Do not include `WRITE_NR` on quota or approval. In the approval callback, require both `connInfo.isEncrypted()` and `connInfo.isBonded()` before copying. In quota, require encryption. A failed bonded-peer check calls `g_server->disconnect(connInfo.getConnHandle())` and enqueues nothing.

- [ ] **Step 4: Route every callback through the queue**

Replace `QuotaCharacteristicCallbacks` and direct `applyApprovalRequest` calls. `onWrite` chooses only a control-message kind, verifies length/security, copies the bytes, and calls `xQueueSend(..., 0)`. The HID callback validates channel/chunk length, copies one raw fragment into `HidChunk`, and enqueues it without trying to identify message completion. Remove `g_rxBuffer` and all callback-side `malloc/free`.

Add a disconnect ingress event before clearing the connection so a matching pending approval can become `cancelled` in the main loop.

- [ ] **Step 5: Process a bounded number of messages in `loop()`**

Drain at most four control messages and eight HID chunks per iteration:

```cpp
vibe::IngressMessage ingress;
for (int processed = 0; processed < 4 && dequeueIngress(ingress); ++processed) {
    processIngressMessage(ingress, millis());
}
vibe::HidChunk chunk;
for (int processed = 0; processed < 8 && dequeueHidChunk(chunk); ++processed) {
    processHidChunkInMainLoop(chunk);
}
```

`processIngressMessage` performs control-message JSON parsing and invokes quota behavior or Task 1's approval controller. An `Approval` message is classified as v2 or legacy only here, never in the callback. `processHidChunkInMainLoop` feeds the connection-scoped assembler and calls `processRpc` only when main-loop parsing reports a complete JSON document. Only these main-loop functions may call `noteActivity`, `playSe`, `vibrate`, or mutate UI state in response to BLE.

- [ ] **Step 6: Send decisions as connection-targeted indications**

Encode `ApprovalDecisionV2` into a fixed 512-byte output buffer and call:

```cpp
g_approvalResult->indicate(
    reinterpret_cast<const std::uint8_t*>(json), written, pendingConnectionHandle);
```

Track `onSubscribe` for the connection handle. Reject a new approval with a protocol error if that peer has not enabled indications. Extend `vibe_protocol` with a tested `encodeProtocolError()` that emits `{"version":2,"kind":"error","request_id":"<canonical UUID or empty string>","code":"<stable code>","message":"<bounded message>"}`. The BLE transport layer, not `ApprovalController`, owns a single `PendingIndication` containing connection handle, fixed payload, payload length, and five-second deadline. Handle `onStatus` to clear this transport state; timeout logs and clears `transport_error` without reopening or approving the domain request.

- [ ] **Step 7: Verify tests and firmware build**

Run:

```bash
./.venv/bin/platformio test -e native
./.venv/bin/platformio run -e m5stack-stopwatch
```

Expected: native queue/isolation tests PASS; firmware build SUCCESS with no undefined NimBLE APIs.

- [ ] **Step 8: Commit the secure ingress**

```bash
git add include/vibe_ingress.h include/vibe_hid.h src/vibe_ingress.cpp src/main.cpp lib/vibe_core test/test_vibe_core
git commit -m "feat: secure BLE approval ingress"
```

---

### Task 3: Integrate approval transactions, truthful telemetry, and larger agent labels

**Files:**
- Modify: `include/vibe_state.h`
- Modify: `src/main.cpp:111-246,1060-1066,1137-1166,1286-1313,1647-1750,2146-2262`
- Test: `test/test_vibe_core/test_main.cpp`

**Interfaces:**
- Consumes: Task 1 controller/quota models and Task 2 ingress/indication path.
- Produces: one authoritative pending modal, correlated approve/reject/expiry/cancel results, unavailable/fresh/stale quota UI, and 2x numeric labels.

- [ ] **Step 1: Add failing tests for telemetry startup and render-state helpers**

Assert a default `QuotaSnapshot` returns `Unavailable`, applying a real snapshot returns `Fresh`, elapsed time above `180000` returns `Stale`, and an invalid update preserves the last valid values. Add a pure constant assertion that `kAgentLabelTextScale == 2.0f` and `kActionLabelTextScale` remains unchanged.

Run: `./.venv/bin/platformio test -e native`

Expected: FAIL until the render constants and migrated quota model exist.

- [ ] **Step 2: Remove fabricated card quota defaults**

Initialize all three cards with unavailable `QuotaState`/`QuotaSnapshot` values. Preserve card names and colors. Delete hard-coded live-looking `86`, `85`, `78`, `1250`, and `1500` startup values.

Render `SYNC WAIT` (English) or `等待同步` (Chinese) when unavailable. Render the last real data plus `SYNC STALE` only when stale. Keep the reset countdown based on `receivedAtMs` and wrap-safe elapsed time.

- [ ] **Step 3: Replace `ApprovalState` mutation with `ApprovalController`**

When an accepted request arrives, copy its display fields into the main-loop-owned view state and persist its connection handle. Duplicate requests acknowledge without replaying alert feedback. Busy requests emit a stable `busy` error indication without changing the visible modal.

Touch and physical buttons call one shared function:

```cpp
void decidePendingApproval(vibe::ApprovalChoice choice) {
    const auto result = g_approvalController.decide(choice, millis());
    if (!result.hasValue) return;
    queueApprovalDecision(result.value);
    playDecisionFeedback(choice);
    g_uiDirty = true;
}
```

Remove generic `sendOuterActionEvent(kOkAction/kNgAction)` from the v2 path. The legacy branch alone sends `ACT07` or `ACT08`, never an indication as well.

- [ ] **Step 4: Add expiry and disconnect behavior**

Once per loop, call `expireIfNeeded(now)`. Expiry closes the modal, indicates `expired`, and does not play approval feedback. A disconnect event calls `cancel(now)` only when its handle owns the pending request; reconnect does not recreate the request.

- [ ] **Step 5: Double the six numeric labels and restore draw state**

In the non-action branch immediately before drawing `label`, set:

```cpp
constexpr float kAgentLabelTextScale = 2.0f;
M5.Display.setFont(&fonts::Orbitron_Light_32);
M5.Display.setTextSize(kAgentLabelTextScale);
M5.Display.setTextDatum(middle_center);
```

Keep the existing luminance-based label color, centered `outerX, outerY` coordinates, and three draw calls at horizontal offsets `-1`, `+1`, and `0`. Immediately after the six-circle loop, restore `Orbitron_Light_32`, text size `1.0f`, and `middle_center` datum so quota/status text does not inherit `2.0f`.

- [ ] **Step 6: Run automated verification**

Run:

```bash
./.venv/bin/platformio test -e native
./.venv/bin/platformio run -e m5stack-stopwatch
git diff --check
```

Expected: all native tests PASS, firmware build SUCCESS, no whitespace errors.

- [ ] **Step 7: Commit firmware behavior and UI**

```bash
git add include/vibe_state.h src/main.cpp test/test_vibe_core
git commit -m "fix: correlate approvals and improve watch readability"
```

---

### Task 4: Split the Swift companion into testable core components

**Files:**
- Modify: `companion/Package.swift`
- Create: `companion/Sources/VibeWatchCompanionCore/ProtocolModels.swift`
- Create: `companion/Sources/VibeWatchCompanionCore/Options.swift`
- Create: `companion/Sources/VibeWatchCompanionCore/AppServerClient.swift`
- Create: `companion/Sources/VibeWatchCompanionCore/Runner.swift`
- Rewrite: `companion/Sources/CodexWatchCompanion/main.swift`
- Create: `companion/Tests/VibeWatchCompanionCoreTests/ProtocolModelsTests.swift`
- Create: `companion/Tests/VibeWatchCompanionCoreTests/OptionsTests.swift`
- Create: `companion/Tests/VibeWatchCompanionCoreTests/AppServerClientTests.swift`

**Interfaces:**
- Produces: public `QuotaSnapshot`, `ApprovalRequestV2`, `ApprovalDecisionV2`, `CompanionOptions`, `CommandRunner`, `AppServerServing`.
- Consumes: Foundation; CoreBluetooth remains outside this task until Task 5.

- [ ] **Step 1: Add library and test targets**

Change `Package.swift` targets to:

```swift
.target(name: "VibeWatchCompanionCore"),
.executableTarget(
    name: "CodexWatchCompanion",
    dependencies: ["VibeWatchCompanionCore"]
),
.testTarget(
    name: "VibeWatchCompanionCoreTests",
    dependencies: ["VibeWatchCompanionCore"]
)
```

- [ ] **Step 2: Write failing Codable and option-validation tests**

Tests must assert exact snake_case JSON, canonical UUID preservation, decision enum values, manual quota preservation, `--auto` selection, unknown-card rejection, negative-credit rejection, TTL `5000...120000`, summary byte limit, incompatible flags, and missing `--device-id` rejection for every non-demo write.

Example:

```swift
func testRealApprovalRequiresPinnedDevice() throws {
    XCTAssertThrowsError(try CompanionOptions.parse([
        "tool", "--approval", "--summary", "Run tests"
    ])) { error in
        XCTAssertEqual(error as? CompanionError, .usage("--approval requires --device-id"))
    }
}
```

Run: `cd companion && swift test`

Expected: FAIL because the library target and models do not exist.

- [ ] **Step 3: Implement protocol models and validated options**

Use these core model shapes:

```swift
public struct ApprovalRequestV2: Codable, Equatable {
    public let version = 2
    public let kind = "approval_request"
    public let requestID: UUID
    public let card: AgentCard
    public let agentID: Int
    public let operationType: String
    public let summary: String
    public let ttlMs: Int
}

public enum ApprovalDecision: String, Codable {
    case approve, reject, expired, cancelled
}

public struct ApprovalDecisionV2: Codable, Equatable {
    public let version: Int
    public let kind: String
    public let requestID: UUID
    public let decision: ApprovalDecision
    public let decidedAtMs: UInt32
}
```

Provide explicit `CodingKeys` for `request_id`, `agent_id`, `operation_type`, `ttl_ms`, and `decided_at_ms`. Model CLI mode as an enum so approval, bootloader, automatic quota, manual quota, demo, and JSON-only cannot overlap accidentally.

- [ ] **Step 4: Extract and fixture-test App Server parsing**

Define `AppServerServing.readRateLimits() -> QuotaSnapshot`. Move process management into `AppServerClient`, inject line input in tests, and cover both `rateLimitsByLimitId.codex.primary` and legacy `rateLimits` shapes. Keep method `account/rateLimits/read`; remove the obsolete `account/rateLimits` variant.

Ensure malformed/missing data throws `malformedRateLimits` and never manufactures a value.

- [ ] **Step 5: Reduce executable main to dependency wiring**

`main.swift` must only create the app activation context, parse arguments, instantiate real dependencies, invoke `Runner`, print final stdout, print errors to stderr, and call `exit` with the returned code. No JSON schema, App Server parsing, or BLE state machine remains in the executable target.

- [ ] **Step 6: Run Swift tests and build**

Run:

```bash
cd companion
swift test
swift build
```

Expected: all XCTest cases PASS and debug build completes.

- [ ] **Step 7: Commit the Swift core split**

```bash
git add companion/Package.swift companion/Sources companion/Tests
git commit -m "refactor: split companion into testable core"
```

---

### Task 5: Implement pinned BLE transport and correlated approval results in Swift

**Files:**
- Create: `companion/Sources/VibeWatchCompanionCore/BLETransport.swift`
- Modify: `companion/Sources/VibeWatchCompanionCore/Runner.swift`
- Create: `companion/Tests/VibeWatchCompanionCoreTests/BLETransportTests.swift`
- Create: `companion/Tests/VibeWatchCompanionCoreTests/RunnerTests.swift`

**Interfaces:**
- Consumes: Task 4 models/options and firmware UUIDs from Task 2.
- Produces: `BLETransporting.writeQuota`, `BLETransporting.requestApproval`, and a CoreBluetooth actor/state machine.

- [ ] **Step 1: Write failing fake-transport runner tests**

Define the protocol in the test first:

```swift
public protocol BLETransporting {
    func writeQuota(_ snapshot: QuotaSnapshot, deviceID: UUID) throws
    func requestApproval(_ request: ApprovalRequestV2, deviceID: UUID) throws -> ApprovalDecisionV2
}
```

Test matching request success, mismatched indication ignored until timeout, duplicate indication delivered once, reject as exit code `0` with JSON decision, expired/cancelled as valid decision JSON, missing device as nonzero `device_not_found`, and transport timeout as nonzero `transport_error`.

Run: `cd companion && swift test`

Expected: FAIL because the transport protocol and runner behavior are absent.

- [ ] **Step 2: Implement exact-device discovery rules**

Use UUIDs `.01` service, `.02` quota, `.03` approval write, and `.04` approval result. For non-demo calls, retrieve/scan only the supplied CoreBluetooth identifier and reject every other peripheral. Remove name matching from real mode. Keep broad name/service discovery in a separate `discoverDemoDevices()` method only.

- [ ] **Step 3: Implement approval subscribe-write-wait ordering**

For approval:

1. connect to pinned peripheral;
2. discover service and `.03`/`.04` characteristics;
3. call `setNotifyValue(true, for: resultCharacteristic)` and wait for notification-state confirmation;
4. write encoded v2 request to `.03` with `.withResponse`;
5. wait for an indication whose decoded `requestID` equals the request;
6. cancel the connection after success or TTL plus five seconds;
7. return the matching decision or throw a stable error.

Never downgrade to `.withoutResponse`. Legacy mode writes the legacy payload to `.03` and exits after ATT acknowledgement because the generic HID consumer receives the later ACT event.

- [ ] **Step 4: Make output machine-readable and errors stable**

Final stdout is one sorted-key JSON object. Human progress and verbose discovery stay on stderr. Define stable error JSON containing `version`, `kind=error`, `code`, and `message`; map failure to exit code `1`, usage to `2`, and success/delivered decision to `0`.

- [ ] **Step 5: Run Swift tests and build**

Run:

```bash
cd companion
swift test
swift build -c release
```

Expected: all tests PASS and release build completes.

- [ ] **Step 6: Commit BLE v2 support**

```bash
git add companion/Sources/VibeWatchCompanionCore companion/Tests
git commit -m "feat: add pinned transactional BLE approvals"
```

---

### Task 6: Repair and test the Python compatibility CLI

**Files:**
- Rewrite: `scripts/sync_watch.py`
- Rewrite: `tests/test_sync_watch.py`

**Interfaces:**
- Consumes: Swift CLI flags and protocol schemas from Tasks 4–5.
- Produces: `build_request(args)`, `run_native(request, executable)`, `run_bleak(request, adapter)`, and `main(argv) -> int`.

- [ ] **Step 1: Replace tautological tests with failing production-path tests**

Use `unittest.mock`/pytest to assert:

```python
def test_manual_quota_forwards_every_value(native_runner):
    code = main([
        "--remaining", "42.5", "--reset", "900", "--card", "workbuddy",
        "--credits", "425", "--total-credits", "1000",
        "--device-id", "550e8400-e29b-41d4-a716-446655440000",
    ])
    assert code == 0
    assert native_runner.command == [
        native_runner.binary,
        "--remaining", "42.5", "--reset", "900", "--card", "workbuddy",
        "--credits", "425", "--total-credits", "1000",
        "--device-id", "550e8400-e29b-41d4-a716-446655440000",
    ]

def test_failed_transport_returns_nonzero(native_runner):
    native_runner.returncode = 1
    assert main(["--auto", "--device-id", VALID_UUID]) == 1
```

Also test approval `card`, request ID, agent ID, type, summary, TTL, and legacy flag; explicit Bleak selection; missing binding; unknown card; native build failure; and absence of any 85% fallback.

Run: `./.venv/bin/pytest -q`

Expected: FAIL because current functions discard values or hide failures.

- [ ] **Step 2: Introduce a normalized request model**

Use frozen dataclasses/enums to distinguish automatic quota, manual quota, approval, and demo discovery. Validation occurs once before choosing a backend. `--auto` and manual `--remaining/--reset` are mutually exclusive; manual mode requires both values; real modes require `--device-id`.

- [ ] **Step 3: Make the Swift backend a lossless adapter**

Build an argument list without `shell=True`. Forward every normalized value exactly. If the debug/release binary is absent, run `swift build`, verify its return code, resolve the produced executable, and then run it. Return the child's exit code and preserve its stdout/stderr streams.

- [ ] **Step 4: Make Bleak fallback explicit and schema-identical**

Add `--backend native|bleak`, defaulting to `native`. Bleak real mode resolves only the pinned device, writes with response to `.02` or `.03`, subscribes to `.04` for v2 approval, validates the returned request ID, and maps failures to nonzero status. Do not automatically retry authentication/validation failures through another backend.

- [ ] **Step 5: Remove obsolete App Server and fake defaults**

Delete Python's duplicated `get_codex_rate_limits()` process client; automatic quota is handled by the tested Swift core. Delete exception swallowing and fallback values `85.0`/`360000`.

- [ ] **Step 6: Run Python and Swift verification**

Run:

```bash
./.venv/bin/pytest -q
python3 -m compileall -q scripts tests
cd companion && swift test
```

Expected: Python tests PASS, compileall exits `0`, Swift tests remain PASS. Remove any newly generated untracked bytecode before committing.

- [ ] **Step 7: Commit the Python repair**

```bash
git add scripts/sync_watch.py tests/test_sync_watch.py
git commit -m "fix: make sync CLI preserve requests and failures"
```

---

### Task 7: Document protocol v2, migration, and real-device acceptance

**Files:**
- Create: `docs/COMPANION_PROTOCOL.md`
- Create: `docs/hardware-acceptance-v2.md`
- Create: `docs/release-notes-v1.01.md`
- Modify: `README.md`
- Modify: `README.zh-CN.md`
- Modify: `companion/README.md`
- Modify: `include/vibe_hid.h`

**Interfaces:**
- Consumes: exact UUIDs, schemas, limits, flags, exit codes, and behavior implemented in Tasks 1–6.
- Produces: authoritative operator/developer contract and a fillable hardware evidence record.

- [ ] **Step 1: Bump firmware protocol/release version consistently**

Change firmware version to `v1.01` and define protocol version `2`. Ensure Swift result JSON and documentation use version `2`; do not claim protocol v2 support from a binary reporting firmware `v1.0`.

- [ ] **Step 2: Write the authoritative protocol document**

Document service `.01`, quota `.02`, approval request `.03`, result `.04`, encrypted/bonded permissions, 512-byte maximum, all request/decision/error schemas, exact validation limits, subscribe-before-write sequence, idempotency/busy/expiry/disconnect rules, stdout/exit behavior, and one-release legacy removal schedule.

- [ ] **Step 3: Correct all user examples**

Every real Swift/Python example includes `--device-id`. Demo discovery examples explicitly say they use synthetic data. Remove references to missing documents and unsupported USB-mic firmware behavior from stable VibeWatch instructions, or clearly separate them as unsupported by this firmware.

- [ ] **Step 4: Create the hardware checklist template**

Include fields for firmware commit, companion commit, macOS version, sanitized device suffix, start/end time, and each of the 13 approved acceptance cases. Add commands for fresh build, upload, demo discovery, pinned quota, v2 approve/reject, duplicate ID, concurrent busy, short TTL expiry, disconnect cancellation, explicit legacy, and 100-cycle soak.

- [ ] **Step 5: Check documentation consistency**

Run:

```bash
rg -n "COMPANION_PROTOCOL|device-id|legacy-approval|7f0d4e66" README.md README.zh-CN.md companion docs include
git diff --check
```

Expected: every link resolves, real-write examples include device binding, UUIDs match implementation, no whitespace errors.

- [ ] **Step 6: Commit documentation and versioning**

```bash
git add README.md README.zh-CN.md companion/README.md docs include/vibe_hid.h
git commit -m "docs: publish secure companion protocol v2"
```

---

### Task 8: Expand CI and remove generated artifacts

**Files:**
- Modify: `.gitignore`
- Modify: `.github/workflows/firmware.yml`
- Create: `.github/workflows/companion.yml`
- Delete: `scripts/__pycache__/sync_watch.cpython-314.pyc`
- Delete: `tests/__pycache__/test_sync_watch.cpython-314-pytest-9.1.1.pyc`

**Interfaces:**
- Consumes: test/build commands established in Tasks 1–7.
- Produces: required Linux/macOS checks and artifact publication gated on all Linux checks.

- [ ] **Step 1: Add a failing hygiene check before deletion**

Run:

```bash
git ls-files | rg '(^|/)__pycache__/|\.pyc$'
```

Expected: FAIL the intended hygiene rule by listing the two tracked `.pyc` files.

- [ ] **Step 2: Ignore and remove generated bytecode**

Add exactly:

```gitignore
__pycache__/
*.py[cod]
.pytest_cache/
```

Remove only the two tracked bytecode files named above with:

```bash
git rm scripts/__pycache__/sync_watch.cpython-314.pyc
git rm tests/__pycache__/test_sync_watch.cpython-314-pytest-9.1.1.pyc
```

Do not remove source files or the local virtual environment.

- [ ] **Step 3: Expand Linux CI into ordered checks**

In `firmware.yml`, install PlatformIO `6.1.18` and pytest, then run:

```yaml
- run: test -z "$(git ls-files | grep -E '(^|/)__pycache__/|\.pyc$' || true)"
- run: python -m pytest -q
- run: python -m platformio test -e native
- run: python -m platformio run -e m5stack-stopwatch
```

Upload `firmware.bin` only after these steps succeed. Keep `permissions: contents: read` and pin current official action major versions.

- [ ] **Step 4: Add macOS Swift CI**

Create `companion.yml` on `macos-14` with checkout, `swift --version`, `swift test --package-path companion`, and `swift build -c release --package-path companion`. No signing, Bluetooth access, or device identifier is required in CI.

- [ ] **Step 5: Run the complete local automated gate**

Run:

```bash
test -z "$(git ls-files | grep -E '(^|/)__pycache__/|\.pyc$' || true)"
./.venv/bin/pytest -q
./.venv/bin/platformio test -e native
./.venv/bin/platformio run -e m5stack-stopwatch
cd companion && swift test && swift build -c release
```

Expected: hygiene check exits `0`; all Python/native/Swift tests PASS; firmware and release companion builds SUCCESS.

- [ ] **Step 6: Commit CI and hygiene**

```bash
git add .gitignore .github
git commit -m "ci: test firmware companion and host tools"
```

---

### Task 9: Execute real-device security, UI, and soak acceptance

**Files:**
- Modify: `docs/hardware-acceptance-v2.md`
- Do not modify production code during evidence collection; any failure starts a new red-green fix commit before repeating the full checklist.

**Interfaces:**
- Consumes: release firmware, release Swift companion, one M5Stack StopWatch, one macOS machine, and the Task 7 checklist.
- Produces: completed, sanitized release evidence.

- [ ] **Step 1: Record immutable test inputs**

Record `git rev-parse HEAD`, macOS version, Swift version, PlatformIO version, firmware SHA-256, companion SHA-256, and only the final six characters of the CoreBluetooth device UUID.

- [ ] **Step 2: Verify fresh-boot telemetry truth**

Erase or reset preferences only through the device's supported pairing/settings flow, flash the release firmware, boot without a host sync, and record PASS only if every card shows waiting/not-synced rather than 86/85/78 or sample credits.

- [ ] **Step 3: Verify unpaired writes are rejected**

Forget/delete the bond, attempt `.02` and `.03` writes, and record PASS only if ATT security fails and the watch shows no quota mutation, approval modal, sound, or vibration. Pair again through the normal UI afterward.

- [ ] **Step 4: Verify pinned quota and card routing**

Run real Codex quota plus manual Workbuddy credits with the exact device ID. Confirm each value appears only on its requested card and becomes stale after the configured interval if refresh stops.

- [ ] **Step 5: Verify correlated approval decisions**

Send one approval with a fixed UUID, approve by touch, and compare stdout UUID/decision. Repeat with a different UUID and reject by physical button. Each command must produce exactly one matching decision.

- [ ] **Step 6: Verify duplicate, busy, expiry, and disconnect cases**

Retry the same UUID while pending and confirm no second alert. Send a second UUID and confirm `busy` without modal replacement. Send `ttl_ms=5000` and do nothing; confirm `expired`. Disconnect while pending; confirm `cancelled` and no stale decision after reconnect.

- [ ] **Step 7: Verify the 2x agent labels and unaffected controls**

Inspect all six circles on each of three cards in default, selected, pressed, and animated agent states. Record PASS only if `1`–`6` are approximately twice the former character height, centered, fully inside the circles, contrast correctly, and the FAST/OK/NG/PLAN/AI layer plus all touch targets remain unchanged.

- [ ] **Step 8: Verify explicit legacy behavior**

Confirm a normal v2 request never emits ACT07/ACT08. Confirm legacy behavior occurs only with `--legacy-approval`, produces only the legacy action path, and is documented as deprecated.

- [ ] **Step 9: Run the 100-cycle soak**

Alternate approve/reject for 100 uniquely identified requests while forcing periodic disconnect/reconnect. Capture starting/ending free heap from sanitized serial diagnostics. PASS requires no crash, stuck modal, lost correlation, queue growth, or material monotonic heap loss.

- [ ] **Step 10: Re-run the automated gate after hardware acceptance**

Run the complete Task 8 local gate again from a clean worktree. Expected: all tests/builds PASS and `git status --short` shows only the completed acceptance document.

- [ ] **Step 11: Commit acceptance evidence**

```bash
git add docs/hardware-acceptance-v2.md
git commit -m "test: record protocol v2 hardware acceptance"
```

---

## Final Release Gate

- [ ] Review `git log --oneline` and confirm each task has its dedicated commit.
- [ ] Run `git diff cbeb1da..HEAD --check` and confirm no whitespace errors.
- [ ] Confirm all automated checks pass on GitHub Actions, including macOS Swift tests.
- [ ] Confirm `docs/hardware-acceptance-v2.md` has no failed or blank required case.
- [ ] Confirm protocol/docs UUIDs and firmware/companion version strings match.
- [ ] Confirm no secret, full device UUID, username, home path, generated app, log, `.pyc`, `.pio`, or `.build` artifact is tracked.
- [ ] Perform a focused security review of GATT permissions, bonded-peer checks, queue ownership, request correlation, and legacy isolation before release.

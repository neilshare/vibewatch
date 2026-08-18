#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vibe_hid.h"
#include "vibe_ingress.h"
#include "vibe_approval.h"
#include "vibe_protocol.h"
#include "vibe_render.h"
#include "vibe_state.h"
#include "sound.h"

namespace {

// -----------------------------------------------------------------------------
// UI geometry, timing, and persistent-setting keys
// -----------------------------------------------------------------------------

// The StopWatch display is 466 x 466 pixels. All primary controls are arranged
// around the physical center so the layout visually follows the round bezel.
constexpr int kOkAction = 1;
constexpr int kNgAction = 2;
constexpr int kScreenCenter = 233;
constexpr int kAgentOrbitRadius = 160;
constexpr int kAgentButtonRadius = 55;
constexpr int kMicButtonRadius = 67;
constexpr int kSettingsX = 72;
constexpr int kSettingsY = 370;
constexpr int kSettingsRadius = 22;
constexpr int kSettingsVisualRadius = 16;
constexpr int kSettingsCloseX = 365;
constexpr int kSettingsCloseY = 55;
constexpr int kSettingsCloseRadius = 25;
constexpr int kSettingsSliderLeft = 103;
constexpr int kSettingsSliderRight = 363;
constexpr std::uint32_t kLeftPhysicalButtonColor = 0xFFAC28;
constexpr std::uint32_t kRightPhysicalButtonColor = 0x2D8CFF;
constexpr std::uint32_t kButtonChordGraceMs = 90;
constexpr std::uint32_t kPhysicalMicHoldMs = 350;
constexpr std::uint32_t kUiAnimationPeriodMs = 80;
constexpr std::uint32_t kSelectionAnimationPeriodMs = 16;
constexpr std::uint32_t kSelectionAnimationBaseMs = 88;
constexpr std::uint32_t kBatteryUpdatePeriodMs = 30000;
constexpr char kPreferencesNamespace[] = "vibe-watch";
constexpr char kDeviceSlotKey[] = "device-slot";
constexpr char kSeVolumeKey[] = "se-volume";
using vibe::kAgentCount;
using vibe::kActionCount;
using vibe::AgentCard;
using vibe::CARD_CODEX;
using vibe::CARD_WORKBUDDY;
using vibe::CARD_ANTIGRAVITY;
using vibe::CARD_COUNT;
using vibe::Language;
using vibe::LANG_ZH;
using vibe::LANG_EN;
using vibe::AgentState;
using vibe::QuotaSnapshot;
using vibe::CardState;
using vibe::ApprovalState;

// Hit-test results share one integer space. Non-negative values below
// kAgentCount are outer-ring items; the remaining values identify fixed UI
// controls such as the microphone and settings widgets.
constexpr int kTouchMic = kAgentCount;
constexpr int kTouchSettings = kAgentCount + 1;
constexpr int kTouchSettingsBack = kAgentCount + 2;
constexpr int kTouchSlot1 = kAgentCount + 3;
constexpr int kTouchSlot2 = kAgentCount + 4;
constexpr int kTouchSlot3 = kAgentCount + 5;
constexpr int kTouchPair = kAgentCount + 6;
constexpr int kTouchVolume = kAgentCount + 7;
constexpr int kTouchVibrationStrength = kAgentCount + 8;
constexpr int kTouchAgentStateVibe = kAgentCount + 9;
constexpr int kTouchLanguage = kAgentCount + 10;

struct AmbientState {
    std::uint32_t color = 0x304FFE;
    float brightness = 0.25f;
    int effect = 0;
    float speed = 0.4f;
};

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------

// Visual state received from the host. Agent colors communicate per-chat state;
// ambient data is retained for protocol compatibility but is not drawn at the bezel.
std::array<AgentState, kAgentCount> g_agents;
AmbientState g_ambient;
String g_focusedApp;

// BLE objects are created once during setup and remain valid for the lifetime
// of the firmware. RPC messages are moved out of the BLE callback through a
// FreeRTOS queue so JSON processing never blocks the NimBLE task.
NimBLEServer* g_server = nullptr;
NimBLEHIDDevice* g_hid = nullptr;
NimBLECharacteristic* g_keyboardInput = nullptr;
NimBLECharacteristic* g_consumerInput = nullptr;
NimBLECharacteristic* g_vendorInput = nullptr;
NimBLECharacteristic* g_vendorOutput = nullptr;
NimBLECharacteristic* g_approvalResult = nullptr;

class NoCopyCharacteristic final : public NimBLECharacteristic {
  public:
    NoCopyCharacteristic(const NimBLEUUID& uuid, std::uint16_t properties,
                         std::uint16_t maxLength, NimBLEService* service)
        : NimBLECharacteristic(uuid, properties, maxLength, service) {}

    const NimBLEAttValue& valueReference() const { return getAttVal(); }
};

NoCopyCharacteristic* addNoCopyCharacteristic(
    NimBLEService* service, const NimBLEUUID& uuid, std::uint16_t properties,
    std::uint16_t maxLength) {
    static const std::array<std::uint8_t, vibe::kIngressPayloadLength>
        capacitySeed{};
    auto* characteristic =
        new NoCopyCharacteristic(uuid, properties, maxLength, service);
    // NimBLE's writeEvent stores the incoming bytes before invoking onWrite.
    // Reserve the full fixed capacity during setup so that store cannot grow
    // the value buffer on the callback task.
    characteristic->setValue(capacitySeed.data(), maxLength);
    service->addCharacteristic(characteristic);
    return characteristic;
}

bool g_connected = false;
bool g_uiDirty = true;
portMUX_TYPE g_callbackStateMux = portMUX_INITIALIZER_UNLOCKED;
bool g_pairingSuccessPending = false;

struct CallbackConnection {
    bool active{false};
    std::uint16_t connectionHandle{0};
    std::uint32_t connectionGeneration{0};
    bool configurePending{false};
    bool disconnectFallbackPending{false};
    std::uint32_t disconnectedGeneration{0};
    bool hidDisconnectPending{false};
    std::uint32_t hidDisconnectGeneration{0};
    bool approvalOverflowPending{false};
    std::uint32_t approvalOverflowGeneration{0};
    bool approvalIndicationsEnabled{false};
};

std::array<CallbackConnection, 6> g_callbackConnections{};
vibe::HidStreamTracker<3> g_hidStreamTracker;

struct IndicationStatusHandoff {
    bool pending{false};
    int code{0};
    std::uint32_t deliveryId{0};
};

IndicationStatusHandoff g_indicationStatusHandoff;
std::uint32_t g_callbackDeliveryId{0};

vibe::HidRpcAssembler g_hidRpcAssembler;

std::array<CardState, CARD_COUNT> g_cards = {{
    CardState("CODEX", 0x12D6B2, QuotaSnapshot{}),
    CardState("WORKBUDDY", 0x00E5FF, QuotaSnapshot{}),
    CardState("ANTIGRAVITY", 0x9D74FF, QuotaSnapshot{})
}};

AgentCard g_currentCard = CARD_CODEX;
constexpr std::uint32_t kQuotaStaleAfterMs = 180000;

void formatResetCountdown(std::uint32_t seconds, char* buffer, std::size_t len) {
    if (seconds == 0) {
        std::snprintf(buffer, len, "RESET --");
        return;
    }
    const unsigned days = seconds / 86400;
    const unsigned hours = (seconds % 86400) / 3600;
    const unsigned mins = (seconds % 3600) / 60;
    if (days > 0) {
        std::snprintf(buffer, len, "RESET %uD %02uH", days, hours);
    } else if (hours > 0) {
        std::snprintf(buffer, len, "RESET %uH %02uM", hours, mins);
    } else {
        std::snprintf(buffer, len, "RESET %uM", mins);
    }
}

void applyQuotaStatus(JsonVariantConst params) {
    if (params.isNull()) return;
    float remaining = -1.0f;
    std::uint32_t resetSec = 0;
    int targetCard = g_currentCard;
    float credits = -1.0f;
    float totalCredits = -1.0f;

    if (params.is<JsonObjectConst>()) {
        JsonObjectConst obj = params.as<JsonObjectConst>();
        remaining = obj["remaining_percent"] | obj["remainingPercent"] | -1.0f;
        resetSec = obj["reset_in_seconds"] | obj["resetInSeconds"] | 0U;
        credits = obj["credits"] | obj["credit"] | obj["balance"] | -1.0f;
        totalCredits = obj["total_credits"] | obj["totalCredits"] | obj["total"] | -1.0f;
        const char* cardName = obj["card"] | obj["agent"] | "";
        if (strcasecmp(cardName, "workbuddy") == 0 || strcasecmp(cardName, "buddy") == 0) {
            targetCard = CARD_WORKBUDDY;
        } else if (strcasecmp(cardName, "antigravity") == 0 || strcasecmp(cardName, "gravity") == 0) {
            targetCard = CARD_ANTIGRAVITY;
        } else if (strcasecmp(cardName, "codex") == 0) {
            targetCard = CARD_CODEX;
        }
    }

    const bool hasCreditFields = credits >= 0.0f || totalCredits >= 0.0f;
    float snapshotCredits = 0.0f;
    float snapshotTotalCredits = 0.0f;
    if (hasCreditFields) {
        if (credits < 0.0f || totalCredits <= 0.0f) {
            Serial.printf("Quota update for %s rejected: incomplete credit snapshot\n",
                          g_cards[targetCard].name);
            return;
        }
        snapshotCredits = credits;
        snapshotTotalCredits = totalCredits;
        if (remaining < 0.0f) {
            remaining = std::max(0.0f, std::min(100.0f,
                                                (credits / totalCredits) * 100.0f));
        }
    }

    auto& quota = g_cards[targetCard].quota;
    if (!quota.apply(remaining, resetSec, snapshotCredits, snapshotTotalCredits, millis())) {
        Serial.printf("Quota update for %s rejected: invalid snapshot\n",
                      g_cards[targetCard].name);
        return;
    }
    g_uiDirty = true;
    Serial.printf("Quota update for %s: %.1f%% credits=%.1f resetIn=%us\n",
                  g_cards[targetCard].name, remaining, quota.credits, resetSec);
}

void noteActivity();
void vibrate(std::uint8_t strength = 120, std::uint32_t durationMs = 25);
void playSe(float frequency = 880.0f, std::uint32_t durationMs = 35);

ApprovalState g_approval;
vibe::ApprovalController g_approvalController;
std::uint16_t g_pendingApprovalConnection = 0;
std::uint32_t g_pendingApprovalGeneration = 0;
bool g_v2ApprovalActive = false;

enum class HeapDiagnosticEvent : std::uint8_t {
    Boot,
    Approved,
    Rejected,
    Expired,
    Cancelled,
};

std::uint32_t g_heapDiagnosticSample = 0;

const char* heapDiagnosticEventName(HeapDiagnosticEvent event) {
    switch (event) {
        case HeapDiagnosticEvent::Boot: return "boot";
        case HeapDiagnosticEvent::Approved: return "approved";
        case HeapDiagnosticEvent::Rejected: return "rejected";
        case HeapDiagnosticEvent::Expired: return "expired";
        case HeapDiagnosticEvent::Cancelled: return "cancelled";
    }
    return "unknown";
}

void emitFreeHeapDiagnostic(HeapDiagnosticEvent event) {
    ++g_heapDiagnosticSample;
    Serial.printf("VW_HEAP_DIAG sample=%lu event=%s free_bytes=%lu\n",
                  static_cast<unsigned long>(g_heapDiagnosticSample),
                  heapDiagnosticEventName(event),
                  static_cast<unsigned long>(ESP.getFreeHeap()));
}

struct PendingIndication {
    bool active{false};
    std::uint16_t connectionHandle{0};
    std::uint32_t connectionGeneration{0};
    std::uint32_t deliveryId{0};
    std::uint16_t length{0};
    std::array<std::uint8_t, vibe::kIngressPayloadLength> payload{};
    std::uint32_t deadlineMs{0};
};

PendingIndication g_pendingIndication;
std::uint32_t g_nextIndicationDeliveryId = 1;
bool g_indicationTransportBlocked = false;
std::uint16_t g_blockedIndicationConnection = 0;
std::uint32_t g_blockedIndicationGeneration = 0;
constexpr std::uint32_t kIndicationDeliveryTimeoutMs = 5000;

constexpr std::uint32_t kApprovalSwitchCooldownMs = 4000;
std::uint32_t g_lastApprovalSwitchedAtMs = 0;

void applyApprovalRequest(JsonVariantConst params) {
    if (g_approvalController.pending()) {
        Serial.println("legacy approval rejected: busy");
        return;
    }
    if (params.isNull()) return;
    if (params.is<JsonObjectConst>()) {
        JsonObjectConst obj = params.as<JsonObjectConst>();
        const bool active = obj["active"] | true;
        if (!active) {
            g_approval.active = false;
            g_uiDirty = true;
            return;
        }

        const char* cardName = obj["card"] | obj["agent_system"] | "";
        AgentCard targetCard = g_currentCard;
        if (strcasecmp(cardName, "workbuddy") == 0 || strcasecmp(cardName, "buddy") == 0) {
            targetCard = CARD_WORKBUDDY;
        } else if (strcasecmp(cardName, "antigravity") == 0 || strcasecmp(cardName, "gravity") == 0) {
            targetCard = CARD_ANTIGRAVITY;
        } else if (strcasecmp(cardName, "codex") == 0) {
            targetCard = CARD_CODEX;
        }

        const uint32_t now = millis();
        // Protection Window: If an approval is active and user is interacting within cooldown,
        // protect current modal and avoid flapping across cards
        if (g_approval.active && (now - g_approval.triggeredAtMs < kApprovalSwitchCooldownMs) && targetCard != g_currentCard) {
            Serial.printf("Approval request for [%s] throttled by active modal on [%s]\n",
                          g_cards[targetCard].name, g_cards[g_currentCard].name);
            return;
        }

        g_currentCard = targetCard;
        g_v2ApprovalActive = false;
        g_approval.active = true;
        g_approval.agentId = obj["agent"] | obj["agentId"] | 0;
        const char* t = obj["type"] | obj["t"] | "EXEC";
        const char* s = obj["summary"] | obj["desc"] | obj["s"] | "Run Command";
        std::strncpy(g_approval.type, t, sizeof(g_approval.type) - 1);
        g_approval.type[sizeof(g_approval.type) - 1] = '\0';
        std::strncpy(g_approval.summary, s, sizeof(g_approval.summary) - 1);
        g_approval.summary[sizeof(g_approval.summary) - 1] = '\0';
        g_approval.triggeredAtMs = now;
        g_lastApprovalSwitchedAtMs = now;
        noteActivity();
        vibrate(250, 90);
        playSe(1250.0f, 75);
        g_uiDirty = true;
        Serial.printf("Approval request for [%s]: type=%s summary=%s\n",
                      g_cards[g_currentCard].name, g_approval.type, g_approval.summary);
    }
}

// Input state is intentionally explicit because a physical-button press may
// become a single action, a long press, or a two-button layer-switch chord.
int g_activeTouch = -1;
int g_touchStartX = -1;
int g_touchStartY = -1;
int g_activeSwipe = -1;
constexpr int kSwipeThresholdPx = 45;

std::uint32_t g_lastActivityAt = 0;
bool g_isDimmed = false;
bool g_isScreenSleeping = false;
constexpr std::uint32_t kDimTimeoutMs = 60000;
constexpr std::uint32_t kSleepTimeoutMs = 180000;

void noteActivity() {
    g_lastActivityAt = millis();
    if (g_isScreenSleeping || g_isDimmed) {
        g_isScreenSleeping = false;
        g_isDimmed = false;
        setCpuFrequencyMhz(240);
        M5.Display.setBrightness(80);
        g_uiDirty = true;
    }
}

std::uint32_t g_vibrationOffAt = 0;
std::uint32_t g_lastUiDraw = 0;
std::uint32_t g_lastBatteryUpdate = 0;
std::uint8_t g_batteryLevel = 100;
bool g_isCharging = false;
bool g_settingsOpen = false;
int g_deviceSlot = 1;
int g_pendingDeviceSlot = 1;
int g_selectedAgent = 0;
int g_selectedAction = 0;
bool g_planModeEnabled = false;
float g_selectionX = 0.0f;
float g_selectionY = 0.0f;
float g_selectionFromX = 0.0f;
float g_selectionFromY = 0.0f;
float g_selectionToX = 0.0f;
float g_selectionToY = 0.0f;
float g_selectionFromAngle = 0.0f;
float g_selectionToAngle = 0.0f;
std::uint32_t g_selectionAnimationStartedAt = 0;
std::uint32_t g_selectionAnimationDurationMs = 260;
bool g_selectionAnimating = false;
int g_leftAgentPressed = -1;
bool g_leftPressedActionLayer = false;
bool g_leftPressPending = false;
std::uint32_t g_leftPressedAt = 0;
bool g_rightLongTriggered = false;
std::uint32_t g_rightPhysicalPressedAt = 0;
bool g_rightActionPending = false;
bool g_rightActionPressed = false;
std::uint32_t g_rightActionPressedAt = 0;
bool g_buttonChordActive = false;
bool g_actionLayer = false;
bool g_touchActionLayer = false;
std::uint32_t g_restartAt = 0;
char g_deviceName[24] = {};
std::uint8_t g_seVolume = 128;
std::uint8_t g_vibrationStrength = 255;
bool g_agentStateVibeEnabled = true;
Language g_language = LANG_ZH;
std::uint32_t g_lastAgentVibrationAt = 0;

std::array<int, kAgentCount> agentX{};
std::array<int, kAgentCount> agentY{};
std::array<int, kActionCount> actionX{};
std::array<int, kActionCount> actionY{};

// -----------------------------------------------------------------------------
// Preferences, sound, color, haptics, and battery helpers
// -----------------------------------------------------------------------------

constexpr char kVibrationStrengthKey[] = "vibe-strength";
constexpr char kAgentStateVibeKey[] = "state-vibe";
constexpr char kLanguageKey[] = "language";
constexpr char kSavedCardKey[] = "card";

void loadPreferences() {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, true);
    g_deviceSlot = preferences.getUChar(kDeviceSlotKey, 1);
    g_seVolume = preferences.getUChar(kSeVolumeKey, 128);
    g_vibrationStrength = preferences.getUChar(kVibrationStrengthKey, 255);
    g_agentStateVibeEnabled = preferences.getBool(kAgentStateVibeKey, true);
    g_language = static_cast<Language>(
        preferences.getUChar(kLanguageKey, static_cast<std::uint8_t>(LANG_ZH)));
    const std::uint8_t savedCard = preferences.getUChar(kSavedCardKey, static_cast<std::uint8_t>(CARD_CODEX));
    if (savedCard < CARD_COUNT) {
        g_currentCard = static_cast<AgentCard>(savedCard);
    }
    preferences.end();
    if (g_deviceSlot < 1 || g_deviceSlot > 3) {
        g_deviceSlot = 1;
    }
    if (g_language > LANG_EN) {
        g_language = LANG_ZH;
    }
    g_pendingDeviceSlot = g_deviceSlot;
    std::snprintf(g_deviceName, sizeof(g_deviceName), "%s%d", vibe::kDeviceNamePrefix, g_deviceSlot);
}

void saveCurrentCard() {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, false);
    preferences.putUChar(kSavedCardKey, static_cast<std::uint8_t>(g_currentCard));
    preferences.end();
}

void saveLanguage() {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, false);
    preferences.putUChar(kLanguageKey, static_cast<std::uint8_t>(g_language));
    preferences.end();
}

void saveDeviceSlot(int slot) {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, false);
    preferences.putUChar(kDeviceSlotKey, static_cast<std::uint8_t>(slot));
    preferences.end();
}

void saveSeVolume() {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, false);
    preferences.putUChar(kSeVolumeKey, g_seVolume);
    preferences.end();
}

void saveFeedbackSettings() {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, false);
    preferences.putUChar(kVibrationStrengthKey, g_vibrationStrength);
    preferences.putBool(kAgentStateVibeKey, g_agentStateVibeEnabled);
    preferences.end();
}

void playSe(float frequency, std::uint32_t durationMs) {
    sound::playSquare(frequency, durationMs, g_seVolume);
}

void playMicSe(bool pressed, std::uint8_t tempoMultiplier = 1) {
    sound::playEffect(
        pressed ? sound::Effect::Start : sound::Effect::StartReverse,
        g_seVolume, tempoMultiplier);
}

void renderUi(std::uint32_t now);

float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

std::uint16_t scaledColor(std::uint32_t packed, float brightness) {
    const float scale = clamp01(brightness);
    const auto r = static_cast<std::uint8_t>(((packed >> 16) & 0xFF) * scale);
    const auto g = static_cast<std::uint8_t>(((packed >> 8) & 0xFF) * scale);
    const auto b = static_cast<std::uint8_t>((packed & 0xFF) * scale);
    return M5.Display.color565(r, g, b);
}

float effectBrightness(int effect, float brightness, float speed, std::uint32_t now) {
    if (effect == 0 || brightness <= 0.0f) {
        return 0.0f;
    }
    if (effect == 4 || effect == 6) {
        const float hz = 0.35f + clamp01(speed) * 1.4f;
        const float phase = static_cast<float>(now % 10000) * 0.001f * hz * 2.0f * PI;
        const float low = effect == 6 ? 0.5f : 0.15f;
        return brightness * (low + (1.0f - low) * (0.5f + 0.5f * std::sin(phase)));
    }
    return brightness;
}

bool uiIsAnimated() {
    if (g_selectionAnimating) {
        return true;
    }
    for (const auto& state : g_cards[g_currentCard].agents) {
        if (state.effect == 4 || state.effect == 6) {
            return true;
        }
    }
    return false;
}

void vibrate(std::uint8_t strength, std::uint32_t durationMs) {
    if (strength == 0 || g_vibrationStrength == 0) {
        return;
    }
    const auto scaledStrength = static_cast<std::uint8_t>(std::max(
        1U, (static_cast<unsigned>(strength) * g_vibrationStrength + 127U) / 255U));
    M5.Power.setVibration(scaledStrength);
    g_vibrationOffAt = millis() + durationMs;
}

void updateBattery(bool notify) {
    const int level = M5.Power.getBatteryLevel();
    if (level >= 0 && level <= 100) {
        g_batteryLevel = static_cast<std::uint8_t>(level);
    }
    g_isCharging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    if (g_hid != nullptr) {
        g_hid->setBatteryLevel(g_batteryLevel, notify && g_connected);
    }
    g_lastBatteryUpdate = millis();
    g_uiDirty = true;
}

// -----------------------------------------------------------------------------
// Host communication
// -----------------------------------------------------------------------------

// Vendor JSON-RPC messages are split into fixed-size HID reports. Byte 0 is the
// channel, byte 1 is the payload length, and bytes 2..62 contain UTF-8 JSON.
void sendFramedJson(String payload, bool appendCrlf,
                    std::uint16_t connectionHandle) {
    if (!g_connected || g_vendorInput == nullptr) {
        return;
    }
    if (appendCrlf && !payload.endsWith("\r\n")) {
        payload += "\r\n";
    }

    const std::size_t total = payload.length();
    std::size_t offset = 0;
    while (offset < total) {
        const std::size_t chunk = std::min(vibe::kRpcChunkLength, total - offset);
        std::uint8_t report[vibe::kBleReportLength] = {};
        report[0] = vibe::kChannelJsonRpc;
        report[1] = static_cast<std::uint8_t>(chunk);
        std::memcpy(&report[2], payload.c_str() + offset, chunk);
        if (!g_vendorInput->notify(report, sizeof(report), connectionHandle)) {
            Serial.println("BLE notify failed");
            return;
        }
        offset += chunk;
        if (offset < total) {
            delay(8);
        }
    }
}

void sendStandardKeyboardKey(uint8_t modifiers, uint8_t keycode, bool pressed) {
    if (!g_connected || g_keyboardInput == nullptr) {
        return;
    }
    uint8_t report[8] = {};
    if (pressed) {
        report[0] = modifiers;
        report[2] = keycode;
    }
    g_keyboardInput->setValue(report, sizeof(report));
    g_keyboardInput->notify();
}

void sendKeyEvent(const char* key, bool pressed) {
    if (!g_connected || g_vendorInput == nullptr) {
        return;
    }

    std::uint8_t report[vibe::kBleReportLength] = {};
    report[0] = vibe::kChannelJsonRpc;
    const int written = std::snprintf(
        reinterpret_cast<char*>(&report[2]), vibe::kRpcChunkLength,
        "{\"m\":\"v.oai.hid\",\"p\":{\"k\":\"%s\",\"act\":%u,\"c\":\"%s\"}}\r\n",
        key, pressed ? 1U : 0U, g_cards[g_currentCard].name);
    if (written < 0 || written >= static_cast<int>(vibe::kRpcChunkLength)) {
        Serial.println("HID event payload overflow");
        return;
    }
    report[1] = static_cast<std::uint8_t>(written);
    g_vendorInput->setValue(report, sizeof(report));
    if (!g_vendorInput->notify()) {
        Serial.printf("HID notify failed: %s\n", key);
        return;
    }
    Serial.printf("HID %s %s [%s] len=%d\n", key, pressed ? "DOWN" : "UP", g_cards[g_currentCard].name, written);
}

void sendConsumerKey(uint16_t keycode, bool pressed) {
    if (!g_connected || g_consumerInput == nullptr) {
        return;
    }
    uint8_t report[2] = {};
    if (pressed) {
        report[0] = static_cast<uint8_t>(keycode & 0xFF);
        report[1] = static_cast<uint8_t>((keycode >> 8) & 0xFF);
    }
    g_consumerInput->setValue(report, sizeof(report));
    g_consumerInput->notify();
}

void sendAgentEvent(int index, bool pressed) {
    char key[5];
    std::snprintf(key, sizeof(key), "AG%02d", index);
    sendKeyEvent(key, pressed);

    // Standard BLE HID Key: Send standard digit keys '1'..'6' (0x1E..0x23) and F13..F18
    if (index >= 0 && index < 6) {
        if (g_currentCard == CARD_WORKBUDDY || g_currentCard == CARD_ANTIGRAVITY) {
            sendStandardKeyboardKey(0x00, 0x1E + index, pressed); // Direct standard digits '1'..'6'
        } else {
            sendStandardKeyboardKey(0x00, 0x68 + index, pressed); // F13..F18
        }
    }
}

void sendActionEvent(int index, bool pressed) {
    char key[6];
    std::snprintf(key, sizeof(key), "ACT%02d", index);
    sendKeyEvent(key, pressed);

    // Standard BLE HID Key mappings for Workbuddy / Antigravity / macOS UI
    if (index == 7) { // kOkAction (6 + 1)
        sendStandardKeyboardKey(0x00, 0x28, pressed); // Return / Enter
    } else if (index == 8) { // kNgAction (6 + 2)
        sendStandardKeyboardKey(0x00, 0x29, pressed); // Escape
    } else if (index == 6) { // FAST (6 + 0)
        sendStandardKeyboardKey(0x00, 0x6F, pressed); // F20
    } else if (index == 9) { // PLAN (6 + 3)
        sendStandardKeyboardKey(0x00, 0x70, pressed); // F21
    } else if (index == 12) { // AI assistant single tap
        if (g_currentCard == CARD_ANTIGRAVITY) {
            // Antigravity prompt focus: send Return or standard Enter
            sendStandardKeyboardKey(0x00, 0x28, pressed);
        } else if (g_currentCard == CARD_WORKBUDDY) {
            // Workbuddy single click: send Return or standard Enter
            sendStandardKeyboardKey(0x00, 0x28, pressed);
        } else {
            sendStandardKeyboardKey(0x00, 0x71, pressed); // F22
        }
    }
}

void sendMicEvent(bool pressed) {
    sendActionEvent(10, pressed);
    // NimBLE notifications are asynchronous. Pace the paired MIC reports so
    // ACT11 cannot overwrite ACT10 in the controller buffer before delivery.
    delay(12);
    sendActionEvent(11, pressed);

    // Dedicated Voice Dictation without interfering with third-party app shortcuts:
    // Only send the dedicated vendor channel ACT10/11 + standard F19
    sendStandardKeyboardKey(0x00, 0x6E, pressed); // F19 (safe function key)
}

void sendJoystickEvent(float angle, float distance) {
    if (!g_connected || g_vendorInput == nullptr) {
        return;
    }
    std::uint8_t report[vibe::kBleReportLength] = {};
    report[0] = vibe::kChannelJsonRpc;
    const int written = std::snprintf(
        reinterpret_cast<char*>(&report[2]), vibe::kRpcChunkLength,
        "{\"m\":\"v.oai.rad\",\"p\":{\"a\":%.2f,\"d\":%.2f}}\r\n", angle, distance);
    if (written < 0 || written >= static_cast<int>(vibe::kRpcChunkLength)) {
        return;
    }
    report[1] = static_cast<std::uint8_t>(written);
    g_vendorInput->setValue(report, sizeof(report));
    g_vendorInput->notify();
    Serial.printf("JOYSTICK angle=%.2f dist=%.2f\n", angle, distance);
}

// Apply the host's compact agent-state array directly to the targeted card's six ring buttons.
void applyAgentStatus(JsonVariantConst params) {
    noteActivity();
    int targetCard = g_currentCard;
    JsonArrayConst items;
    if (params.is<JsonArrayConst>()) {
        items = params.as<JsonArrayConst>();
    } else if (params.is<JsonObjectConst>()) {
        JsonObjectConst obj = params.as<JsonObjectConst>();
        const char* cardName = obj["card"] | obj["agent_system"] | "";
        if (strcasecmp(cardName, "workbuddy") == 0 || strcasecmp(cardName, "buddy") == 0) {
            targetCard = CARD_WORKBUDDY;
        } else if (strcasecmp(cardName, "antigravity") == 0 || strcasecmp(cardName, "gravity") == 0) {
            targetCard = CARD_ANTIGRAVITY;
        } else if (strcasecmp(cardName, "codex") == 0) {
            targetCard = CARD_CODEX;
        }
        items = obj["agents"].as<JsonArrayConst>();
    } else {
        return;
    }

    if (items.isNull()) return;

    bool anyAgentChanged = false;
    for (JsonObjectConst item : items) {
        const int id = item["id"] | -1;
        if (id < 0 || id >= kAgentCount) {
            continue;
        }
        AgentState next;
        next.color = item["c"] | 0U;
        next.brightness = item["b"] | 0.0f;
        next.effect = item["e"] | 0;
        next.speed = item["s"] | 0.0f;
        auto& state = g_cards[targetCard].agents[id];
        const bool changed = state.color != next.color ||
                             std::abs(state.brightness - next.brightness) > 0.001f ||
                             state.effect != next.effect ||
                             std::abs(state.speed - next.speed) > 0.001f;
        anyAgentChanged = anyAgentChanged || changed;
        state = next;
    }

    const std::uint32_t now = millis();
    if (anyAgentChanged && g_agentStateVibeEnabled &&
        now - g_lastAgentVibrationAt >= 120) {
        vibrate(190, 42);
        g_lastAgentVibrationAt = now;
    }
    g_uiDirty = true;
}

void applyAmbientStatus(JsonVariantConst params) {
    JsonObjectConst ambient = params["ambient"].as<JsonObjectConst>();
    if (ambient.isNull()) {
        return;
    }
    g_ambient.color = ambient["c"] | 0U;
    g_ambient.brightness = ambient["b"] | 0.0f;
    g_ambient.effect = ambient["e"] | 0;
    g_ambient.speed = ambient["s"] | 0.0f;
    g_uiDirty = true;
}

void applyFocusedApp(JsonVariantConst params) {
    const char* appName = params["appName"] | "";
    g_focusedApp = appName;
    if (g_focusedApp.length() > 18) {
        g_focusedApp = g_focusedApp.substring(0, 17) + "…";
    }
    g_uiDirty = true;
}

void sendRpcResponse(const char* method, int id,
                     std::uint16_t connectionHandle) {
    JsonDocument response;
    response["id"] = id;
    response["method"] = method;

    if (std::strcmp(method, "device.status") == 0) {
        updateBattery(false);
        JsonObject result = response["result"].to<JsonObject>();
        result["version"] = vibe::kFirmwareVersion;
        result["profile_index"] = 0;
        result["layer_index"] = 1;
        result["battery"] = g_batteryLevel;
        result["is_charging"] = g_isCharging;
    } else if (std::strcmp(method, "sys.version") == 0) {
        response["result"]["version"] = vibe::kFirmwareVersion;
    } else {
        response["result"]["ok"] = 1;
    }

    String json;
    serializeJson(response, json);
    sendFramedJson(json, true, connectionHandle);
    Serial.printf("RPC response: %s id=%d\n", method, id);
}

void processRpc(const std::uint8_t* json, std::size_t length,
                std::uint16_t connectionHandle) {
    JsonDocument request;
    const DeserializationError error = deserializeJson(request, json, length);
    if (error) {
        Serial.printf("RPC parse failed: %s\n", error.c_str());
        return;
    }

    const char* method = request["method"] | request["m"] | "";
    if (!vibe::hidRpcMethodAllowed(method)) {
        Serial.println("HID approval ingress rejected");
        return;
    }
    int id = request["id"] | request["i"] | -1;
    JsonVariantConst params = request["params"];
    if (params.isNull()) {
        params = request["p"];
    }

    if (std::strcmp(method, "v.oai.thstatus") == 0) {
        applyAgentStatus(params);
    } else if (std::strcmp(method, "v.oai.rgbcfg") == 0) {
        applyAmbientStatus(params);
    } else if (std::strcmp(method, "host.focused_app") == 0) {
        applyFocusedApp(params);
    } else if (std::strcmp(method, "v.oai.quota") == 0 || std::strcmp(method, "quota") == 0) {
        applyQuotaStatus(params);
    }

    if (id >= 0 && method[0] != '\0') {
        sendRpcResponse(method, id, connectionHandle);
    }
}

CallbackConnection* findCallbackConnectionLocked(
    std::uint16_t connectionHandle) {
    for (auto& connection : g_callbackConnections) {
        if (connection.connectionGeneration != 0 &&
            connection.connectionHandle == connectionHandle) {
            return &connection;
        }
    }
    return nullptr;
}

CallbackConnection* findInactiveCallbackConnectionLocked() {
    for (auto& connection : g_callbackConnections) {
        if (!connection.active &&
            !connection.disconnectFallbackPending) {
            return &connection;
        }
    }
    return nullptr;
}

vibe::HidStreamToken publishConnectionConnected(
    std::uint16_t connectionHandle) {
    portENTER_CRITICAL(&g_callbackStateMux);
    const vibe::HidStreamToken token =
        g_hidStreamTracker.connect(connectionHandle);
    CallbackConnection* connection =
        findCallbackConnectionLocked(connectionHandle);
    if (connection == nullptr) {
        connection = findInactiveCallbackConnectionLocked();
    }
    if (connection != nullptr && token.active) {
        const bool disconnectFallbackPending =
            connection->disconnectFallbackPending;
        const std::uint32_t disconnectedGeneration =
            connection->disconnectedGeneration;
        *connection = {};
        connection->active = true;
        connection->connectionHandle = connectionHandle;
        connection->connectionGeneration = token.connectionGeneration;
        connection->configurePending = true;
        connection->disconnectFallbackPending =
            disconnectFallbackPending;
        connection->disconnectedGeneration = disconnectedGeneration;
    }
    portEXIT_CRITICAL(&g_callbackStateMux);
    return token;
}

vibe::HidStreamToken currentConnectionToken(
    std::uint16_t connectionHandle) {
    portENTER_CRITICAL(&g_callbackStateMux);
    const vibe::HidStreamToken token =
        g_hidStreamTracker.current(connectionHandle);
    portEXIT_CRITICAL(&g_callbackStateMux);
    return token;
}

void publishConnectionDisconnected(std::uint16_t connectionHandle,
                                   std::uint32_t generation,
                                   bool queueFallback) {
    portENTER_CRITICAL(&g_callbackStateMux);
    CallbackConnection* connection =
        findCallbackConnectionLocked(connectionHandle);
    if (connection != nullptr &&
        connection->connectionGeneration == generation) {
        connection->active = false;
        connection->approvalIndicationsEnabled = false;
        if (queueFallback) {
            connection->disconnectFallbackPending = true;
            connection->disconnectedGeneration = generation;
        }
    }
    g_hidStreamTracker.disconnect(connectionHandle);
    portEXIT_CRITICAL(&g_callbackStateMux);
}

bool approvalIndicationsEnabled(std::uint16_t connectionHandle,
                                std::uint32_t generation) {
    bool enabled = false;
    portENTER_CRITICAL(&g_callbackStateMux);
    const CallbackConnection* connection =
        findCallbackConnectionLocked(connectionHandle);
    if (connection != nullptr) {
        enabled = connection->active &&
                  connection->connectionGeneration == generation &&
                  connection->approvalIndicationsEnabled;
    }
    portEXIT_CRITICAL(&g_callbackStateMux);
    return enabled;
}

void publishApprovalSubscription(std::uint16_t connectionHandle,
                                 bool enabled) {
    portENTER_CRITICAL(&g_callbackStateMux);
    CallbackConnection* connection =
        findCallbackConnectionLocked(connectionHandle);
    if (connection != nullptr && connection->active) {
        connection->approvalIndicationsEnabled = enabled;
    }
    portEXIT_CRITICAL(&g_callbackStateMux);
}

bool anyCallbackConnectionActive() {
    bool active = false;
    portENTER_CRITICAL(&g_callbackStateMux);
    for (const auto& connection : g_callbackConnections) {
        active = active || connection.active;
    }
    portEXIT_CRITICAL(&g_callbackStateMux);
    return active;
}

bool callbackConnectionCurrent(std::uint16_t connectionHandle,
                               std::uint32_t generation) {
    bool current = false;
    portENTER_CRITICAL(&g_callbackStateMux);
    const CallbackConnection* connection =
        findCallbackConnectionLocked(connectionHandle);
    if (connection != nullptr) {
        current = connection->active &&
                  connection->connectionGeneration == generation;
    }
    portEXIT_CRITICAL(&g_callbackStateMux);
    return current;
}

void refreshIndicationTransportBlock() {
    if (g_indicationTransportBlocked &&
        !callbackConnectionCurrent(g_blockedIndicationConnection,
                                   g_blockedIndicationGeneration)) {
        g_indicationTransportBlocked = false;
        g_blockedIndicationConnection = 0;
        g_blockedIndicationGeneration = 0;
    }
}

bool hidChunkCurrent(const vibe::HidChunk& chunk) {
    portENTER_CRITICAL(&g_callbackStateMux);
    const bool current = g_hidStreamTracker.isCurrent(chunk);
    portEXIT_CRITICAL(&g_callbackStateMux);
    return current;
}

void publishHidEnqueueResult(const vibe::HidStreamToken& attempted,
                             vibe::EnqueueResult result) {
    if (result != vibe::EnqueueResult::QueueFull) {
        return;
    }
    portENTER_CRITICAL(&g_callbackStateMux);
    g_hidStreamTracker.noteEnqueueResult(attempted, result);
    CallbackConnection* connection =
        findCallbackConnectionLocked(attempted.connectionHandle);
    if (connection != nullptr && connection->active &&
        connection->connectionGeneration == attempted.connectionGeneration &&
        !g_hidStreamTracker.current(attempted.connectionHandle)
             .acceptingChunks) {
        connection->hidDisconnectPending = true;
        connection->hidDisconnectGeneration =
            attempted.connectionGeneration;
    }
    portEXIT_CRITICAL(&g_callbackStateMux);
}

void publishApprovalOverflow(std::uint16_t connectionHandle,
                             std::uint32_t generation) {
    portENTER_CRITICAL(&g_callbackStateMux);
    CallbackConnection* connection =
        findCallbackConnectionLocked(connectionHandle);
    if (connection != nullptr &&
        connection->connectionGeneration == generation) {
        connection->approvalOverflowPending = true;
        connection->approvalOverflowGeneration = generation;
    }
    portEXIT_CRITICAL(&g_callbackStateMux);
}

bool sendIndicationPayload(const std::uint8_t* payload, std::size_t length,
                           std::uint16_t connectionHandle,
                           std::uint32_t connectionGeneration,
                           std::uint32_t nowMs) {
    refreshIndicationTransportBlock();
    if (g_approvalResult == nullptr || payload == nullptr || length == 0 ||
        length > g_pendingIndication.payload.size() ||
        g_pendingIndication.active || g_indicationTransportBlocked ||
        !callbackConnectionCurrent(connectionHandle,
                                   connectionGeneration)) {
        return false;
    }

    std::memcpy(g_pendingIndication.payload.data(), payload, length);
    g_pendingIndication.connectionHandle = connectionHandle;
    g_pendingIndication.connectionGeneration = connectionGeneration;
    g_pendingIndication.length = static_cast<std::uint16_t>(length);
    g_pendingIndication.deadlineMs = nowMs + kIndicationDeliveryTimeoutMs;
    g_pendingIndication.deliveryId = g_nextIndicationDeliveryId++;
    if (g_nextIndicationDeliveryId == 0) {
        g_nextIndicationDeliveryId = 1;
    }
    g_pendingIndication.active = true;
    portENTER_CRITICAL(&g_callbackStateMux);
    g_callbackDeliveryId = g_pendingIndication.deliveryId;
    portEXIT_CRITICAL(&g_callbackStateMux);
    if (!g_approvalResult->indicate(g_pendingIndication.payload.data(), length,
                                    connectionHandle)) {
        portENTER_CRITICAL(&g_callbackStateMux);
        if (g_callbackDeliveryId == g_pendingIndication.deliveryId) {
            g_callbackDeliveryId = 0;
        }
        portEXIT_CRITICAL(&g_callbackStateMux);
        g_pendingIndication = {};
        return false;
    }
    return true;
}

bool queueApprovalDecision(const vibe::ApprovalDecisionV2& decision,
                           std::uint16_t connectionHandle,
                           std::uint32_t connectionGeneration,
                           std::uint32_t nowMs) {
    char json[vibe::kIngressPayloadLength]{};
    std::size_t written = 0;
    if (!vibe::encodeApprovalDecision(decision, json, sizeof(json), written)) {
        return false;
    }
    return sendIndicationPayload(
        reinterpret_cast<const std::uint8_t*>(json), written,
        connectionHandle, connectionGeneration, nowMs);
}

bool queueProtocolError(const char* requestId, vibe::ProtocolErrorCode code,
                        const char* message, std::uint16_t connectionHandle,
                        std::uint32_t connectionGeneration,
                        std::uint32_t nowMs) {
    char json[vibe::kIngressPayloadLength]{};
    std::size_t written = 0;
    if (!vibe::encodeProtocolError(requestId, code, message, json,
                                   sizeof(json), written)) {
        return false;
    }
    return sendIndicationPayload(
        reinterpret_cast<const std::uint8_t*>(json), written,
        connectionHandle, connectionGeneration, nowMs);
}

void presentApprovalRequest(const vibe::ApprovalRequestV2& request,
                            std::uint32_t nowMs) {
    switch (request.card) {
        case vibe::AgentCardId::Codex: g_currentCard = CARD_CODEX; break;
        case vibe::AgentCardId::Workbuddy: g_currentCard = CARD_WORKBUDDY; break;
        case vibe::AgentCardId::Antigravity: g_currentCard = CARD_ANTIGRAVITY; break;
    }
    g_v2ApprovalActive = true;
    g_approval.active = true;
    g_approval.agentId = request.agentId;
    std::memcpy(g_approval.type, request.operationType,
                sizeof(g_approval.type));
    std::memcpy(g_approval.summary, request.summary,
                sizeof(g_approval.summary));
    g_approval.triggeredAtMs = nowMs;
    g_lastApprovalSwitchedAtMs = nowMs;
    noteActivity();
    vibrate(250, 90);
    playSe(1250.0f, 75);
    g_uiDirty = true;
    Serial.printf("approval accepted request_id=%.8s\n", request.requestId);
}

void decidePendingApproval(vibe::ApprovalChoice choice) {
    if (g_pendingIndication.active || g_indicationTransportBlocked) {
        Serial.println("approval decision deferred: indication busy");
        return;
    }
    const std::uint32_t nowMs = millis();
    if (!callbackConnectionCurrent(g_pendingApprovalConnection,
                                   g_pendingApprovalGeneration)) {
        const auto cancelled = g_approvalController.cancel(nowMs);
        if (cancelled.hasValue) {
            Serial.printf("approval cancelled before input request_id=%.8s\n",
                          cancelled.value.requestId);
            emitFreeHeapDiagnostic(HeapDiagnosticEvent::Cancelled);
        }
        g_v2ApprovalActive = false;
        g_approval.active = false;
        g_pendingApprovalConnection = 0;
        g_pendingApprovalGeneration = 0;
        g_uiDirty = true;
        return;
    }
    const auto expired = g_approvalController.expireIfNeeded(nowMs);
    if (expired.hasValue) {
        queueApprovalDecision(expired.value, g_pendingApprovalConnection,
                              g_pendingApprovalGeneration, nowMs);
        g_v2ApprovalActive = false;
        g_approval.active = false;
        g_pendingApprovalConnection = 0;
        g_pendingApprovalGeneration = 0;
        g_uiDirty = true;
        emitFreeHeapDiagnostic(HeapDiagnosticEvent::Expired);
        return;
    }
    const auto decision = g_approvalController.decide(choice, nowMs);
    if (!decision.hasValue) {
        return;
    }

    if (!queueApprovalDecision(decision.value, g_pendingApprovalConnection,
                               g_pendingApprovalGeneration, nowMs)) {
        Serial.printf("approval decision transport_error request_id=%.8s\n",
                      decision.value.requestId);
    }
    g_v2ApprovalActive = false;
    g_approval.active = false;
    g_pendingApprovalConnection = 0;
    g_pendingApprovalGeneration = 0;
    if (choice == vibe::ApprovalChoice::Approve) {
        playSe(1350.0f, 60);
        vibrate(180, 50);
    } else {
        playSe(450.0f, 60);
        vibrate(120, 35);
    }
    g_uiDirty = true;
    emitFreeHeapDiagnostic(
        choice == vibe::ApprovalChoice::Approve
            ? HeapDiagnosticEvent::Approved
            : HeapDiagnosticEvent::Rejected);
}

void expirePendingApproval(std::uint32_t nowMs) {
    refreshIndicationTransportBlock();
    if (g_pendingIndication.active) {
        return;
    }
    const vibe::ApprovalIteration iteration(nowMs);
    const auto expired = iteration.expire(g_approvalController);
    if (!expired.hasValue) {
        return;
    }
    if (!queueApprovalDecision(expired.value, g_pendingApprovalConnection,
                               g_pendingApprovalGeneration, nowMs)) {
        Serial.printf("approval expired transport_error request_id=%.8s\n",
                      expired.value.requestId);
    }
    g_v2ApprovalActive = false;
    g_approval.active = false;
    g_pendingApprovalConnection = 0;
    g_pendingApprovalGeneration = 0;
    g_uiDirty = true;
    emitFreeHeapDiagnostic(HeapDiagnosticEvent::Expired);
}

void processApprovalIngress(const vibe::IngressMessage& ingress,
                            std::uint32_t nowMs) {
    const auto decoded = vibe::decodeApprovalRequest(
        ingress.payload.data(), ingress.length, true);
    if (decoded.error != vibe::ProtocolError::None) {
        const auto code = decoded.error == vibe::ProtocolError::UnsupportedVersion
                              ? vibe::ProtocolErrorCode::UnsupportedVersion
                              : vibe::ProtocolErrorCode::InvalidPayload;
        if (!queueProtocolError("", code, "approval request rejected",
                                ingress.connectionHandle,
                                ingress.connectionGeneration, nowMs)) {
            Serial.println("approval error delivery transport_error");
        }
        return;
    }

    if (decoded.legacy) {
        JsonDocument document;
        if (!deserializeJson(document, ingress.payload.data(), ingress.length)) {
            applyApprovalRequest(document["params"]);
        }
        return;
    }

    if (!approvalIndicationsEnabled(ingress.connectionHandle,
                                    ingress.connectionGeneration)) {
        if (!queueProtocolError(decoded.request.requestId,
                                vibe::ProtocolErrorCode::TransportError,
                                "approval indications are not enabled",
                                ingress.connectionHandle,
                                ingress.connectionGeneration, nowMs)) {
            Serial.printf("approval transport_error request_id=%.8s\n",
                          decoded.request.requestId);
        }
        return;
    }

    refreshIndicationTransportBlock();
    if (g_pendingIndication.active || g_indicationTransportBlocked) {
        Serial.printf("approval transport busy request_id=%.8s\n",
                      decoded.request.requestId);
        return;
    }

    const vibe::ApprovalIteration iteration(nowMs);
    const auto accepted = iteration.accept(g_approvalController,
                                           decoded.request);
    if (accepted == vibe::ApprovalAcceptResult::Busy) {
        queueProtocolError(decoded.request.requestId,
                           vibe::ProtocolErrorCode::Busy,
                           "another request is pending",
                           ingress.connectionHandle,
                           ingress.connectionGeneration, nowMs);
        return;
    }
    if (accepted == vibe::ApprovalAcceptResult::Duplicate) {
        Serial.printf("approval duplicate request_id=%.8s\n",
                      decoded.request.requestId);
        return;
    }

    g_pendingApprovalConnection = ingress.connectionHandle;
    g_pendingApprovalGeneration = ingress.connectionGeneration;
    presentApprovalRequest(decoded.request, nowMs);
}

void processIngressMessage(const vibe::IngressMessage& ingress,
                           std::uint32_t nowMs) {
    if (ingress.kind == vibe::IngressKind::Disconnect) {
        g_hidRpcAssembler.clear(ingress.connectionHandle);
        g_connected = anyCallbackConnectionActive();
        g_activeTouch = -1;
        g_uiDirty = true;
        if (g_approvalController.pending() &&
            g_pendingApprovalConnection == ingress.connectionHandle &&
            g_pendingApprovalGeneration == ingress.connectionGeneration) {
            const auto cancelled = g_approvalController.cancel(nowMs);
            g_v2ApprovalActive = false;
            g_approval.active = false;
            g_pendingApprovalConnection = 0;
            g_pendingApprovalGeneration = 0;
            g_uiDirty = true;
            if (cancelled.hasValue &&
                !queueApprovalDecision(cancelled.value,
                                       ingress.connectionHandle,
                                       ingress.connectionGeneration, nowMs)) {
                Serial.printf("approval cancelled transport_error request_id=%.8s\n",
                              cancelled.value.requestId);
            }
            if (cancelled.hasValue) {
                emitFreeHeapDiagnostic(HeapDiagnosticEvent::Cancelled);
            }
        }
        NimBLEDevice::startAdvertising();
        Serial.printf("BLE disconnected: handle=%u\n",
                      ingress.connectionHandle);
        return;
    }

    if (!callbackConnectionCurrent(ingress.connectionHandle,
                                   ingress.connectionGeneration)) {
        return;
    }

    if (ingress.length == 0) {
        return;
    }
    if (ingress.kind == vibe::IngressKind::Approval) {
        processApprovalIngress(ingress, nowMs);
        return;
    }

    JsonDocument document;
    const DeserializationError error = deserializeJson(
        document, ingress.payload.data(), ingress.length);
    if (error) {
        Serial.printf("quota invalid_payload: %s\n", error.c_str());
        return;
    }
    applyQuotaStatus(document.as<JsonVariantConst>());
}

void processHidChunkInMainLoop(const vibe::HidChunk& chunk) {
    if (!hidChunkCurrent(chunk)) {
        return;
    }
    vibe::HidRpcView completed{};
    const auto result = g_hidRpcAssembler.consume(chunk, completed);
    if (!hidChunkCurrent(chunk)) {
        g_hidRpcAssembler.clear(chunk.connectionHandle);
        return;
    }
    if (result == vibe::HidConsumeResult::Complete) {
        processRpc(completed.data, completed.length,
                   completed.connectionHandle);
    } else if (result != vibe::HidConsumeResult::Incomplete) {
        Serial.printf("HID RPC discarded: %u\n",
                      static_cast<unsigned>(result));
    }
}

bool takeIndicationStatus(IndicationStatusHandoff& status) {
    bool available = false;
    portENTER_CRITICAL(&g_callbackStateMux);
    if (g_indicationStatusHandoff.pending) {
        status = g_indicationStatusHandoff;
        g_indicationStatusHandoff.pending = false;
        if (g_callbackDeliveryId == status.deliveryId) {
            g_callbackDeliveryId = 0;
        }
        available = true;
    }
    portEXIT_CRITICAL(&g_callbackStateMux);
    return available;
}

void processIndicationTransport(std::uint32_t nowMs) {
    IndicationStatusHandoff status{};
    if (takeIndicationStatus(status)) {
        if (g_pendingIndication.active &&
            status.deliveryId == g_pendingIndication.deliveryId) {
            if (status.code != BLE_HS_EDONE) {
                Serial.printf("approval indication transport_error status=%d\n",
                              status.code);
            }
            g_pendingIndication = {};
        }
    }
    if (vibe::indicationTimeoutRequiresDisconnect(
            g_pendingIndication.active, nowMs,
            g_pendingIndication.deadlineMs)) {
        Serial.println("approval indication transport_error timeout");
        portENTER_CRITICAL(&g_callbackStateMux);
        if (g_callbackDeliveryId == g_pendingIndication.deliveryId) {
            g_callbackDeliveryId = 0;
        }
        portEXIT_CRITICAL(&g_callbackStateMux);
        g_indicationTransportBlocked = true;
        g_blockedIndicationConnection =
            g_pendingIndication.connectionHandle;
        g_blockedIndicationGeneration =
            g_pendingIndication.connectionGeneration;
        g_pendingIndication = {};
    }
    refreshIndicationTransportBlock();
    if (g_indicationTransportBlocked && g_server != nullptr &&
        callbackConnectionCurrent(g_blockedIndicationConnection,
                                  g_blockedIndicationGeneration)) {
        g_server->disconnect(g_blockedIndicationConnection);
    }
}

void processBleLifecycleEvents(std::uint32_t nowMs) {
    std::array<CallbackConnection, 6> snapshot{};
    bool pairingSuccess = false;
    portENTER_CRITICAL(&g_callbackStateMux);
    snapshot = g_callbackConnections;
    for (auto& connection : g_callbackConnections) {
        connection.configurePending = false;
        connection.disconnectFallbackPending = false;
        connection.hidDisconnectPending = false;
        connection.approvalOverflowPending = false;
    }
    pairingSuccess = g_pairingSuccessPending;
    g_pairingSuccessPending = false;
    portEXIT_CRITICAL(&g_callbackStateMux);

    bool connectionChanged = false;
    bool anyConnected = false;
    for (const auto& connection : snapshot) {
        anyConnected = anyConnected || connection.active;
        if (connection.active && connection.configurePending) {
            const std::uint16_t connectionHandle =
                connection.connectionHandle;
            if (g_server != nullptr) {
                g_server->updateConnParams(connectionHandle, 12, 24, 0, 180);
            }
            connectionChanged = true;
            Serial.printf("BLE connected: handle=%u\n", connectionHandle);
        }
        if (connection.disconnectFallbackPending) {
            vibe::IngressMessage disconnect{};
            disconnect.kind = vibe::IngressKind::Disconnect;
            disconnect.connectionHandle = connection.connectionHandle;
            disconnect.connectionGeneration =
                connection.disconnectedGeneration;
            processIngressMessage(disconnect, nowMs);
            Serial.printf("disconnect queue_full fallback handle=%u\n",
                          connection.connectionHandle);
        }
        if (connection.hidDisconnectPending) {
            g_hidRpcAssembler.clear(connection.connectionHandle);
            if (g_server != nullptr &&
                callbackConnectionCurrent(
                    connection.connectionHandle,
                    connection.hidDisconnectGeneration)) {
                if (!g_server->disconnect(connection.connectionHandle)) {
                    portENTER_CRITICAL(&g_callbackStateMux);
                    CallbackConnection* retry =
                        findCallbackConnectionLocked(
                            connection.connectionHandle);
                    if (retry != nullptr && retry->active &&
                        retry->connectionGeneration ==
                            connection.hidDisconnectGeneration) {
                        retry->hidDisconnectPending = true;
                        retry->hidDisconnectGeneration =
                            connection.hidDisconnectGeneration;
                    }
                    portEXIT_CRITICAL(&g_callbackStateMux);
                }
            }
        }
        if (connection.approvalOverflowPending &&
            !queueProtocolError("", vibe::ProtocolErrorCode::QueueFull,
                                "approval ingress queue is full",
                                connection.connectionHandle,
                                connection.approvalOverflowGeneration,
                                nowMs)) {
            Serial.printf("approval queue_full handle=%u\n",
                          connection.connectionHandle);
        }
    }
    g_connected = anyConnected;
    refreshIndicationTransportBlock();
    if (connectionChanged) {
        g_uiDirty = true;
    }
    if (pairingSuccess) {
        playSe(1320.0f, 95);
        vibrate(220, 75);
        Serial.println("BLE pairing authenticated");
    }
}

// BLE output callbacks only validate and copy fixed-size fragments. Completion
// detection and JSON parsing happen in processHidChunkInMainLoop().
class RpcOutputCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic,
                 NimBLEConnInfo& connection) override {
        const NimBLEAttValue& value =
            static_cast<NoCopyCharacteristic*>(characteristic)
                ->valueReference();
        const auto* data = value.data();
        const std::size_t length = value.size();
        if (data == nullptr || length < 2 || data[0] != vibe::kChannelJsonRpc) {
            return;
        }

        const std::size_t chunkLength = data[1];
        if (chunkLength == 0 || chunkLength > vibe::kRpcChunkLength ||
            chunkLength > length - 2) {
            return;
        }
        const auto token = currentConnectionToken(connection.getConnHandle());
        if (!token.active || !token.acceptingChunks) {
            return;
        }
        const auto result = vibe::enqueueHidChunk(
            token.connectionHandle, token.connectionGeneration,
            token.streamEpoch, data + 2, chunkLength);
        publishHidEnqueueResult(token, result);
    }
};

class GattWriteCallbacks : public NimBLECharacteristicCallbacks {
  public:
    GattWriteCallbacks(vibe::IngressKind kind, bool requireBond)
        : kind_(kind), requireBond_(requireBond) {}

    void onWrite(NimBLECharacteristic* characteristic,
                 NimBLEConnInfo& connection) override {
        const bool encrypted = connection.isEncrypted();
        const bool bonded = connection.isBonded();
        if (!encrypted || (requireBond_ && !bonded)) {
            if (requireBond_ && g_server != nullptr) {
                g_server->disconnect(connection.getConnHandle());
            }
            return;
        }

        const auto token = currentConnectionToken(connection.getConnHandle());
        if (!token.active) {
            return;
        }
        const NimBLEAttValue& value =
            static_cast<NoCopyCharacteristic*>(characteristic)
                ->valueReference();
        const auto result = vibe::enqueueGattWrite(
            kind_, token.connectionHandle, token.connectionGeneration,
            value.data(), value.size(), true);
        if (kind_ == vibe::IngressKind::Approval &&
            result == vibe::EnqueueResult::QueueFull) {
            publishApprovalOverflow(token.connectionHandle,
                                    token.connectionGeneration);
        }
    }

  private:
    vibe::IngressKind kind_;
    bool requireBond_;
};

class ApprovalResultCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& connection,
                     std::uint16_t subscription) override {
        publishApprovalSubscription(connection.getConnHandle(),
                                    (subscription & 0x0002U) != 0);
    }

    void onStatus(NimBLECharacteristic*, int code) override {
        portENTER_CRITICAL(&g_callbackStateMux);
        if (g_callbackDeliveryId != 0) {
            g_indicationStatusHandoff.pending = true;
            g_indicationStatusHandoff.code = code;
            g_indicationStatusHandoff.deliveryId = g_callbackDeliveryId;
        }
        portEXIT_CRITICAL(&g_callbackStateMux);
    }
};

// Connection callbacks keep the UI synchronized with pairing state and resume
// advertising automatically after a disconnect.
class HidServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& connection) override {
        publishConnectionConnected(connection.getConnHandle());
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& connection,
                      int reason) override {
        const auto token = currentConnectionToken(connection.getConnHandle());
        const auto result = vibe::enqueueGattWrite(
            vibe::IngressKind::Disconnect, token.connectionHandle,
            token.connectionGeneration, nullptr, 0);
        publishConnectionDisconnected(
            token.connectionHandle, token.connectionGeneration,
            result != vibe::EnqueueResult::Accepted);
        (void)reason;
    }

    void onAuthenticationComplete(NimBLEConnInfo& connection) override {
        if (!connection.isEncrypted()) {
            Serial.println("BLE encryption failed");
            NimBLEDevice::getServer()->disconnect(connection.getConnHandle());
            return;
        }
        // Defer hardware feedback to Arduino's main loop; NimBLE owns this
        // callback task and should only publish the successful result.
        portENTER_CRITICAL(&g_callbackStateMux);
        g_pairingSuccessPending = true;
        portEXIT_CRITICAL(&g_callbackStateMux);
    }
};

RpcOutputCallbacks g_rpcCallbacks;
GattWriteCallbacks g_quotaCallbacks(vibe::IngressKind::Quota, false);
GattWriteCallbacks g_approvalCallbacks(vibe::IngressKind::Approval, true);
ApprovalResultCallbacks g_approvalResultCallbacks;
HidServerCallbacks g_serverCallbacks;

void addDeviceInfoCharacteristic(std::uint16_t uuid, const char* value) {
    auto* characteristic = g_hid->getDeviceInfoService()->createCharacteristic(uuid, NIMBLE_PROPERTY::READ);
    characteristic->setValue(value);
}

void initializeBle() {
    // Expose standard keyboard/consumer/pointer reports plus the vendor report
    // used for agent status, actions, and request/response messages.
    NimBLEDevice::init(g_deviceName);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityAuth(true, false, true);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&g_serverCallbacks);
    g_hid = new NimBLEHIDDevice(g_server);

    g_hid->setManufacturer(vibe::kManufacturer);
    g_hid->setPnp(0x01, vibe::kVendorId, vibe::kProductId, vibe::kProductVersion);
    g_hid->setHidInfo(0x00, 0x01);
    g_hid->setReportMap(vibe::kReportMap, sizeof(vibe::kReportMap));

    char serial[17];
    std::snprintf(serial, sizeof(serial), "%016llX", ESP.getEfuseMac());
    addDeviceInfoCharacteristic(0x2A24, vibe::kModelNumber);
    addDeviceInfoCharacteristic(0x2A25, serial);
    addDeviceInfoCharacteristic(0x2A26, vibe::kFirmwareVersion);

    g_keyboardInput = g_hid->getInputReport(1);
    g_consumerInput = g_hid->getInputReport(2);
    auto* pointerInput = g_hid->getInputReport(3);
    g_vendorInput = g_hid->getInputReport(vibe::kVendorReportId);
    auto* vendorOutput = addNoCopyCharacteristic(
        g_hid->getHidService(), NimBLEUUID(static_cast<std::uint16_t>(0x2A4D)),
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
            NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ_ENC |
            NIMBLE_PROPERTY::WRITE_ENC,
        vibe::kBleReportLength);
    auto* vendorOutputDescriptor = vendorOutput->createDescriptor(
        NimBLEUUID(static_cast<std::uint16_t>(0x2908)),
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
            NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE_ENC);
    const std::uint8_t vendorOutputDescriptorValue[] = {
        vibe::kVendorReportId, 0x02};
    vendorOutputDescriptor->setValue(vendorOutputDescriptorValue,
                                     sizeof(vendorOutputDescriptorValue));
    g_vendorOutput = vendorOutput;
    g_hid->getFeatureReport(vibe::kVendorReportId);

    const std::uint8_t keyboardIdle[8] = {};
    const std::uint8_t consumerIdle[2] = {};
    const std::uint8_t pointerIdle[5] = {};
    const std::uint8_t vendorIdle[vibe::kBleReportLength] = {};
    if (g_keyboardInput != nullptr) {
        g_keyboardInput->setValue(keyboardIdle, sizeof(keyboardIdle));
    }
    if (g_consumerInput != nullptr) {
        g_consumerInput->setValue(consumerIdle, sizeof(consumerIdle));
    }
    pointerInput->setValue(pointerIdle, sizeof(pointerIdle));
    g_vendorInput->setValue(vendorIdle, sizeof(vendorIdle));
    g_vendorOutput->setCallbacks(&g_rpcCallbacks);

    updateBattery(false);

    auto* quotaService = g_server->createService(vibe::kQuotaServiceUuid);
    auto* quotaCharacteristic = addNoCopyCharacteristic(
        quotaService, NimBLEUUID(vibe::kQuotaWriteUuid),
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,
        512);
    quotaCharacteristic->setCallbacks(&g_quotaCallbacks);
    auto* approvalCharacteristic = addNoCopyCharacteristic(
        quotaService, NimBLEUUID(vibe::kApprovalWriteUuid),
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,
        512);
    approvalCharacteristic->setCallbacks(&g_approvalCallbacks);
    g_approvalResult = quotaService->createCharacteristic(
        vibe::kApprovalResultUuid,
        NIMBLE_PROPERTY::INDICATE | NIMBLE_PROPERTY::READ_ENC,
        512);
    g_approvalResult->setCallbacks(&g_approvalResultCallbacks);

    if (!g_server->start()) {
        Serial.println("Failed to start BLE GATT server");
        return;
    }

    auto* advertising = NimBLEDevice::getAdvertising();
    advertising->setName(g_deviceName);
    advertising->setAppearance(HID_KEYBOARD);
    advertising->addServiceUUID(g_hid->getHidService()->getUUID());
    advertising->addServiceUUID(vibe::kQuotaServiceUuid);
    advertising->enableScanResponse(true);
    advertising->start();
    Serial.printf("BLE HID advertising started as %s\n", g_deviceName);
}

void beginPairing() {
    // A slot is a separate advertised identity. Clear every stored bond before
    // restarting so macOS discovers the selected slot with a clean GATT cache.
    saveDeviceSlot(g_pendingDeviceSlot);

    if (g_server != nullptr) {
        const auto peers = g_server->getPeerDevices();
        for (const auto connectionHandle : peers) {
            g_server->disconnect(connectionHandle);
        }
    }
    NimBLEDevice::deleteAllBonds();

    g_connected = false;
    g_deviceSlot = g_pendingDeviceSlot;
    std::snprintf(g_deviceName, sizeof(g_deviceName), "%s%d", vibe::kDeviceNamePrefix, g_deviceSlot);
    g_restartAt = millis() + 900;
    g_uiDirty = true;
    Serial.printf("Pairing requested for %s; restarting\n", g_deviceName);
}

// -----------------------------------------------------------------------------
// Circular layout and agent-selection animation
// -----------------------------------------------------------------------------

void initializeAgentPositions() {
    for (int i = 0; i < kAgentCount; ++i) {
        // Flat-top hexagon: leaves a readable status area at the bottom while
        // pushing all six agent buttons toward the circular bezel.
        const float angle = (-120.0f + 60.0f * i) * PI / 180.0f;
        agentX[i] = kScreenCenter + static_cast<int>(std::cos(angle) * kAgentOrbitRadius);
        agentY[i] = kScreenCenter + static_cast<int>(std::sin(angle) * kAgentOrbitRadius);
    }
    // FAST, OK, NG, PLAN, AI. OK/NG sit directly below the left/right
    // physical buttons; the other three follow the lower circular edge.
    actionX = {70, 354, 112, 233, 396};
    actionY = {250, 105, 105, 368, 250};
    g_selectionX = static_cast<float>(agentX[g_selectedAgent]);
    g_selectionY = static_cast<float>(agentY[g_selectedAgent]);
    g_selectionFromX = g_selectionToX = g_selectionX;
    g_selectionFromY = g_selectionToY = g_selectionY;
    g_selectionFromAngle = g_selectionToAngle =
        std::atan2(g_selectionY - kScreenCenter, g_selectionX - kScreenCenter);
}

float selectionProgress(std::uint32_t now) {
    if (!g_selectionAnimating) {
        return 1.0f;
    }
    return clamp01(static_cast<float>(now - g_selectionAnimationStartedAt) /
                   static_cast<float>(g_selectionAnimationDurationMs));
}

float snappySelectionProgress(float progress) {
    // Ease-out-back moves decisively, overshoots by a few pixels, then snaps
    // onto the target like a spring-loaded watch mechanism.
    if (progress >= 1.0f) {
        return 1.0f;
    }
    constexpr float kOvershoot = 1.10f;
    const float shifted = progress - 1.0f;
    return 1.0f + (kOvershoot + 1.0f) * shifted * shifted * shifted +
           kOvershoot * shifted * shifted;
}

void selectionPositionAt(std::uint32_t now, float& x, float& y) {
    const float eased = snappySelectionProgress(selectionProgress(now));
    const float angle = g_selectionFromAngle +
                        (g_selectionToAngle - g_selectionFromAngle) * eased;
    x = kScreenCenter + std::cos(angle) * kAgentOrbitRadius;
    y = kScreenCenter + std::sin(angle) * kAgentOrbitRadius;
}

void selectAgent(int index) {
    if (index < 0 || index >= kAgentCount || index == g_selectedAgent) {
        return;
    }
    const std::uint32_t now = millis();
    selectionPositionAt(now, g_selectionX, g_selectionY);
    const float currentAngle = std::atan2(g_selectionY - kScreenCenter,
                                          g_selectionX - kScreenCenter);
    const float targetAngle = std::atan2(static_cast<float>(agentY[index] - kScreenCenter),
                                         static_cast<float>(agentX[index] - kScreenCenter));
    float angleDelta = targetAngle - currentAngle;
    while (angleDelta > PI) {
        angleDelta -= 2.0f * PI;
    }
    while (angleDelta < -PI) {
        angleDelta += 2.0f * PI;
    }
    g_selectionFromX = g_selectionX;
    g_selectionFromY = g_selectionY;
    g_selectionToX = static_cast<float>(agentX[index]);
    g_selectionToY = static_cast<float>(agentY[index]);
    g_selectionFromAngle = currentAngle;
    g_selectionToAngle = currentAngle + angleDelta;
    // Nearby steps complete in about 123 ms; even a half-turn finishes under
    // 200 ms so physical-button navigation feels immediate.
    g_selectionAnimationDurationMs = kSelectionAnimationBaseMs +
        static_cast<std::uint32_t>(105.0f * std::abs(angleDelta) / PI);
    g_selectionAnimationStartedAt = now;
    g_selectionAnimating = true;
    g_selectedAgent = index;
    g_uiDirty = true;
}

// -----------------------------------------------------------------------------
// Touch and physical-button input
// -----------------------------------------------------------------------------

bool pointInRect(int x, int y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

int hitTestSettings(int x, int y) {
    const int backDx = x - kSettingsCloseX;
    const int backDy = y - kSettingsCloseY;
    if (backDx * backDx + backDy * backDy <= kSettingsCloseRadius * kSettingsCloseRadius) {
        return kTouchSettingsBack;
    }
    for (int i = 0; i < 3; ++i) {
        const int dx = x - (122 + i * 111);
        const int dy = y - 112;
        if (dx * dx + dy * dy <= 34 * 34) {
            return kTouchSlot1 + i;
        }
    }
    if (pointInRect(x, y, 153, 151, 160, 45)) {
        return kTouchPair;
    }
    if (pointInRect(x, y, 80, 205, 306, 55)) {
        return kTouchVolume;
    }
    if (pointInRect(x, y, 80, 267, 306, 55)) {
        return kTouchVibrationStrength;
    }
    if (pointInRect(x, y, 95, 355, 120, 50)) {
        return kTouchAgentStateVibe;
    }
    if (pointInRect(x, y, 226, 355, 148, 50)) {
        return kTouchLanguage;
    }
    return -1;
}

int hitTestMain(int x, int y) {
    const int settingsDx = x - kSettingsX;
    const int settingsDy = y - kSettingsY;
    if (!g_actionLayer &&
        settingsDx * settingsDx + settingsDy * settingsDy <= kSettingsRadius * kSettingsRadius) {
        return kTouchSettings;
    }
    const int outerCount = g_actionLayer ? kActionCount : kAgentCount;
    for (int i = 0; i < outerCount; ++i) {
        const int dx = x - (g_actionLayer ? actionX[i] : agentX[i]);
        const int dy = y - (g_actionLayer ? actionY[i] : agentY[i]);
        if (dx * dx + dy * dy <= kAgentButtonRadius * kAgentButtonRadius) {
            return i;
        }
    }
    const int dx = x - kScreenCenter;
    const int dy = y - kScreenCenter;
    if (dx * dx + dy * dy <= kMicButtonRadius * kMicButtonRadius) {
        return kTouchMic;
    }
    return -1;
}

void updateVolumeFromTouch(int x) {
    const int boundedX = std::max(kSettingsSliderLeft, std::min(kSettingsSliderRight, x));
    g_seVolume = static_cast<std::uint8_t>(
        (boundedX - kSettingsSliderLeft) * 255 /
        (kSettingsSliderRight - kSettingsSliderLeft));
    M5.Speaker.setVolume(g_seVolume);
    g_uiDirty = true;
}

void updateVibrationStrengthFromTouch(int x) {
    const int boundedX = std::max(kSettingsSliderLeft, std::min(kSettingsSliderRight, x));
    g_vibrationStrength = static_cast<std::uint8_t>(
        (boundedX - kSettingsSliderLeft) * 255 /
        (kSettingsSliderRight - kSettingsSliderLeft));
    g_uiDirty = true;
}

void sendOuterActionEvent(int index, bool pressed) {
    if (index == 3 && pressed) {
        g_planModeEnabled = !g_planModeEnabled;
        g_uiDirty = true;
    }
    sendActionEvent(index == 4 ? 12 : 6 + index, pressed);
}

void playOuterActionPressSe(int index) {
    if (index == kOkAction) {
        playSe(659.25f, 34);
        delay(42);
        playSe(987.77f, 86);
        return;
    }
    if (index == kNgAction) {
        playSe(392.00f, 42);
        delay(50);
        playSe(293.66f, 105);
        return;
    }
    playSe(900.0f + index * 75.0f, 40);
}

void handleSettingsTouch(const m5::Touch_Class::touch_detail_t& touch) {
    if (touch.wasPressed()) {
        g_activeTouch = hitTestSettings(touch.x, touch.y);
        if (g_activeTouch == kTouchSettingsBack) {
            g_settingsOpen = false;
            playSe(540.0f);
            vibrate(80, 20);
        } else if (g_activeTouch >= kTouchSlot1 && g_activeTouch <= kTouchSlot3) {
            g_pendingDeviceSlot = 1 + g_activeTouch - kTouchSlot1;
            playSe(760.0f + g_pendingDeviceSlot * 90.0f);
            vibrate(80, 20);
        } else if (g_activeTouch == kTouchPair) {
            playSe(1100.0f, 55);
            vibrate(150, 35);
        } else if (g_activeTouch == kTouchVolume) {
            updateVolumeFromTouch(touch.x);
        } else if (g_activeTouch == kTouchVibrationStrength) {
            updateVibrationStrengthFromTouch(touch.x);
        } else if (g_activeTouch == kTouchAgentStateVibe) {
            g_agentStateVibeEnabled = !g_agentStateVibeEnabled;
            saveFeedbackSettings();
            playSe(g_agentStateVibeEnabled ? 1040.0f : 620.0f, 45);
            vibrate(130, 28);
        } else if (g_activeTouch == kTouchLanguage) {
            g_language = (g_language == LANG_ZH) ? LANG_EN : LANG_ZH;
            saveLanguage();
            playSe(880.0f, 40);
            vibrate(100, 25);
        }
        g_uiDirty = true;
    }
    if (touch.isPressed()) {
        if (g_activeTouch == kTouchVolume) {
            updateVolumeFromTouch(touch.x);
        } else if (g_activeTouch == kTouchVibrationStrength) {
            updateVibrationStrengthFromTouch(touch.x);
        }
    }
    if (touch.wasReleased() && g_activeTouch >= 0) {
        if (g_activeTouch == kTouchPair) {
            beginPairing();
        } else if (g_activeTouch == kTouchVolume) {
            saveSeVolume();
            playSe(980.0f, 70);
        } else if (g_activeTouch == kTouchVibrationStrength) {
            saveFeedbackSettings();
            vibrate(255, 70);
        }
        g_activeTouch = -1;
        g_uiDirty = true;
    }
}

void handleTouch() {
    const auto touch = M5.Touch.getDetail();
    if (touch.wasPressed() || touch.isPressed() || touch.wasReleased()) {
        noteActivity();
    }

    if (g_approval.active) {
        if (touch.wasPressed()) {
            if (touch.x < kScreenCenter && touch.y > 270) { // Left / NG
                if (g_v2ApprovalActive) {
                    decidePendingApproval(vibe::ApprovalChoice::Reject);
                    return;
                }
                g_approval.active = false;
                sendOuterActionEvent(kNgAction, true);
                delay(20);
                sendOuterActionEvent(kNgAction, false);
                playSe(450.0f, 60);
                vibrate(120, 35);
                g_uiDirty = true;
                return;
            } else if (touch.x >= kScreenCenter && touch.y > 270) { // Right / OK
                if (g_v2ApprovalActive) {
                    decidePendingApproval(vibe::ApprovalChoice::Approve);
                    return;
                }
                g_approval.active = false;
                sendOuterActionEvent(kOkAction, true);
                delay(20);
                sendOuterActionEvent(kOkAction, false);
                playSe(1350.0f, 60);
                vibrate(180, 50);
                g_uiDirty = true;
                return;
            }
        }
        return;
    }

    if (g_settingsOpen) {
        handleSettingsTouch(touch);
        return;
    }

    if (touch.wasPressed()) {
        g_touchStartX = touch.x;
        g_touchStartY = touch.y;
        g_activeSwipe = -1;
        g_activeTouch = hitTestMain(touch.x, touch.y);
        // Remember the layer from touch-down through touch-up. A layer change
        // during the gesture must not release a different host-side control.
        g_touchActionLayer = g_actionLayer;
        if (g_activeTouch >= 0 && g_activeTouch < kAgentCount) {
            if (g_touchActionLayer) {
                g_selectedAction = g_activeTouch;
                sendOuterActionEvent(g_activeTouch, true);
                playOuterActionPressSe(g_activeTouch);
            } else {
                selectAgent(g_activeTouch);
                sendAgentEvent(g_activeTouch, true);
                playSe(820.0f + g_activeTouch * 55.0f);
            }
            vibrate();
        } else if (g_activeTouch == kTouchMic) {
            // The center is always a dedicated PTT button. Send DOWN at the
            // touch edge so AI assistant starts listening immediately.
            sendMicEvent(true);
            playMicSe(true);
            vibrate(150, 35);
        } else if (g_activeTouch == kTouchSettings) {
            g_settingsOpen = true;
            g_pendingDeviceSlot = g_deviceSlot;
            playSe(760.0f);
            vibrate(80, 20);
        }
        g_uiDirty = true;
    }

    if (touch.isPressed() && g_activeSwipe < 0 && g_touchStartX >= 0) {
        const int dx = touch.x - g_touchStartX;
        const int dy = touch.y - g_touchStartY;
        if (dx * dx + dy * dy >= kSwipeThresholdPx * kSwipeThresholdPx) {
            // Cancel any button press that was initiated before swipe
            if (g_activeTouch == kTouchMic) {
                sendMicEvent(false);
                playMicSe(false);
            } else if (g_activeTouch >= 0 && g_activeTouch < kAgentCount) {
                if (g_touchActionLayer) {
                    sendOuterActionEvent(g_activeTouch, false);
                } else {
                    sendAgentEvent(g_activeTouch, false);
                }
            }
            g_activeTouch = -1;

            if (std::abs(dx) > std::abs(dy)) {
                // Horizontal Swipe -> Card Navigation across Codex / Workbuddy / Antigravity
                if (dx < 0) {
                    // Swipe Left -> Next Card
                    g_currentCard = static_cast<AgentCard>((g_currentCard + 1) % CARD_COUNT);
                } else {
                    // Swipe Right -> Prev Card
                    g_currentCard = static_cast<AgentCard>((g_currentCard + CARD_COUNT - 1) % CARD_COUNT);
                }
                saveCurrentCard();
                g_activeSwipe = dx > 0 ? 1 : 3;
                playSe(1150.0f, 35);
                vibrate(100, 30);
                g_uiDirty = true;
                return;
            } else {
                // Vertical Swipe -> Joystick Up / Down
                g_activeSwipe = dy > 0 ? 2 : 0;
                const float angle = dy > 0 ? 0.25f : 0.75f;
                sendJoystickEvent(angle, 1.0f);
                vibrate(120, 30);
                playSe(880.0f, 25);
                g_uiDirty = true;
                return;
            }
        }
    }

    if (touch.wasReleased()) {
        if (g_activeSwipe >= 0) {
            float angle = 0.0f;
            if (g_activeSwipe == 1) angle = 0.00f; // Right
            else if (g_activeSwipe == 2) angle = 0.25f; // Down
            else if (g_activeSwipe == 3) angle = 0.50f; // Left
            else if (g_activeSwipe == 0) angle = 0.75f; // Up
            sendJoystickEvent(angle, 0.0f);
            g_activeSwipe = -1;
            g_activeTouch = -1;
            g_touchStartX = -1;
            g_touchStartY = -1;
            g_uiDirty = true;
            return;
        }
        g_touchStartX = -1;
        g_touchStartY = -1;
        if (g_activeTouch >= 0) {
            if (g_activeTouch < kAgentCount) {
                if (g_touchActionLayer) {
                    sendOuterActionEvent(g_activeTouch, false);
                } else {
                    sendAgentEvent(g_activeTouch, false);
                }
            } else if (g_activeTouch == kTouchMic) {
                sendMicEvent(false);
                playMicSe(false);
            }
            g_activeTouch = -1;
            g_uiDirty = true;
        }
    }
}

void handlePhysicalButtons() {
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
        noteActivity();
    }

    if (g_approval.active) {
        if (M5.BtnA.wasPressed()) { // Left physical button -> Reject
            if (g_v2ApprovalActive) {
                decidePendingApproval(vibe::ApprovalChoice::Reject);
                return;
            }
            g_approval.active = false;
            sendOuterActionEvent(kNgAction, true);
            delay(20);
            sendOuterActionEvent(kNgAction, false);
            playSe(450.0f, 60);
            vibrate(120, 35);
            g_uiDirty = true;
            return;
        }
        if (M5.BtnB.wasPressed()) { // Right physical button -> Approve
            if (g_v2ApprovalActive) {
                decidePendingApproval(vibe::ApprovalChoice::Approve);
                return;
            }
            g_approval.active = false;
            sendOuterActionEvent(kOkAction, true);
            delay(20);
            sendOuterActionEvent(kOkAction, false);
            playSe(1350.0f, 60);
            vibrate(180, 50);
            g_uiDirty = true;
            return;
        }
        return;
    }
    // single-button action. The short grace period prevents an Agent/OK/NG
    // event from leaking out when the user's intention is to switch layers.
    if (!g_buttonChordActive && M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
        if (g_leftAgentPressed >= 0) {
            if (g_leftPressedActionLayer) {
                sendOuterActionEvent(g_leftAgentPressed, false);
            } else {
                sendAgentEvent(g_leftAgentPressed, false);
            }
            g_leftAgentPressed = -1;
        }
        if (g_rightLongTriggered) {
            sendMicEvent(false);
            playMicSe(false);
            g_rightLongTriggered = false;
        }
        g_rightPhysicalPressedAt = 0;
        if (g_rightActionPressed) {
            sendOuterActionEvent(kOkAction, false);
            g_rightActionPressed = false;
        }
        g_leftPressPending = false;
        g_rightActionPending = false;
        g_actionLayer = !g_actionLayer;
        g_selectionAnimating = false;
        g_selectionX = g_selectionToX = static_cast<float>(agentX[g_selectedAgent]);
        g_selectionY = g_selectionToY = static_cast<float>(agentY[g_selectedAgent]);
        g_selectionFromX = g_selectionX;
        g_selectionFromY = g_selectionY;
        g_selectionFromAngle = g_selectionToAngle =
            std::atan2(g_selectionY - kScreenCenter, g_selectionX - kScreenCenter);
        g_buttonChordActive = true;
        playSe(g_actionLayer ? 1120.0f : 680.0f, 48);
        vibrate(170, 42);
        g_uiDirty = true;
        renderUi(millis());
        return;
    }
    if (g_buttonChordActive) {
        if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) {
            g_buttonChordActive = false;
        }
        return;
    }

    if (M5.BtnA.wasPressed()) {
        // Wait briefly so a near-simultaneous right press can become a chord
        // without emitting an unwanted Agent or action click first.
        g_leftPressPending = true;
        g_leftPressedAt = millis();
    }
    if (g_leftPressPending && M5.BtnA.isPressed() &&
        millis() - g_leftPressedAt >= kButtonChordGraceMs) {
        g_leftPressPending = false;
        g_leftPressedActionLayer = g_actionLayer;
        if (g_actionLayer) {
            g_selectedAction = kNgAction;
            g_leftAgentPressed = kNgAction;
            sendOuterActionEvent(g_leftAgentPressed, true);
            playOuterActionPressSe(g_leftAgentPressed);
        } else {
            selectAgent((g_selectedAgent + 1) % kAgentCount);
            g_leftAgentPressed = g_selectedAgent;
            sendAgentEvent(g_leftAgentPressed, true);
            playSe(820.0f + g_selectedAgent * 55.0f, 33);
        }
        vibrate();
        g_uiDirty = true;
    }
    if (M5.BtnA.wasReleased()) {
        if (g_leftPressPending) {
            // Preserve very quick single clicks that end inside the chord
            // grace window.
            g_leftPressPending = false;
            g_leftPressedActionLayer = g_actionLayer;
            if (g_actionLayer) {
                g_selectedAction = kNgAction;
                g_leftAgentPressed = kNgAction;
                sendOuterActionEvent(g_leftAgentPressed, true);
                playOuterActionPressSe(g_leftAgentPressed);
            } else {
                selectAgent((g_selectedAgent + 1) % kAgentCount);
                g_leftAgentPressed = g_selectedAgent;
                sendAgentEvent(g_leftAgentPressed, true);
                playSe(820.0f + g_selectedAgent * 55.0f, 33);
            }
            delay(12);
        }
        if (g_leftAgentPressed >= 0) {
            if (g_leftPressedActionLayer) {
                sendOuterActionEvent(g_leftAgentPressed, false);
            } else {
                sendAgentEvent(g_leftAgentPressed, false);
            }
            g_leftAgentPressed = -1;
        }
        g_leftPressedActionLayer = false;
        g_uiDirty = true;
    }

    if (g_actionLayer && M5.BtnB.wasPressed()) {
        // In Action mode the physical buttons map spatially to OK and NG.
        // Delay NG briefly for the same chord-detection reason as the left key.
        g_rightActionPending = true;
        g_rightActionPressedAt = millis();
    }
    if (g_actionLayer && g_rightActionPending && M5.BtnB.isPressed() &&
        millis() - g_rightActionPressedAt >= kButtonChordGraceMs) {
        g_rightActionPending = false;
        g_rightActionPressed = true;
        g_selectedAction = kOkAction;
        sendOuterActionEvent(kOkAction, true);
        playOuterActionPressSe(kOkAction);
        vibrate();
        g_uiDirty = true;
    }
    if (g_actionLayer && M5.BtnB.wasReleased()) {
        if (g_rightActionPending) {
            g_rightActionPending = false;
            g_selectedAction = kOkAction;
            sendOuterActionEvent(kOkAction, true);
            playOuterActionPressSe(kOkAction);
            delay(12);
            sendOuterActionEvent(kOkAction, false);
            vibrate();
        } else if (g_rightActionPressed) {
            sendOuterActionEvent(kOkAction, false);
            g_rightActionPressed = false;
        }
        g_uiDirty = true;
        return;
    }
    if (g_actionLayer) {
        return;
    }

    if (M5.BtnB.wasPressed()) {
        // Outside Action mode, a right-button tap invokes the assistant and a
        // hold becomes push-to-talk. The threshold keeps both gestures quick.
        g_rightLongTriggered = false;
        g_rightPhysicalPressedAt = millis();
        g_uiDirty = true;
        renderUi(millis());
    }
    if (M5.BtnB.isPressed() && !g_rightLongTriggered &&
        millis() - g_rightPhysicalPressedAt >= kPhysicalMicHoldMs) {
        g_rightLongTriggered = true;
        sendMicEvent(true);
        vibrate(150, 35);
        g_uiDirty = true;
        renderUi(millis());
        playMicSe(true);
    }
    if (M5.BtnB.wasReleased()) {
        if (g_rightLongTriggered) {
            sendMicEvent(false);
            playMicSe(false);
        } else {
            // A short click is the AI assistant key (ACT12).
            sendActionEvent(12, true);
            delay(12);
            sendActionEvent(12, false);
            playSe(1180.0f, 38);
            vibrate(100, 24);
        }
        g_rightLongTriggered = false;
        g_rightPhysicalPressedAt = 0;
        g_uiDirty = true;
        renderUi(millis());
    }
}

// -----------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------

void drawThickCircle(int x, int y, int radius, int thickness, std::uint16_t color) {
    for (int i = 0; i < thickness; ++i) {
        M5.Display.drawCircle(x, y, radius - i, color);
    }
}

void drawThickRoundRect(int x, int y, int width, int height, int radius, int thickness,
                        std::uint16_t color) {
    for (int i = 0; i < thickness; ++i) {
        M5.Display.drawRoundRect(x + i, y + i, width - i * 2, height - i * 2,
                                 std::max(1, radius - i), color);
    }
}

void drawSelectionIndicator(std::uint32_t now) {
    const float rawProgress = selectionProgress(now);
    if (g_selectionAnimating) {
        // Paint oldest samples first. The dim, narrowing rings form a short
        // motion-blur tail without requiring an alpha framebuffer.
        static constexpr std::uint32_t kTrailColors[] = {
            0x30234D, 0x49346F, 0x684A9A, 0x8964C5,
        };
        for (int trail = 4; trail >= 1; --trail) {
            const float sample = std::max(0.0f, rawProgress - trail * 0.11f);
            const float eased = snappySelectionProgress(sample);
            const float angle = g_selectionFromAngle +
                                (g_selectionToAngle - g_selectionFromAngle) * eased;
            const int x = kScreenCenter + static_cast<int>(std::lround(
                std::cos(angle) * kAgentOrbitRadius));
            const int y = kScreenCenter + static_cast<int>(std::lround(
                std::sin(angle) * kAgentOrbitRadius));
            drawThickCircle(x, y, kAgentButtonRadius + 2, std::max(1, 5 - trail),
                            scaledColor(kTrailColors[4 - trail], 1.0f));
        }
    }

    selectionPositionAt(now, g_selectionX, g_selectionY);
    const int x = static_cast<int>(std::lround(g_selectionX));
    const int y = static_cast<int>(std::lround(g_selectionY));
    const float landingProgress = clamp01((rawProgress - 0.62f) / 0.38f);
    const int landingPulse = g_selectionAnimating
        ? static_cast<int>(std::lround(std::sin(landingProgress * PI) * 5.0f))
        : 0;
    const int indicatorRadius = kAgentButtonRadius + landingPulse;
    drawThickCircle(x, y, indicatorRadius + 4, 3, M5.Display.color565(74, 56, 128));
    drawThickCircle(x, y, indicatorRadius, 7, M5.Display.color565(163, 132, 255));
    drawThickCircle(x, y, indicatorRadius - 9, 2, TFT_WHITE);

    if (g_selectionAnimating && rawProgress >= 1.0f) {
        g_selectionAnimating = false;
        g_selectionX = g_selectionFromX = g_selectionToX;
        g_selectionY = g_selectionFromY = g_selectionToY;
        g_selectionFromAngle = g_selectionToAngle;
    }
}

void drawSettingsGlyph(int x, int y, std::uint16_t color) {
    drawThickCircle(x, y, 8, 2, color);
    M5.Display.fillCircle(x, y, 3, color);
    for (int i = 0; i < 8; ++i) {
        const float angle = i * PI / 4.0f;
        const int x1 = x + static_cast<int>(std::cos(angle) * 10);
        const int y1 = y + static_cast<int>(std::sin(angle) * 10);
        const int x2 = x + static_cast<int>(std::cos(angle) * 15);
        const int y2 = y + static_cast<int>(std::sin(angle) * 15);
        M5.Display.drawWideLine(x1, y1, x2, y2, 1.7f, color);
    }
}

void drawMicGlyph(int x, int y, std::uint16_t color) {
    drawThickRoundRect(x - 13, y - 27, 26, 40, 12, 3, color);
    M5.Display.drawWideLine(x - 21, y - 3, x - 21, y + 7, 2.0f, color);
    M5.Display.drawWideLine(x + 21, y - 3, x + 21, y + 7, 2.0f, color);
    M5.Display.drawWideLine(x - 21, y + 7, x - 13, y + 17, 2.0f, color);
    M5.Display.drawWideLine(x + 21, y + 7, x + 13, y + 17, 2.0f, color);
    M5.Display.drawWideLine(x - 13, y + 17, x + 13, y + 17, 2.0f, color);
    M5.Display.drawWideLine(x, y + 17, x, y + 29, 2.0f, color);
    M5.Display.drawWideLine(x - 11, y + 29, x + 11, y + 29, 2.0f, color);
}

void drawLargeMicGlyph(int x, int y, std::uint16_t color) {
    drawThickRoundRect(x - 18, y - 36, 36, 54, 17, 4, color);
    M5.Display.drawWideLine(x - 29, y - 5, x - 29, y + 9, 3.5f, color);
    M5.Display.drawWideLine(x + 29, y - 5, x + 29, y + 9, 3.5f, color);
    M5.Display.drawWideLine(x - 29, y + 9, x - 18, y + 23, 3.5f, color);
    M5.Display.drawWideLine(x + 29, y + 9, x + 18, y + 23, 3.5f, color);
    M5.Display.drawWideLine(x - 18, y + 23, x + 18, y + 23, 3.5f, color);
    M5.Display.drawWideLine(x, y + 23, x, y + 39, 3.5f, color);
    M5.Display.drawWideLine(x - 15, y + 39, x + 15, y + 39, 3.5f, color);
}

void drawAssistantGlyph(int x, int y, std::uint16_t color) {
    drawThickCircle(x, y, 24, 3, color);
    M5.Display.drawWideLine(x - 11, y, x - 3, y - 8, 3.0f, color);
    M5.Display.drawWideLine(x - 3, y - 8, x + 8, y - 5, 3.0f, color);
    M5.Display.drawWideLine(x + 8, y - 5, x + 13, y + 6, 3.0f, color);
    M5.Display.fillCircle(x - 6, y + 7, 2, color);
    M5.Display.fillCircle(x + 7, y + 7, 2, color);
}

void drawCardHeader() {
    if (g_actionLayer) return;
    const auto& card = g_cards[g_currentCard];
    const auto cardColor = scaledColor(card.primaryColor, 1.0f);

    // 3 Pagination dots at y = 22
    constexpr int dotRadius = 4;
    constexpr int dotSpacing = 16;
    constexpr int dotsStartX = kScreenCenter - dotSpacing;
    for (int i = 0; i < CARD_COUNT; ++i) {
        const int dx = dotsStartX + i * dotSpacing;
        if (i == g_currentCard) {
            M5.Display.fillCircle(dx, 22, dotRadius + 1, cardColor);
        } else {
            M5.Display.fillCircle(dx, 22, dotRadius - 1, M5.Display.color565(75, 80, 95));
        }
    }

    // Card Name Pill at y = 44
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextSize(0.50f);
    M5.Display.setTextColor(cardColor, TFT_BLACK);
    M5.Display.drawString(card.name, kScreenCenter, 44);
}

void drawPhysicalActionLinks() {
    // Colored rails enter from the real button positions at the top edge and
    // terminate under the OK/NG circles. Pressing either side lights its rail.
    const bool leftActive = g_leftAgentPressed == kNgAction || g_activeTouch == kNgAction;
    const bool rightActive = g_rightActionPressed || g_activeTouch == kOkAction;
    const auto leftColor = scaledColor(kLeftPhysicalButtonColor, 1.0f);
    const auto rightColor = scaledColor(kRightPhysicalButtonColor, 1.0f);
    const auto leftGlow = scaledColor(kLeftPhysicalButtonColor, leftActive ? 0.58f : 0.24f);
    const auto rightGlow = scaledColor(kRightPhysicalButtonColor, rightActive ? 0.58f : 0.24f);

    // Two short segments make each link follow the curved case rather than
    // looking like a generic straight divider.
    M5.Display.drawWideLine(67, 0, 73, 28, leftActive ? 14.0f : 11.0f, leftGlow);
    M5.Display.drawWideLine(73, 28, 85, 58, leftActive ? 14.0f : 11.0f, leftGlow);
    M5.Display.drawWideLine(67, 0, 73, 28, 5.0f, leftActive ? TFT_WHITE : leftColor);
    M5.Display.drawWideLine(73, 28, 85, 58, 5.0f, leftActive ? TFT_WHITE : leftColor);

    M5.Display.drawWideLine(399, 0, 393, 28, rightActive ? 14.0f : 11.0f, rightGlow);
    M5.Display.drawWideLine(393, 28, 381, 58, rightActive ? 14.0f : 11.0f, rightGlow);
    M5.Display.drawWideLine(399, 0, 393, 28, 5.0f, rightActive ? TFT_WHITE : rightColor);
    M5.Display.drawWideLine(393, 28, 381, 58, 5.0f, rightActive ? TFT_WHITE : rightColor);
}

std::size_t utf8SafeSplitIndex(const char* str, std::size_t maxBytes) {
    if (str == nullptr) return 0;
    std::size_t len = std::strlen(str);
    if (len <= maxBytes) return len;
    std::size_t idx = maxBytes;
    while (idx > 0 && (static_cast<std::uint8_t>(str[idx]) & 0xC0) == 0x80) {
        --idx;
    }
    return idx == 0 ? maxBytes : idx;
}

void drawApprovalOverlay() {
    if (!g_approval.active) return;

    const auto cardBg = M5.Display.color565(22, 25, 34);
    const auto amberWarning = M5.Display.color565(255, 172, 54);
    const auto greenOk = M5.Display.color565(43, 201, 110);
    const auto redNg = M5.Display.color565(245, 90, 104);
    const auto muted = M5.Display.color565(180, 188, 205);

    constexpr int cardX = 63;
    constexpr int cardY = 88;
    constexpr int cardW = 340;
    constexpr int cardH = 290;
    constexpr int cardR = 24;

    M5.Display.fillRoundRect(cardX, cardY, cardW, cardH, cardR, cardBg);
    drawThickRoundRect(cardX, cardY, cardW, cardH, cardR, 3, amberWarning);

    M5.Display.setTextDatum(middle_center);
    if (g_language == LANG_ZH) {
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.15f);
        M5.Display.setTextColor(amberWarning, cardBg);
        M5.Display.drawString("! 需要人工确认", kScreenCenter, cardY + 32);
    } else {
        M5.Display.setFont(&fonts::Orbitron_Light_24);
        M5.Display.setTextSize(0.66f);
        M5.Display.setTextColor(amberWarning, cardBg);
        M5.Display.drawString("APPROVAL REQUIRED", kScreenCenter, cardY + 32);
    }

    char typeLabel[32];
    std::snprintf(typeLabel, sizeof(typeLabel), "[ %s ]", g_approval.type);
    M5.Display.setFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextSize(0.55f);
    M5.Display.setTextColor(TFT_WHITE, cardBg);
    M5.Display.drawString(typeLabel, kScreenCenter, cardY + 70);

    if (g_language == LANG_ZH) {
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.0f);
    } else {
        M5.Display.setFont(&fonts::DejaVu18);
        M5.Display.setTextSize(0.82f);
    }
    M5.Display.setTextColor(muted, cardBg);

    const std::size_t summaryLen = std::strlen(g_approval.summary);
    if (summaryLen > 22) {
        char line1[48] = {};
        char line2[48] = {};
        const std::size_t split1 = utf8SafeSplitIndex(g_approval.summary, 22);
        std::memcpy(line1, g_approval.summary, split1);
        line1[split1] = '\0';
        const std::size_t split2 = utf8SafeSplitIndex(g_approval.summary + split1, 26);
        std::memcpy(line2, g_approval.summary + split1, split2);
        line2[split2] = '\0';
        M5.Display.drawString(line1, kScreenCenter, cardY + 118);
        M5.Display.drawString(line2, kScreenCenter, cardY + 148);
    } else {
        M5.Display.drawString(g_approval.summary[0] != '\0' ? g_approval.summary : "Execute command?", kScreenCenter, cardY + 130);
    }

    constexpr int btnY = cardY + cardH - 64;
    constexpr int btnW = 130;
    constexpr int btnH = 48;
    constexpr int btnR = 14;

    // NG Button (Left)
    const int ngX = cardX + 22;
    M5.Display.fillRoundRect(ngX, btnY, btnW, btnH, btnR, redNg);
    drawThickRoundRect(ngX, btnY, btnW, btnH, btnR, 2, TFT_WHITE);
    M5.Display.setTextColor(TFT_WHITE, redNg);
    if (g_language == LANG_ZH) {
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.05f);
        M5.Display.drawString("拒绝 [左键]", ngX + btnW / 2, btnY + btnH / 2);
    } else {
        M5.Display.setFont(&fonts::Orbitron_Light_24);
        M5.Display.setTextSize(0.60f);
        M5.Display.drawString("NG [LEFT]", ngX + btnW / 2, btnY + btnH / 2);
    }

    // OK Button (Right)
    const int okX = cardX + cardW - btnW - 22;
    M5.Display.fillRoundRect(okX, btnY, btnW, btnH, btnR, greenOk);
    drawThickRoundRect(okX, btnY, btnW, btnH, btnR, 2, TFT_WHITE);
    M5.Display.setTextColor(TFT_WHITE, greenOk);
    if (g_language == LANG_ZH) {
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.05f);
        M5.Display.drawString("确认 [右键]", okX + btnW / 2, btnY + btnH / 2);
    } else {
        M5.Display.setFont(&fonts::Orbitron_Light_24);
        M5.Display.setTextSize(0.60f);
        M5.Display.drawString("OK [RIGHT]", okX + btnW / 2, btnY + btnH / 2);
    }
}

void drawFastGlyph(int x, int y, std::uint16_t color) {
    M5.Display.drawWideLine(x + 10, y - 28, x - 13, y + 1, 7.0f, color);
    M5.Display.drawWideLine(x - 13, y + 1, x + 3, y + 1, 7.0f, color);
    M5.Display.drawWideLine(x + 3, y + 1, x - 8, y + 29, 7.0f, color);
    M5.Display.drawWideLine(x - 8, y + 29, x + 18, y - 7, 7.0f, color);
}

void drawApproveGlyph(int x, int y, std::uint16_t color) {
    M5.Display.drawWideLine(x - 19, y, x - 6, y + 15, 6.0f, color);
    M5.Display.drawWideLine(x - 6, y + 15, x + 21, y - 18, 6.0f, color);
}

void drawRejectGlyph(int x, int y, std::uint16_t color) {
    M5.Display.drawWideLine(x - 18, y - 18, x + 18, y + 18, 6.0f, color);
    M5.Display.drawWideLine(x + 18, y - 18, x - 18, y + 18, 6.0f, color);
}

void drawPlanGlyph(int x, int y, std::uint16_t color, bool enabled) {
    drawThickRoundRect(x - 29, y - 15, 58, 30, 15, 3, color);
    M5.Display.fillCircle(x + (enabled ? 14 : -14), y, 9, color);
}

void drawSwipeChevron(int x, int y, bool pointsRight, std::uint16_t color) {
    const int direction = pointsRight ? 1 : -1;
    M5.Display.drawWideLine(x - direction * 4, y - 7, x + direction * 3, y, 2.0f, color);
    M5.Display.drawWideLine(x + direction * 3, y, x - direction * 4, y + 7, 2.0f, color);
}

void drawCenterActionGlyph(int action, int x, int y, std::uint16_t color) {
    switch (action) {
        case 0:
            drawMicGlyph(x, y - 4, color);
            break;
        case 1:
            drawFastGlyph(x, y - 5, color);
            break;
        case 2:
            drawApproveGlyph(x, y - 5, color);
            break;
        case 3:
            drawRejectGlyph(x, y - 5, color);
            break;
        default:
            drawPlanGlyph(x, y - 5, color, g_planModeEnabled);
            break;
    }
}

void drawStatusBar() {
    const auto panel = M5.Display.color565(21, 24, 31);
    const auto stateColor = g_connected ? M5.Display.color565(66, 232, 139)
                                        : M5.Display.color565(255, 174, 54);
    M5.Display.fillRoundRect(151, 426, 164, 26, 13, panel);
    drawThickRoundRect(151, 426, 164, 26, 13, 2, stateColor);

    char status[40];
    if (g_restartAt != 0) {
        std::snprintf(status, sizeof(status), g_language == LANG_ZH ? "重启  #%d" : "RESTART  #%d", g_deviceSlot);
    } else {
        const char* stateText = g_connected ? (g_language == LANG_ZH ? "在线" : "ON")
                                            : (g_language == LANG_ZH ? "配对" : "PAIR");
        std::snprintf(status, sizeof(status), "%s  #%d  %u%%%s", stateText,
                      g_deviceSlot, g_batteryLevel, g_isCharging ? "+" : "");
    }
    if (g_language == LANG_ZH) {
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.0f);
    } else {
        M5.Display.setFont(&fonts::Orbitron_Light_24);
        M5.Display.setTextSize(0.75f);
    }
    M5.Display.setTextColor(TFT_WHITE, panel);
    M5.Display.drawString(status, kScreenCenter, 439);
}

void renderSettingsUi() {
    const auto panel = M5.Display.color565(27, 30, 38);
    const auto panelBorder = M5.Display.color565(118, 124, 142);
    const auto purple = M5.Display.color565(145, 120, 255);
    const auto muted = M5.Display.color565(205, 210, 222);
    const auto& quota = g_cards[g_currentCard].quota;
    const auto quotaState = vibe::quotaPresentation(
        quota.freshness(millis(), kQuotaStaleAfterMs));
    const bool hasSettingsQuota = quotaState != vibe::QuotaPresentation::Unavailable;
    char quotaLine[64]{};
    if (hasSettingsQuota) {
        if (quotaState == vibe::QuotaPresentation::Stale) {
            std::snprintf(quotaLine, sizeof(quotaLine),
                          g_language == LANG_ZH ? "%s: %.0f%% (数据过期)"
                                                : "%s: %.0f%% (SYNC STALE)",
                          g_cards[g_currentCard].name, quota.remainingPercent);
        } else {
            char resetStr[24];
            const std::uint32_t elapsed = (millis() - quota.receivedAtMs) / 1000;
            const std::uint32_t remSec = elapsed >= quota.resetInSeconds
                ? 0 : quota.resetInSeconds - elapsed;
            formatResetCountdown(remSec, resetStr, sizeof(resetStr));
            std::snprintf(quotaLine, sizeof(quotaLine), "%s: %.0f%% (%s)",
                          g_cards[g_currentCard].name, quota.remainingPercent, resetStr);
        }
    }

    M5.Display.setTextDatum(middle_center);
    if (g_language == LANG_ZH) {
        M5.Display.setFont(&fonts::efontCN_24);
        M5.Display.setTextSize(1.0f);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.drawString("系统设置", 218, 38);

        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.0f);
        M5.Display.setTextColor(muted, TFT_BLACK);
        if (hasSettingsQuota) {
            M5.Display.drawString(quotaLine, kScreenCenter, 72);
        } else {
            M5.Display.drawString("蓝牙设备槽位", kScreenCenter, 72);
        }
    } else {
        M5.Display.setFont(&fonts::Orbitron_Light_32);
        M5.Display.setTextSize(0.82f);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.drawString("SETTINGS", 218, 38);

        M5.Display.setFont(&fonts::DejaVu18);
        M5.Display.setTextSize(0.78f);
        M5.Display.setTextColor(muted, TFT_BLACK);
        if (hasSettingsQuota) {
            M5.Display.drawString(quotaLine, kScreenCenter, 72);
        } else {
            M5.Display.drawString("BLUETOOTH DEVICE", kScreenCenter, 72);
        }
    }

    M5.Display.fillCircle(kSettingsCloseX, kSettingsCloseY, kSettingsCloseRadius, panel);
    drawThickCircle(kSettingsCloseX, kSettingsCloseY, kSettingsCloseRadius, 3, panelBorder);
    M5.Display.drawWideLine(kSettingsCloseX - 10, kSettingsCloseY - 10, kSettingsCloseX + 10,
                            kSettingsCloseY + 10, 2.5f, TFT_WHITE);
    M5.Display.drawWideLine(kSettingsCloseX + 10, kSettingsCloseY - 10, kSettingsCloseX - 10,
                            kSettingsCloseY + 10, 2.5f, TFT_WHITE);

    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextSize(1);
    for (int i = 0; i < 3; ++i) {
        const int slot = i + 1;
        const int x = 122 + i * 111;
        const bool selected = slot == g_pendingDeviceSlot;
        M5.Display.fillCircle(x, 112, 30, selected ? purple : panel);
        drawThickCircle(x, 112, 30, selected ? 4 : 2, selected ? TFT_WHITE : panelBorder);
        M5.Display.setTextColor(TFT_WHITE, selected ? purple : panel);
        char label[4];
        std::snprintf(label, sizeof(label), "#%d", slot);
        M5.Display.drawString(label, x, 112);
    }

    const bool pairPressed = g_activeTouch == kTouchPair;
    const auto pairFill = pairPressed ? M5.Display.color565(103, 81, 220) : purple;
    M5.Display.fillRoundRect(153, 151, 160, 45, 22, pairFill);
    drawThickRoundRect(153, 151, 160, 45, 22, pairPressed ? 5 : 3,
                       pairPressed ? TFT_WHITE : panelBorder);
    M5.Display.setTextColor(TFT_WHITE, pairFill);
    if (g_language == LANG_ZH) {
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.1f);
        M5.Display.drawString("开始配对", kScreenCenter, 173);
    } else {
        M5.Display.setFont(&fonts::DejaVu18);
        M5.Display.setTextSize(1.0f);
        M5.Display.drawString("PAIR", kScreenCenter, 173);
    }

    char volumeLabel[32];
    if (g_language == LANG_ZH) {
        std::snprintf(volumeLabel, sizeof(volumeLabel), "按键音量  %u%%",
                      static_cast<unsigned>(g_seVolume) * 100 / 255);
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.0f);
    } else {
        std::snprintf(volumeLabel, sizeof(volumeLabel), "SE VOLUME  %u%%",
                      static_cast<unsigned>(g_seVolume) * 100 / 255);
        M5.Display.setFont(&fonts::DejaVu18);
        M5.Display.setTextSize(0.82f);
    }
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(volumeLabel, kScreenCenter, 216);

    const int volumeX = kSettingsSliderLeft + static_cast<int>(g_seVolume) *
                        (kSettingsSliderRight - kSettingsSliderLeft) / 255;
    M5.Display.drawWideLine(kSettingsSliderLeft, 242, kSettingsSliderRight, 242,
                            6.0f, panelBorder);
    if (volumeX > kSettingsSliderLeft) {
        M5.Display.drawWideLine(kSettingsSliderLeft, 242, volumeX, 242, 6.0f, purple);
    }
    M5.Display.fillCircle(volumeX, 242, 12, TFT_WHITE);
    drawThickCircle(volumeX, 242, 12, 2, purple);

    char vibrationLabel[32];
    if (g_language == LANG_ZH) {
        std::snprintf(vibrationLabel, sizeof(vibrationLabel), "震动强度  %u%%",
                      static_cast<unsigned>(g_vibrationStrength) * 100 / 255);
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.0f);
    } else {
        std::snprintf(vibrationLabel, sizeof(vibrationLabel), "VIBE STRENGTH  %u%%",
                      static_cast<unsigned>(g_vibrationStrength) * 100 / 255);
        M5.Display.setFont(&fonts::DejaVu18);
        M5.Display.setTextSize(0.82f);
    }
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(vibrationLabel, kScreenCenter, 278);

    const int vibrationX = kSettingsSliderLeft + static_cast<int>(g_vibrationStrength) *
                           (kSettingsSliderRight - kSettingsSliderLeft) / 255;
    M5.Display.drawWideLine(kSettingsSliderLeft, 304, kSettingsSliderRight, 304,
                            6.0f, panelBorder);
    if (vibrationX > kSettingsSliderLeft) {
        M5.Display.drawWideLine(kSettingsSliderLeft, 304, vibrationX, 304, 6.0f, purple);
    }
    M5.Display.fillCircle(vibrationX, 304, 12, TFT_WHITE);
    drawThickCircle(vibrationX, 304, 12, 2, purple);

    if (g_language == LANG_ZH) {
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(0.95f);
        M5.Display.setTextColor(muted, TFT_BLACK);
        M5.Display.drawString("智能体状态变化提醒", kScreenCenter, 341);
    } else {
        M5.Display.setFont(&fonts::DejaVu18);
        M5.Display.setTextSize(0.72f);
        M5.Display.setTextColor(muted, TFT_BLACK);
        M5.Display.drawString("AGENT 1-6 STATE CHANGE", kScreenCenter, 341);
    }

    const auto drawCheckbox = [&](int x, const char* label, bool checked) {
        const auto fill = checked ? purple : panel;
        M5.Display.fillRoundRect(x, 367, 28, 28, 6, fill);
        drawThickRoundRect(x, 367, 28, 28, 6, 2, checked ? TFT_WHITE : panelBorder);
        if (checked) {
            M5.Display.drawWideLine(x + 6, 381, x + 12, 387, 3.0f, TFT_WHITE);
            M5.Display.drawWideLine(x + 12, 387, x + 23, 375, 3.0f, TFT_WHITE);
        }
        M5.Display.setTextDatum(middle_left);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        if (g_language == LANG_ZH) {
            M5.Display.setFont(&fonts::efontCN_16);
            M5.Display.setTextSize(1.0f);
        } else {
            M5.Display.setFont(&fonts::DejaVu18);
            M5.Display.setTextSize(0.78f);
        }
        M5.Display.drawString(label, x + 36, 381);
        M5.Display.setTextDatum(middle_center);
    };
    drawCheckbox(105, g_language == LANG_ZH ? "震动" : "VIBE", g_agentStateVibeEnabled);

    // Language toggle switch on the right side
    constexpr int kLangSwitchX = 236;
    constexpr int kLangSwitchY = 367;
    constexpr int kLangSwitchW = 128;
    constexpr int kLangSwitchH = 28;
    M5.Display.fillRoundRect(kLangSwitchX, kLangSwitchY, kLangSwitchW, kLangSwitchH, 6, panel);
    drawThickRoundRect(kLangSwitchX, kLangSwitchY, kLangSwitchW, kLangSwitchH, 6, 2, panelBorder);

    const int pillX = (g_language == LANG_ZH) ? kLangSwitchX + 2 : kLangSwitchX + kLangSwitchW / 2;
    M5.Display.fillRoundRect(pillX, kLangSwitchY + 2, kLangSwitchW / 2 - 2, kLangSwitchH - 4, 4, purple);

    M5.Display.setFont(&fonts::efontCN_16);
    M5.Display.setTextSize(0.9f);
    M5.Display.setTextColor(g_language == LANG_ZH ? TFT_WHITE : muted,
                            (g_language == LANG_ZH) ? purple : panel);
    M5.Display.drawString("中文", kLangSwitchX + kLangSwitchW / 4, kLangSwitchY + kLangSwitchH / 2);

    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextSize(0.75f);
    M5.Display.setTextColor(g_language == LANG_EN ? TFT_WHITE : muted,
                            (g_language == LANG_EN) ? purple : panel);
    M5.Display.drawString("EN", kLangSwitchX + 3 * kLangSwitchW / 4, kLangSwitchY + kLangSwitchH / 2);

    drawStatusBar();
}

void renderUi(std::uint32_t now) {
    // Redraw the small round display as one frame. The UI is simple enough that
    // full-frame painting avoids stale pixels when switching between layers.
    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_BLACK);

    if (g_settingsOpen) {
        renderSettingsUi();
        M5.Display.endWrite();
        g_uiDirty = false;
        g_lastUiDraw = now;
        return;
    }

    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::Orbitron_Light_32);
    M5.Display.setTextSize(1);
    const int outerCount = g_actionLayer ? kActionCount : kAgentCount;
    if (g_actionLayer) {
        drawPhysicalActionLinks();
    }
    // Draw current card header & pagination
    drawCardHeader();

    const auto& currentCard = g_cards[g_currentCard];
    const auto currentPrimaryColor = scaledColor(currentCard.primaryColor, 1.0f);

    for (int i = 0; i < outerCount; ++i) {
        const int outerX = g_actionLayer ? actionX[i] : agentX[i];
        const int outerY = g_actionLayer ? actionY[i] : agentY[i];
        std::uint16_t fill = M5.Display.color565(17, 22, 28);
        std::uint16_t accent = M5.Display.color565(105, 114, 132);
        bool selected = false;
        if (g_actionLayer) {
            static constexpr std::uint32_t kActionColors[kActionCount] = {
                0x9D74FF, kRightPhysicalButtonColor, kLeftPhysicalButtonColor,
                0x33C4E8, 0xE5E8EF,
            };
            accent = scaledColor(kActionColors[i], 1.0f);
            selected = i == 3 ? g_planModeEnabled : g_selectedAction == i;
        } else {
            const auto& state = currentCard.agents[i];
            const float brightness = effectBrightness(state.effect, state.brightness, state.speed, now);
            fill = state.effect == 0 || state.color == 0
                       ? M5.Display.color565(17, 22, 28)
                       : scaledColor(state.color, brightness);
            selected = false;
            accent = currentPrimaryColor;
        }
        const bool pressed = g_activeTouch == i || g_leftAgentPressed == i ||
                             (g_actionLayer && g_rightActionPressed && i == kOkAction);
        if (g_actionLayer && pressed) {
            fill = accent;
        }
        M5.Display.fillCircle(outerX, outerY, kAgentButtonRadius, fill);
        if (selected) {
            drawThickCircle(outerX, outerY, kAgentButtonRadius + 4, 3,
                            g_actionLayer ? scaledColor(0x34303F, 1.0f)
                                          : M5.Display.color565(74, 56, 128));
        }
        const auto borderColor = pressed ? TFT_WHITE
                                         : selected ? accent
                                                    : g_actionLayer ? accent
                                                                    : M5.Display.color565(105, 114, 132);
        drawThickCircle(outerX, outerY, kAgentButtonRadius, pressed ? 7 : selected ? 7 : 3,
                        borderColor);
        if (pressed) {
            drawThickCircle(outerX, outerY, kAgentButtonRadius - 8, 2, TFT_WHITE);
        } else if (selected) {
            drawThickCircle(outerX, outerY, kAgentButtonRadius - 9, 2, TFT_WHITE);
        }
        if (g_actionLayer) {
            const int glyphY = outerY - 10;
            if (i == 0) {
                drawFastGlyph(outerX, glyphY, TFT_WHITE);
            } else if (i == 1) {
                drawApproveGlyph(outerX, glyphY, TFT_WHITE);
            } else if (i == 2) {
                drawRejectGlyph(outerX, glyphY, TFT_WHITE);
            } else if (i == 3) {
                drawPlanGlyph(outerX, glyphY, TFT_WHITE, g_planModeEnabled);
            } else {
                drawAssistantGlyph(outerX, glyphY, TFT_WHITE);
            }
            static constexpr const char* kOuterActionLabelsEn[kActionCount] = {
                "FAST", "OK", "NG", "PLAN", "AI",
            };
            static constexpr const char* kOuterActionLabelsZh[kActionCount] = {
                "快速", "确认", "拒绝", "计划", "AI",
            };
            if (g_language == LANG_ZH) {
                M5.Display.setFont(&fonts::efontCN_16);
                M5.Display.setTextSize(1.0f);
                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.drawString(kOuterActionLabelsZh[i], outerX,
                                      outerY + kAgentButtonRadius + 13);
            } else {
                M5.Display.setFont(&fonts::Orbitron_Light_24);
                M5.Display.setTextSize(0.62f);
                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.drawString(kOuterActionLabelsEn[i], outerX,
                                      outerY + kAgentButtonRadius + 13);
            }
            M5.Display.setFont(&fonts::Orbitron_Light_32);
            M5.Display.setTextSize(1);
        } else {
            const auto labelStyle = vibe::normalAgentLabelRenderStyle();
            const int fillRed = ((fill >> 11) & 0x1F) * 255 / 31;
            const int fillGreen = ((fill >> 5) & 0x3F) * 255 / 63;
            const int fillBlue = (fill & 0x1F) * 255 / 31;
            const int fillLuminance = (fillRed * 299 + fillGreen * 587 + fillBlue * 114) / 1000;
            const auto labelColor = fillLuminance >= 150 ? TFT_BLACK : TFT_WHITE;
            M5.Display.setFont(&fonts::Orbitron_Light_32);
            M5.Display.setTextSize(labelStyle.textScale);
            M5.Display.setTextDatum(middle_center);
            M5.Display.setTextColor(labelColor);
            char label[2];
            std::snprintf(label, sizeof(label), "%d", i + 1);
            M5.Display.drawString(label, outerX - 1, outerY);
            M5.Display.drawString(label, outerX + 1, outerY);
            M5.Display.drawString(label, outerX, outerY);
        }
    }

    if (!g_actionLayer) {
        const auto actionStyle = vibe::actionLabelRenderStyle();
        M5.Display.setFont(&fonts::Orbitron_Light_32);
        M5.Display.setTextSize(actionStyle.textScale);
        M5.Display.setTextDatum(middle_center);
    }

    if (!g_actionLayer) {
        drawSelectionIndicator(now);
    }

    // Draw Quota orbital gauge ring for active card
    const auto& quota = currentCard.quota;
    const auto quotaFreshness = quota.freshness(now, kQuotaStaleAfterMs);
    const bool quotaAvailable = quotaFreshness != vibe::QuotaFreshness::Unavailable;
    const bool quotaStale = quotaFreshness == vibe::QuotaFreshness::Stale;
    if (quotaAvailable && !g_actionLayer) {
        constexpr int kQuotaOuterR = 82;
        constexpr int kQuotaInnerR = 76;
        const auto trackColor = M5.Display.color565(38, 50, 61);
        M5.Display.fillArc(kScreenCenter, kScreenCenter, kQuotaOuterR, kQuotaInnerR, 0, 360, trackColor);

        const float remaining = std::max(0.0f, std::min(100.0f, quota.remainingPercent));
        const auto quotaColor = quotaStale
            ? M5.Display.color565(116, 85, 39)
            : (remaining > 20.0f ? currentPrimaryColor : M5.Display.color565(255, 180, 74));

        M5.Display.fillArc(kScreenCenter, kScreenCenter, kQuotaOuterR, kQuotaInnerR, 0,
                           static_cast<int>(remaining * 3.6f), quotaColor);

        // 4 Calibration tick marks
        M5.Display.fillRoundRect(kScreenCenter - 1, kScreenCenter - kQuotaOuterR, 3, 7, 1, TFT_BLACK);
        M5.Display.fillRoundRect(kScreenCenter + kQuotaInnerR - 1, kScreenCenter - 1, 7, 3, 1, TFT_BLACK);
        M5.Display.fillRoundRect(kScreenCenter - 1, kScreenCenter + kQuotaInnerR - 1, 3, 7, 1, TFT_BLACK);
        M5.Display.fillRoundRect(kScreenCenter - kQuotaOuterR, kScreenCenter - 1, 7, 3, 1, TFT_BLACK);
    }

    const bool micPressed = g_rightLongTriggered || g_activeTouch == kTouchMic;
    const auto micAccent = currentPrimaryColor;
    const auto micFill = micPressed ? micAccent : M5.Display.color565(25, 31, 40);
    M5.Display.fillCircle(kScreenCenter, kScreenCenter, kMicButtonRadius, micFill);
    drawThickCircle(kScreenCenter, kScreenCenter, kMicButtonRadius, micPressed ? 7 : 4,
                    micPressed ? TFT_WHITE : micAccent);

    if (!micPressed && !g_actionLayer) {
        const auto textColor = quotaStale ? M5.Display.color565(180, 150, 100) : currentPrimaryColor;
        const auto mutedColor = M5.Display.color565(130, 142, 160);

        if (!quotaAvailable) {
            M5.Display.setTextColor(mutedColor, micFill);
            if (g_language == LANG_ZH) {
                M5.Display.setFont(&fonts::efontCN_16);
                M5.Display.setTextSize(1.0f);
                M5.Display.drawString("等待同步", kScreenCenter, kScreenCenter - 1);
            } else {
                M5.Display.setFont(&fonts::Orbitron_Light_24);
                M5.Display.setTextSize(0.58f);
                M5.Display.drawString("SYNC WAIT", kScreenCenter, kScreenCenter - 1);
            }
        } else if (quota.hasCredits) {
            // Workbuddy Credit Mode:
            // Top line: Pure technical label "CREDITS"
            M5.Display.setFont(&fonts::Orbitron_Light_24);
            M5.Display.setTextSize(0.48f);
            M5.Display.setTextColor(mutedColor, micFill);
            M5.Display.drawString(quotaStale ? "SYNC STALE" : "CREDITS", kScreenCenter, kScreenCenter - 34);

            // Center line: Large Credit count e.g. "1250" or "850"
            char creditText[20];
            const float crVal = quota.credits;
            if (crVal >= 10000.0f) {
                std::snprintf(creditText, sizeof(creditText), "%.1fK", crVal / 1000.0f);
            } else {
                std::snprintf(creditText, sizeof(creditText), "%.0f", crVal);
            }
            M5.Display.setFont(&fonts::Orbitron_Light_32);
            M5.Display.setTextSize(0.85f);
            M5.Display.setTextColor(textColor, micFill);
            M5.Display.drawString(creditText, kScreenCenter, kScreenCenter - 1);

            // Bottom line: Subtitle e.g. "1250 CR" or "TOTAL 1500" or "85% LEFT"
            char creditSub[24];
            if (quota.totalCredits > 0.0f) {
                std::snprintf(creditSub, sizeof(creditSub), "TOTAL %.0f", quota.totalCredits);
            } else {
                std::snprintf(creditSub, sizeof(creditSub), "%.0f CR LEFT", crVal);
            }
            M5.Display.setFont(&fonts::Orbitron_Light_24);
            M5.Display.setTextSize(0.46f);
            M5.Display.setTextColor(quotaStale ? mutedColor : TFT_WHITE, micFill);
            M5.Display.drawString(creditSub, kScreenCenter, kScreenCenter + 32);
        } else {
            // Top line: Pure technical label e.g. "WEEKLY"
            M5.Display.setFont(&fonts::Orbitron_Light_24);
            M5.Display.setTextSize(0.48f);
            M5.Display.setTextColor(mutedColor, micFill);
            M5.Display.drawString(quotaStale ? "SYNC STALE" : (g_currentCard == CARD_ANTIGRAVITY ? "MONTHLY" : "WEEKLY"),
                                  kScreenCenter, kScreenCenter - 34);

            // Center line: Crisp percentage
            char quotaText[16];
            std::snprintf(quotaText, sizeof(quotaText), "%.0f%%", quota.remainingPercent);
            M5.Display.setFont(&fonts::Orbitron_Light_32);
            M5.Display.setTextSize(0.85f);
            M5.Display.setTextColor(textColor, micFill);
            M5.Display.drawString(quotaText, kScreenCenter, kScreenCenter - 1);

            // Bottom line: Minimal concise countdown e.g. "RESET 1H 00M"
            char resetStr[24];
            const std::uint32_t elapsed = (now - quota.receivedAtMs) / 1000;
            const std::uint32_t remSec = elapsed >= quota.resetInSeconds ? 0 : quota.resetInSeconds - elapsed;
            formatResetCountdown(remSec, resetStr, sizeof(resetStr));

            M5.Display.setFont(&fonts::Orbitron_Light_24);
            M5.Display.setTextSize(0.46f);
            M5.Display.setTextColor(quotaStale ? mutedColor : TFT_WHITE, micFill);
            M5.Display.drawString(resetStr, kScreenCenter, kScreenCenter + 32);
        }
    } else {
        drawLargeMicGlyph(kScreenCenter, kScreenCenter - 2, TFT_WHITE);
    }

    if (!g_actionLayer) {
        const auto settingsFill = M5.Display.color565(30, 32, 40);
        M5.Display.fillCircle(kSettingsX, kSettingsY, kSettingsVisualRadius, settingsFill);
        drawThickCircle(kSettingsX, kSettingsY, kSettingsVisualRadius, 3,
                        M5.Display.color565(120, 126, 143));
        drawSettingsGlyph(kSettingsX, kSettingsY, TFT_WHITE);
    }

    if (!g_actionLayer) {
        drawStatusBar();
    }

    if (g_approval.active) {
        drawApprovalOverlay();
    }

    M5.Display.endWrite();
    g_uiDirty = false;
    g_lastUiDraw = now;
}

// Draw one frame of the boot animation. The six orbiting dots preview the
// Agent-layer layout before the full interface appears.
void drawSplashFrame(float progress) {
    const float eased = 1.0f - std::pow(1.0f - clamp01(progress), 3.0f);
    const float rawFade = clamp01(progress / 0.68f);
    const float textFade = rawFade * rawFade * (3.0f - 2.0f * rawFade);
    const float pulse = 0.78f + 0.22f * std::sin(progress * PI * 4.0f);
    const auto purple = scaledColor(0x9D74FF, textFade);
    const auto cyan = scaledColor(0x33C4E8, textFade);
    const auto muted = scaledColor(0xAAB4C8, textFade * 0.85f);

    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_BLACK);

    // Expand the primary logo ring from the center.
    const int ringRadius = 72 + static_cast<int>(56.0f * eased);
    drawThickCircle(kScreenCenter, kScreenCenter, ringRadius + 8, 2,
                    scaledColor(0x34284F, textFade));
    drawThickCircle(kScreenCenter, kScreenCenter, ringRadius, 4,
                    scaledColor(0x9D74FF, textFade * pulse));
    // Reveal six status dots clockwise, mirroring the main Agent layer.
    for (int i = 0; i < kAgentCount; ++i) {
        const float revealAt = 0.10f + i * 0.075f;
        if (progress < revealAt) {
            continue;
        }
        const float dotFade = clamp01((progress - revealAt) / 0.18f);
        const float angle = (-120.0f + i * 60.0f) * PI / 180.0f;
        const int x = kScreenCenter + static_cast<int>(std::cos(angle) * 166.0f);
        const int y = kScreenCenter + static_cast<int>(std::sin(angle) * 166.0f);
        M5.Display.fillCircle(x, y, 5 + static_cast<int>(3.0f * dotFade),
                              scaledColor(i % 2 == 0 ? 0x9D74FF : 0x33C4E8, dotFade));
    }

    // The Orbitron face matches the technical visual language of the main UI.
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::Orbitron_Light_32);
    M5.Display.setTextSize(1.24f);
    M5.Display.setTextColor(purple, TFT_BLACK);
    M5.Display.drawString("VIBEWATCH", kScreenCenter, 202);

    if (g_language == LANG_ZH) {
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.1f);
        M5.Display.setTextColor(cyan, TFT_BLACK);
        M5.Display.drawString("AI 交互控制器", kScreenCenter, 252);
    } else {
        M5.Display.setFont(&fonts::Orbitron_Light_24);
        M5.Display.setTextSize(0.62f);
        M5.Display.setTextColor(cyan, TFT_BLACK);
        M5.Display.drawString("AI CONTROL SURFACE", kScreenCenter, 252);
    }

    M5.Display.setFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextSize(0.68f);
    M5.Display.setTextColor(muted, TFT_BLACK);
    M5.Display.drawString(vibe::kFirmwareVersion, kScreenCenter, 291);

    // Animate from zero to the measured charge so the battery readout also
    // acts as a compact startup progress indicator.
    const int animatedBattery = static_cast<int>(
        std::lround(static_cast<float>(g_batteryLevel) * eased));
    const std::uint32_t batteryPacked = g_batteryLevel <= 15
        ? 0xF55367
        : g_batteryLevel <= 35 ? 0xFFAC28 : 0x33C4E8;
    const auto batteryColor = scaledColor(batteryPacked, textFade);
    const auto batteryTrack = scaledColor(0x566071, textFade * 0.55f);
    constexpr int kBatteryBarX = 143;
    constexpr int kBatteryBarY = 361;
    constexpr int kBatteryBarWidth = 180;
    constexpr int kBatteryBarHeight = 10;
    const int batteryFillWidth = kBatteryBarWidth * animatedBattery / 100;

    char batteryLabel[24];
    if (g_language == LANG_ZH) {
        std::snprintf(batteryLabel, sizeof(batteryLabel), "电量  %d%%", animatedBattery);
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1.0f);
    } else {
        std::snprintf(batteryLabel, sizeof(batteryLabel), "BATTERY  %d%%", animatedBattery);
        M5.Display.setFont(&fonts::Orbitron_Light_24);
        M5.Display.setTextSize(0.58f);
    }
    M5.Display.setTextColor(batteryColor, TFT_BLACK);
    M5.Display.drawString(batteryLabel, kScreenCenter, 338);
    M5.Display.fillRoundRect(kBatteryBarX, kBatteryBarY, kBatteryBarWidth,
                             kBatteryBarHeight, kBatteryBarHeight / 2, batteryTrack);
    if (batteryFillWidth > 0) {
        M5.Display.fillRoundRect(kBatteryBarX, kBatteryBarY, batteryFillWidth,
                                 kBatteryBarHeight, kBatteryBarHeight / 2, batteryColor);
    }

    // A small center mark gives the logo a watch-dial focal point.
    M5.Display.fillCircle(kScreenCenter, 151, 4, cyan);
    M5.Display.drawWideLine(kScreenCenter - 13, 151, kScreenCenter - 6, 151, 2.0f, purple);
    M5.Display.drawWideLine(kScreenCenter + 6, 151, kScreenCenter + 13, 151, 2.0f, purple);

    M5.Display.endWrite();
}

void showSplashScreen() {
    constexpr std::uint32_t kSplashAnimationMs = 1800;
    constexpr std::uint32_t kSplashHoldMs = 1100;
    struct ChiptuneNote {
        std::uint32_t atMs;
        float frequency;
        std::uint32_t durationMs;
    };
    // Original NES-style pulse-wave arpeggio; no existing game melody is used.
    static constexpr ChiptuneNote kStartupJingle[] = {
        {70, 293.66f, 70},   // D4
        {160, 440.00f, 70},  // A4
        {250, 587.33f, 80},  // D5
        {360, 698.46f, 85},  // F5
        {480, 880.00f, 95},  // A5
        {610, 698.46f, 65},  // F5
        {700, 880.00f, 75},  // A5
        {800, 1174.66f, 240},  // D6
    };
    constexpr std::size_t kJingleNoteCount =
        sizeof(kStartupJingle) / sizeof(kStartupJingle[0]);

    drawSplashFrame(0.0f);
    const std::uint32_t startedAt = millis();
    std::size_t nextNote = 0;

    while (millis() - startedAt < kSplashAnimationMs) {
        const std::uint32_t elapsed = millis() - startedAt;
        while (nextNote < kJingleNoteCount &&
               elapsed >= kStartupJingle[nextNote].atMs) {
            const auto& note = kStartupJingle[nextNote];
            playSe(note.frequency, note.durationMs);
            ++nextNote;
        }
        const float progress = static_cast<float>(elapsed) /
                               static_cast<float>(kSplashAnimationMs);
        drawSplashFrame(progress);
        delay(24);
    }
    drawSplashFrame(1.0f);
    delay(kSplashHoldMs);
}

}  // namespace

// -----------------------------------------------------------------------------
// Arduino lifecycle
// -----------------------------------------------------------------------------

void setup() {
    // Initialize hardware and local state before advertising the BLE HID. This
    // ensures the first screen and battery report are valid when a host connects.
    Serial.begin(115200);
    delay(200);

    auto config = M5.config();
    config.clear_display = true;
    config.internal_spk = true;
    M5.begin(config);
    M5.Display.setBrightness(80);
    M5.Display.setRotation(0);

    loadPreferences();
    M5.Speaker.setVolume(g_seVolume);
    updateBattery(false);
    showSplashScreen();

    initializeAgentPositions();
    if (!vibe::initializeIngressQueue()) {
        Serial.println("Failed to create ingress queues");
        while (true) {
            delay(1000);
        }
    }

    renderUi(millis());
    initializeBle();
    g_uiDirty = true;
    emitFreeHeapDiagnostic(HeapDiagnosticEvent::Boot);
}

void loop() {
    // Keep input, host messages, deferred restart, haptics, battery updates, and
    // rendering cooperative; no path should block long enough to starve BLE.
    M5.update();
    const std::uint32_t ingressNow = millis();
    processBleLifecycleEvents(ingressNow);

    vibe::IngressMessage ingress;
    for (int processed = 0;
         processed < 4 && vibe::dequeueIngress(ingress); ++processed) {
        processIngressMessage(ingress, ingressNow);
    }
    vibe::HidChunk chunk;
    for (int processed = 0;
         processed < 8 && vibe::dequeueHidChunk(chunk); ++processed) {
        processHidChunkInMainLoop(chunk);
    }

    processIndicationTransport(ingressNow);
    expirePendingApproval(ingressNow);
    handleTouch();
    handlePhysicalButtons();

    const std::uint32_t now = millis();
    if (g_lastActivityAt == 0) {
        g_lastActivityAt = now;
    }

    // Auto-dimming & screen sleep with Dynamic CPU Frequency Scaling (DVFS)
    if (!g_isDimmed && !g_isScreenSleeping && (now - g_lastActivityAt >= kDimTimeoutMs)) {
        g_isDimmed = true;
        M5.Display.setBrightness(15);
        setCpuFrequencyMhz(160);
    }
    if (!g_isScreenSleeping && (now - g_lastActivityAt >= kSleepTimeoutMs)) {
        g_isScreenSleeping = true;
        M5.Display.setBrightness(0);
        setCpuFrequencyMhz(80);
    }

    if (g_restartAt != 0 && static_cast<std::int32_t>(now - g_restartAt) >= 0) {
        ESP.restart();
    }
    if (g_vibrationOffAt != 0 && static_cast<std::int32_t>(now - g_vibrationOffAt) >= 0) {
        M5.Power.setVibration(0);
        g_vibrationOffAt = 0;
    }
    if (now - g_lastBatteryUpdate >= kBatteryUpdatePeriodMs) {
        updateBattery(true);
    }
    const std::uint32_t uiPeriod = g_selectionAnimating ? kSelectionAnimationPeriodMs
                                                        : kUiAnimationPeriodMs;
    const bool shouldRedrawQuota = g_cards[g_currentCard].quota.available && (now - g_lastUiDraw >= 1000);
    if (!g_isScreenSleeping && (g_uiDirty || uiIsAnimated() || shouldRedrawQuota) && now - g_lastUiDraw >= uiPeriod) {
        renderUi(now);
    }

    delay(5);
}

#pragma once

#include <Arduino.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace vibe {

constexpr int kAgentCount = 6;
constexpr int kActionCount = 5;

enum AgentCard : std::uint8_t {
    CARD_CODEX = 0,
    CARD_WORKBUDDY = 1,
    CARD_ANTIGRAVITY = 2,
    CARD_COUNT = 3
};

enum Language : std::uint8_t {
    LANG_ZH = 0,
    LANG_EN = 1
};

struct AgentState {
    std::uint32_t color = 0;
    float brightness = 0.0f;
    int effect = 0;
    float speed = 0.0f;
};

struct QuotaState {
    float remainingPercent = 0.0f;
    std::uint32_t resetInSeconds = 0;
    std::uint32_t receivedAtMs = 0;
    bool available = false;
    float credits = 0.0f;        // Workbuddy specific Credit balance
    float totalCredits = 0.0f;   // Total Credit allocation (optional)
    bool hasCredits = false;     // Flag indicating credit mode

    QuotaState() = default;
    QuotaState(float rem, std::uint32_t resetSec, std::uint32_t recAt = 0, bool avail = true,
               float cr = 0.0f, float totCr = 0.0f, bool hasCr = false)
        : remainingPercent(rem), resetInSeconds(resetSec), receivedAtMs(recAt), available(avail),
          credits(cr), totalCredits(totCr), hasCredits(hasCr) {}
};

struct CardState {
    const char* name;
    std::uint32_t primaryColor;
    QuotaState quota;
    std::array<AgentState, kAgentCount> agents;
    int selectedAgent;

    CardState(const char* n, std::uint32_t c, QuotaState q, int sel = 0)
        : name(n), primaryColor(c), quota(q), selectedAgent(sel) {}
};

struct ApprovalState {
    bool active = false;
    int agentId = 0;
    char type[24] = "EXEC";
    char summary[96] = "";
    std::uint32_t triggeredAtMs = 0;
};

}  // namespace vibe

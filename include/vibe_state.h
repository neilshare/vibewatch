#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "vibe_quota.h"

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

struct CardState {
    const char* name;
    std::uint32_t primaryColor;
    QuotaSnapshot quota;
    std::array<AgentState, kAgentCount> agents;
    int selectedAgent;

    CardState(const char* n, std::uint32_t c, QuotaSnapshot q, int sel = 0)
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

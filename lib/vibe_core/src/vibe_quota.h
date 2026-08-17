#pragma once

#include <cstdint>

namespace vibe {

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

}  // namespace vibe

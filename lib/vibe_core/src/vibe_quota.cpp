#include "vibe_quota.h"

#include <cmath>

namespace vibe {

bool QuotaSnapshot::apply(float remaining, std::uint32_t reset, float creditsValue,
                          float totalValue, std::uint32_t nowMs) {
    if (!std::isfinite(remaining) || remaining < 0.0f || remaining > 100.0f ||
        !std::isfinite(creditsValue) || !std::isfinite(totalValue)) {
        return false;
    }

    const bool hasCreditValues = creditsValue != 0.0f || totalValue != 0.0f;
    if (hasCreditValues && (creditsValue < 0.0f || totalValue <= 0.0f || creditsValue > totalValue)) {
        return false;
    }

    remainingPercent = remaining;
    resetInSeconds = reset;
    credits = creditsValue;
    totalCredits = totalValue;
    receivedAtMs = nowMs;
    available = true;
    hasCredits = hasCreditValues;
    return true;
}

QuotaFreshness QuotaSnapshot::freshness(std::uint32_t nowMs, std::uint32_t staleAfterMs) const {
    if (!available) {
        return QuotaFreshness::Unavailable;
    }
    return static_cast<std::uint32_t>(nowMs - receivedAtMs) > staleAfterMs
               ? QuotaFreshness::Stale
               : QuotaFreshness::Fresh;
}

}  // namespace vibe

#pragma once

#include <cstdint>

#include "vibe_quota.h"

namespace vibe {

constexpr float kAgentLabelTextScale = 2.0f;
constexpr float kActionLabelTextScale = 1.0f;

enum class RenderFont : std::uint8_t { OrbitronLight32 };
enum class RenderTextDatum : std::uint8_t { MiddleCenter };

struct LabelRenderStyle {
    float textScale;
    RenderFont font;
    RenderTextDatum datum;
};

constexpr LabelRenderStyle normalAgentLabelRenderStyle() {
    return {kAgentLabelTextScale, RenderFont::OrbitronLight32,
            RenderTextDatum::MiddleCenter};
}

constexpr LabelRenderStyle actionLabelRenderStyle() {
    return {kActionLabelTextScale, RenderFont::OrbitronLight32,
            RenderTextDatum::MiddleCenter};
}

enum class QuotaPresentation : std::uint8_t { Unavailable, Current, Stale };

inline QuotaPresentation quotaPresentation(QuotaFreshness freshness) {
    switch (freshness) {
        case QuotaFreshness::Fresh: return QuotaPresentation::Current;
        case QuotaFreshness::Stale: return QuotaPresentation::Stale;
        case QuotaFreshness::Unavailable: return QuotaPresentation::Unavailable;
    }
    return QuotaPresentation::Unavailable;
}

}  // namespace vibe

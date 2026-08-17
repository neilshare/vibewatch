#pragma once

#include <cstddef>
#include <cstdint>

#include "vibe_approval.h"

namespace vibe {

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

}  // namespace vibe

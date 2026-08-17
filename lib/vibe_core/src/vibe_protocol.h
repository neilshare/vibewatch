#pragma once

#include <cstddef>
#include <cstdint>

#include "vibe_approval.h"

namespace vibe {

enum class ProtocolError : std::uint8_t {
    None, InvalidPayload, UnsupportedVersion, InvalidRequestId,
    InvalidCard, InvalidAgentId, InvalidOperationType, InvalidSummary, InvalidTtl
};

enum class ProtocolErrorCode : std::uint8_t {
    Unauthorized,
    InvalidPayload,
    UnsupportedVersion,
    Busy,
    QueueFull,
    TransportError
};

constexpr std::size_t kProtocolErrorMessageLength = 96;

struct ApprovalDecodeResult {
    ProtocolError error{ProtocolError::InvalidPayload};
    ApprovalRequestV2 request{};
    bool legacy{false};
};

ApprovalDecodeResult decodeApprovalRequest(const std::uint8_t* data, std::size_t length,
                                            bool allowLegacy);
bool encodeApprovalDecision(const ApprovalDecisionV2&, char* output,
                            std::size_t capacity, std::size_t& written);
bool encodeProtocolError(const char* requestId, ProtocolErrorCode code,
                         const char* message, char* output,
                         std::size_t capacity, std::size_t& written);

}  // namespace vibe

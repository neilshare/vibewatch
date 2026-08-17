#pragma once

#include <cstddef>
#include <cstdint>

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

template <typename T> struct OptionalValue {
    bool hasValue{false};
    T value{};
};

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

}  // namespace vibe

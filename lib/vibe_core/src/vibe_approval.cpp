#include "vibe_approval.h"

#include <cstring>

namespace vibe {

namespace {

bool matchingRequestId(const ApprovalRequestV2& left, const ApprovalRequestV2& right) {
    return std::memcmp(left.requestId, right.requestId, kRequestIdLength) == 0;
}

ApprovalDecisionV2 makeDecision(const ApprovalRequestV2& request, ApprovalChoice choice,
                                std::uint32_t nowMs) {
    ApprovalDecisionV2 decision{};
    std::memcpy(decision.requestId, request.requestId, kRequestIdLength);
    decision.choice = choice;
    decision.decidedAtMs = nowMs;
    return decision;
}

}  // namespace

ApprovalAcceptResult ApprovalController::accept(const ApprovalRequestV2& request,
                                                std::uint32_t nowMs) {
    if (pending_) {
        return matchingRequestId(current_, request) ? ApprovalAcceptResult::Duplicate
                                                    : ApprovalAcceptResult::Busy;
    }

    current_ = request;
    receivedAtMs_ = nowMs;
    pending_ = true;
    return ApprovalAcceptResult::Accepted;
}

OptionalValue<ApprovalDecisionV2> ApprovalController::decide(ApprovalChoice choice,
                                                              std::uint32_t nowMs) {
    if (!pending_) {
        return {};
    }

    OptionalValue<ApprovalDecisionV2> result{};
    result.hasValue = true;
    result.value = makeDecision(current_, choice, nowMs);
    pending_ = false;
    return result;
}

OptionalValue<ApprovalDecisionV2> ApprovalController::expireIfNeeded(std::uint32_t nowMs) {
    if (!pending_ || static_cast<std::uint32_t>(nowMs - receivedAtMs_) < current_.ttlMs) {
        return {};
    }
    return decide(ApprovalChoice::Expired, nowMs);
}

OptionalValue<ApprovalDecisionV2> ApprovalController::cancel(std::uint32_t nowMs) {
    return decide(ApprovalChoice::Cancelled, nowMs);
}

bool ApprovalController::pending() const {
    return pending_;
}

const ApprovalRequestV2* ApprovalController::current() const {
    return pending_ ? &current_ : nullptr;
}

}  // namespace vibe

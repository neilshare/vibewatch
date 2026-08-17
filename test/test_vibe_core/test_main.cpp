#include <cstring>
#include <string>

#include <unity.h>
#include "vibe_approval.h"
#include "vibe_protocol.h"
#include "vibe_quota.h"

using namespace vibe;

static ApprovalRequestV2 request(const char* id, std::uint32_t ttl = 30000) {
    ApprovalRequestV2 value{};
    std::strncpy(value.requestId, id, sizeof(value.requestId) - 1);
    value.card = AgentCardId::Codex;
    value.agentId = 0;
    std::strcpy(value.operationType, "EXEC");
    std::strcpy(value.summary, "Run firmware tests");
    value.ttlMs = ttl;
    return value;
}

static ApprovalDecodeResult decode(const std::string& payload, bool allowLegacy = false) {
    return decodeApprovalRequest(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                 payload.size(), allowLegacy);
}

static std::string requestJson(const std::string& requestId, const std::string& card,
                               unsigned agentId, const std::string& operation,
                               const std::string& summary, unsigned ttlMs) {
    return "{\"version\":2,\"kind\":\"approval_request\",\"request_id\":\"" + requestId +
           "\",\"card\":\"" + card + "\",\"agent_id\":" + std::to_string(agentId) +
           ",\"operation_type\":\"" + operation + "\",\"summary\":\"" + summary +
           "\",\"ttl_ms\":" + std::to_string(ttlMs) + "}";
}

void test_accept_duplicate_busy_and_decide() {
    ApprovalController controller;
    const auto first = request("550e8400-e29b-41d4-a716-446655440000");
    const auto second = request("d9428888-122b-11e1-b85c-61cd3cbb3210");
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalAcceptResult::Accepted),
                            static_cast<std::uint8_t>(controller.accept(first, 100)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalAcceptResult::Duplicate),
                            static_cast<std::uint8_t>(controller.accept(first, 120)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalAcceptResult::Busy),
                            static_cast<std::uint8_t>(controller.accept(second, 130)));
    const auto decision = controller.decide(ApprovalChoice::Approve, 200);
    TEST_ASSERT_TRUE(decision.hasValue);
    TEST_ASSERT_EQUAL_STRING(first.requestId, decision.value.requestId);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalChoice::Approve),
                            static_cast<std::uint8_t>(decision.value.choice));
    TEST_ASSERT_FALSE(controller.decide(ApprovalChoice::Reject, 201).hasValue);
}

void test_expiry_and_disconnect_are_not_approval() {
    ApprovalController controller;
    const auto value = request("550e8400-e29b-41d4-a716-446655440000", 5000);
    controller.accept(value, 1000);
    const auto expired = controller.expireIfNeeded(6000);
    TEST_ASSERT_TRUE(expired.hasValue);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalChoice::Expired),
                            static_cast<std::uint8_t>(expired.value.choice));
    controller.accept(value, 7000);
    const auto cancelled = controller.cancel(7100);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalChoice::Cancelled),
                            static_cast<std::uint8_t>(cancelled.value.choice));
}

void test_decodes_the_canonical_v2_request() {
    const std::string payload = R"json({
  "version": 2,
  "kind": "approval_request",
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "card": "codex",
  "agent_id": 0,
  "operation_type": "EXEC",
  "summary": "Run the firmware test suite",
  "ttl_ms": 30000
})json";
    const auto result = decode(payload);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::None),
                            static_cast<std::uint8_t>(result.error));
    TEST_ASSERT_FALSE(result.legacy);
    TEST_ASSERT_EQUAL_STRING("550e8400-e29b-41d4-a716-446655440000", result.request.requestId);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(AgentCardId::Codex),
                            static_cast<std::uint8_t>(result.request.card));
}

void test_rejects_invalid_request_id_card_agent_and_ttl() {
    const char* id = "550e8400-e29b-41d4-a716-446655440000";
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidRequestId),
                            static_cast<std::uint8_t>(decode(requestJson(
                                "550e8400-e29b-41d4-a716-44665544000Z", "codex", 0, "EXEC", "Run", 30000)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidCard),
                            static_cast<std::uint8_t>(decode(requestJson(id, "unknown", 0, "EXEC", "Run", 30000)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidAgentId),
                            static_cast<std::uint8_t>(decode(requestJson(id, "workbuddy", 6, "EXEC", "Run", 30000)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidTtl),
                            static_cast<std::uint8_t>(decode(requestJson(id, "antigravity", 5, "EXEC", "Run", 4999)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidTtl),
                            static_cast<std::uint8_t>(decode(requestJson(id, "codex", 0, "EXEC", "Run", 120001)).error));
}

void test_enforces_utf8_and_byte_boundaries() {
    const char* id = "550e8400-e29b-41d4-a716-446655440000";
    const auto validSummary = std::string(93, 'a') + "\xC3\xA9";
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::None),
                            static_cast<std::uint8_t>(decode(requestJson(
                                id, "codex", 0, std::string(23, 'X'), validSummary, 30000)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidOperationType),
                            static_cast<std::uint8_t>(decode(requestJson(
                                id, "codex", 0, std::string(24, 'X'), "Run", 30000)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidSummary),
                            static_cast<std::uint8_t>(decode(requestJson(
                                id, "codex", 0, "EXEC", std::string(94, 'a') + "\xC3\xA9", 30000)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidSummary),
                            static_cast<std::uint8_t>(decode(requestJson(
                                id, "codex", 0, "EXEC", "bad\xC3", 30000)).error));
}

void test_rejects_oversized_payload_and_gates_legacy_shape() {
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidPayload),
                            static_cast<std::uint8_t>(decode(std::string(513, ' ')).error));
    const std::string legacy = R"json({"method":"v.oai.approval_req","params":{"active":true,"type":"EXEC","summary":"Run firmware tests"}})json";
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidPayload),
                            static_cast<std::uint8_t>(decode(legacy).error));
    const auto accepted = decode(legacy, true);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::None),
                            static_cast<std::uint8_t>(accepted.error));
    TEST_ASSERT_TRUE(accepted.legacy);
}

void test_encodes_the_canonical_v2_decision() {
    ApprovalDecisionV2 decision{};
    std::strcpy(decision.requestId, "550e8400-e29b-41d4-a716-446655440000");
    decision.choice = ApprovalChoice::Approve;
    decision.decidedAtMs = 184230;
    char output[160]{};
    std::size_t written = 0;
    TEST_ASSERT_TRUE(encodeApprovalDecision(decision, output, sizeof(output), written));
    TEST_ASSERT_EQUAL_STRING(
        R"json({"version":2,"kind":"approval_decision","request_id":"550e8400-e29b-41d4-a716-446655440000","decision":"approve","decided_at_ms":184230})json",
        output);
    TEST_ASSERT_EQUAL_UINT(strlen(output), written);
}

void test_quota_defaults_apply_validation_and_freshness() {
    QuotaSnapshot quota{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaFreshness::Unavailable),
                            static_cast<std::uint8_t>(quota.freshness(0, 100)));
    TEST_ASSERT_TRUE(quota.apply(75.0f, 3600, 15.0f, 20.0f, 1000));
    TEST_ASSERT_TRUE(quota.available);
    TEST_ASSERT_TRUE(quota.hasCredits);
    TEST_ASSERT_EQUAL_FLOAT(75.0f, quota.remainingPercent);
    TEST_ASSERT_EQUAL_UINT32(1000, quota.receivedAtMs);
    TEST_ASSERT_FALSE(quota.apply(101.0f, 1, 1.0f, 2.0f, 2000));
    TEST_ASSERT_EQUAL_FLOAT(75.0f, quota.remainingPercent);
    TEST_ASSERT_EQUAL_UINT32(1000, quota.receivedAtMs);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaFreshness::Fresh),
                            static_cast<std::uint8_t>(quota.freshness(1100, 100)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaFreshness::Stale),
                            static_cast<std::uint8_t>(quota.freshness(1101, 100)));
}

void test_quota_accepts_a_snapshot_without_credit_fields() {
    QuotaSnapshot quota{};
    TEST_ASSERT_TRUE(quota.apply(80.0f, 120, 0.0f, 0.0f, 42));
    TEST_ASSERT_TRUE(quota.available);
    TEST_ASSERT_FALSE(quota.hasCredits);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, quota.credits);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, quota.totalCredits);
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_accept_duplicate_busy_and_decide);
    RUN_TEST(test_expiry_and_disconnect_are_not_approval);
    RUN_TEST(test_decodes_the_canonical_v2_request);
    RUN_TEST(test_rejects_invalid_request_id_card_agent_and_ttl);
    RUN_TEST(test_enforces_utf8_and_byte_boundaries);
    RUN_TEST(test_rejects_oversized_payload_and_gates_legacy_shape);
    RUN_TEST(test_encodes_the_canonical_v2_decision);
    RUN_TEST(test_quota_defaults_apply_validation_and_freshness);
    RUN_TEST(test_quota_accepts_a_snapshot_without_credit_fields);
    UNITY_END();
}

void loop() {}

int main(int, char**) {
    setup();
    return 0;
}

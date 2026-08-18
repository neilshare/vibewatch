#include <cstring>
#include <limits>
#include <string>

#include <unity.h>
#include "vibe_approval.h"
#include "vibe_ingress_core.h"
#include "vibe_protocol.h"
#include "vibe_quota.h"
#include "vibe_render.h"

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

void test_rejects_embedded_nuls_in_protocol_strings() {
    const char* id = "550e8400-e29b-41d4-a716-446655440000";
    auto kindWithNul = requestJson(id, "codex", 0, "EXEC", "Run", 30000);
    const auto kindPosition = kindWithNul.find("approval_request");
    kindWithNul.replace(kindPosition, sizeof("approval_request") - 1, "approval\\u0000request");
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidPayload),
                            static_cast<std::uint8_t>(decode(kindWithNul).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidRequestId),
                            static_cast<std::uint8_t>(decode(requestJson(
                                "550e8400-e29b-41d4-a716-446655440000\\u0000", "codex", 0, "EXEC", "Run", 30000)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidCard),
                            static_cast<std::uint8_t>(decode(requestJson(
                                id, "codex\\u0000", 0, "EXEC", "Run", 30000)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidOperationType),
                            static_cast<std::uint8_t>(decode(requestJson(
                                id, "codex", 0, "EX\\u0000EC", "Run", 30000)).error));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ProtocolError::InvalidSummary),
                            static_cast<std::uint8_t>(decode(requestJson(
                                id, "codex", 0, "EXEC", "Run\\u0000tests", 30000)).error));
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

void test_rejects_a_non_terminated_decision_id() {
    ApprovalDecisionV2 decision{};
    std::memset(decision.requestId, 'a', sizeof(decision.requestId));
    decision.choice = ApprovalChoice::Approve;
    char output[160]{};
    std::size_t written = 123;
    TEST_ASSERT_FALSE(encodeApprovalDecision(decision, output, sizeof(output), written));
    TEST_ASSERT_EQUAL_UINT(0, written);
}

void test_wraparound_expiry_and_quota_freshness() {
    constexpr std::uint32_t approvalReceivedAt = std::numeric_limits<std::uint32_t>::max() - 255;
    ApprovalController controller;
    controller.accept(request("550e8400-e29b-41d4-a716-446655440000", 5000), approvalReceivedAt);
    TEST_ASSERT_FALSE(controller.expireIfNeeded(4743).hasValue);
    const auto expired = controller.expireIfNeeded(4744);
    TEST_ASSERT_TRUE(expired.hasValue);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(ApprovalChoice::Expired),
                            static_cast<std::uint8_t>(expired.value.choice));

    QuotaSnapshot quota{};
    constexpr std::uint32_t quotaReceivedAt = std::numeric_limits<std::uint32_t>::max() - 99;
    TEST_ASSERT_TRUE(quota.apply(50.0f, 60, 0.0f, 0.0f, quotaReceivedAt));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaFreshness::Fresh),
                            static_cast<std::uint8_t>(quota.freshness(199, 299)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaFreshness::Stale),
                            static_cast<std::uint8_t>(quota.freshness(200, 299)));
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

void test_quota_snapshot_truthfully_reports_startup_and_staleness() {
    QuotaSnapshot quota{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaFreshness::Unavailable),
                            static_cast<std::uint8_t>(quota.freshness(0, 180000)));

    TEST_ASSERT_TRUE(quota.apply(86.0f, 3600, 0.0f, 0.0f, 1000));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaFreshness::Fresh),
                            static_cast<std::uint8_t>(quota.freshness(181000, 180000)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaFreshness::Stale),
                            static_cast<std::uint8_t>(quota.freshness(181001, 180000)));

    TEST_ASSERT_FALSE(quota.apply(101.0f, 1, 0.0f, 0.0f, 2000));
    TEST_ASSERT_EQUAL_FLOAT(86.0f, quota.remainingPercent);
    TEST_ASSERT_EQUAL_UINT32(1000, quota.receivedAtMs);
}

void test_agent_label_scale_preserves_action_label_scale() {
    const auto agentStyle = normalAgentLabelRenderStyle();
    const auto actionStyle = actionLabelRenderStyle();
    TEST_ASSERT_EQUAL_FLOAT(2.0f, agentStyle.textScale);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(RenderFont::OrbitronLight32),
                            static_cast<std::uint8_t>(agentStyle.font));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(RenderTextDatum::MiddleCenter),
                            static_cast<std::uint8_t>(agentStyle.datum));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, actionStyle.textScale);
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(RenderFont::OrbitronLight32),
                            static_cast<std::uint8_t>(actionStyle.font));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(RenderTextDatum::MiddleCenter),
                            static_cast<std::uint8_t>(actionStyle.datum));
}

void test_quota_presentation_marks_only_stale_data_as_stale() {
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaPresentation::Unavailable),
                            static_cast<std::uint8_t>(quotaPresentation(QuotaFreshness::Unavailable)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaPresentation::Current),
                            static_cast<std::uint8_t>(quotaPresentation(QuotaFreshness::Fresh)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(QuotaPresentation::Stale),
                            static_cast<std::uint8_t>(quotaPresentation(QuotaFreshness::Stale)));
}

void test_quota_accepts_a_snapshot_without_credit_fields() {
    QuotaSnapshot quota{};
    TEST_ASSERT_TRUE(quota.apply(80.0f, 120, 0.0f, 0.0f, 42));
    TEST_ASSERT_TRUE(quota.available);
    TEST_ASSERT_FALSE(quota.hasCredits);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, quota.credits);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, quota.totalCredits);
}

void test_ingress_buffer_is_bounded_and_fifo() {
    IngressBuffer<6, 512> ingress;
    for (std::uint8_t value = 0; value < 6; ++value) {
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<std::uint8_t>(EnqueueResult::Accepted),
            static_cast<std::uint8_t>(
                ingress.push(IngressKind::Quota, value + 1, &value, sizeof(value))));
    }

    const std::uint8_t seventh = 6;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(EnqueueResult::QueueFull),
        static_cast<std::uint8_t>(
            ingress.push(IngressKind::Quota, 7, &seventh, sizeof(seventh))));

    for (std::uint8_t expected = 0; expected < 6; ++expected) {
        IngressMessage message{};
        TEST_ASSERT_TRUE(ingress.pop(message));
        TEST_ASSERT_EQUAL_UINT16(expected + 1, message.connectionHandle);
        TEST_ASSERT_EQUAL_UINT16(1, message.length);
        TEST_ASSERT_EQUAL_UINT8(expected, message.payload[0]);
    }
    IngressMessage message{};
    TEST_ASSERT_FALSE(ingress.pop(message));
}

void test_ingress_buffer_rejects_oversized_payloads() {
    IngressBuffer<6, 512> ingress;
    std::array<std::uint8_t, 513> payload{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(EnqueueResult::PayloadTooLarge),
        static_cast<std::uint8_t>(ingress.push(
            IngressKind::Approval, 1, payload.data(), payload.size())));
}

void test_ingress_buffer_rejects_missing_payload_bytes() {
    IngressBuffer<6, 512> ingress;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(EnqueueResult::PayloadTooLarge),
        static_cast<std::uint8_t>(
            ingress.push(IngressKind::Quota, 1, nullptr, 1)));
}

static HidChunk hidChunk(std::uint16_t handle, const char* payload) {
    HidChunk chunk{};
    chunk.connectionHandle = handle;
    chunk.length = static_cast<std::uint8_t>(std::strlen(payload));
    std::memcpy(chunk.payload.data(), payload, chunk.length);
    return chunk;
}

static HidChunk hidChunk(HidStreamToken token, const char* payload) {
    HidChunk chunk = hidChunk(token.connectionHandle, payload);
    chunk.connectionGeneration = token.connectionGeneration;
    chunk.streamEpoch = token.streamEpoch;
    return chunk;
}

void test_hid_rpc_assembly_is_connection_scoped_and_main_loop_parsed() {
    HidRpcAssembler assembler;
    HidRpcView completed{};

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(HidConsumeResult::Incomplete),
        static_cast<std::uint8_t>(assembler.consume(
            hidChunk(1, R"json({"method":"one","value":")json"), completed)));
    TEST_ASSERT_NULL(completed.data);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(HidConsumeResult::Complete),
        static_cast<std::uint8_t>(assembler.consume(
            hidChunk(2, R"json({"method":"two"})json"), completed)));
    TEST_ASSERT_EQUAL_UINT16(2, completed.connectionHandle);
    TEST_ASSERT_EQUAL_UINT(std::strlen(R"json({"method":"two"})json"), completed.length);
    TEST_ASSERT_EQUAL_MEMORY(R"json({"method":"two"})json", completed.data,
                             completed.length);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(HidConsumeResult::Complete),
        static_cast<std::uint8_t>(assembler.consume(
            hidChunk(1, R"json(done"})json"), completed)));
    TEST_ASSERT_EQUAL_UINT16(1, completed.connectionHandle);
    TEST_ASSERT_EQUAL_UINT(std::strlen(R"json({"method":"one","value":"done"})json"),
                           completed.length);
    TEST_ASSERT_EQUAL_MEMORY(R"json({"method":"one","value":"done"})json",
                             completed.data, completed.length);
}

void test_encodes_bounded_v2_protocol_error() {
    char output[256]{};
    std::size_t written = 0;
    TEST_ASSERT_TRUE(encodeProtocolError(
        "550e8400-e29b-41d4-a716-446655440000",
        ProtocolErrorCode::Busy, "another request is pending",
        output, sizeof(output), written));
    TEST_ASSERT_EQUAL_STRING(
        R"json({"version":2,"kind":"error","request_id":"550e8400-e29b-41d4-a716-446655440000","code":"busy","message":"another request is pending"})json",
        output);
    TEST_ASSERT_EQUAL_UINT(std::strlen(output), written);
}

void test_new_approval_cannot_expire_in_accepting_iteration_at_wrap() {
    ApprovalController controller;
    ApprovalIteration iteration(std::numeric_limits<std::uint32_t>::max() - 7);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(ApprovalAcceptResult::Accepted),
        static_cast<std::uint8_t>(iteration.accept(
            controller,
            request("550e8400-e29b-41d4-a716-446655440000", 5000))));
    TEST_ASSERT_FALSE(iteration.expire(controller).hasValue);
    TEST_ASSERT_TRUE(controller.pending());
}

void test_hid_rpc_rejects_all_legacy_approval_method_aliases() {
    TEST_ASSERT_FALSE(hidRpcMethodAllowed("v.oai.approval_req"));
    TEST_ASSERT_FALSE(hidRpcMethodAllowed("approval"));
    TEST_ASSERT_FALSE(hidRpcMethodAllowed("v.oai.prompt"));
    TEST_ASSERT_TRUE(hidRpcMethodAllowed("v.oai.thstatus"));
    TEST_ASSERT_TRUE(hidRpcMethodAllowed("quota"));
}

void test_indication_timeout_requires_active_disconnect() {
    TEST_ASSERT_FALSE(indicationTimeoutRequiresDisconnect(
        false, 5000, 5000));
    TEST_ASSERT_FALSE(indicationTimeoutRequiresDisconnect(
        true, 4999, 5000));
    TEST_ASSERT_TRUE(indicationTimeoutRequiresDisconnect(
        true, 5000, 5000));
    TEST_ASSERT_TRUE(indicationTimeoutRequiresDisconnect(
        true, 3, std::numeric_limits<std::uint32_t>::max() - 2));
}

void test_hid_overflow_rejects_suffix_until_new_connection_generation() {
    HidStreamTracker<3> streams;
    HidRpcAssembler assembler;
    HidRpcView completed{};

    const auto beforeOverflow = streams.connect(1);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(HidConsumeResult::Incomplete),
        static_cast<std::uint8_t>(assembler.consume(
            hidChunk(beforeOverflow, R"json({"method":"old")json"), completed)));

    streams.noteEnqueueResult(beforeOverflow, EnqueueResult::QueueFull);
    const auto afterOverflow = streams.current(1);
    TEST_ASSERT_FALSE(afterOverflow.acceptingChunks);
    TEST_ASSERT_FALSE(streams.isCurrent(hidChunk(beforeOverflow, R"json(})json")));
    TEST_ASSERT_FALSE(streams.isCurrent(
        hidChunk(afterOverflow, R"json({"method":"suffix"})json")));

    assembler.clear(1);
    streams.disconnect(1);
    const auto reconnected = streams.connect(1);
    TEST_ASSERT_TRUE(reconnected.acceptingChunks);
    TEST_ASSERT_TRUE(reconnected.connectionGeneration !=
                     beforeOverflow.connectionGeneration);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(HidConsumeResult::Complete),
        static_cast<std::uint8_t>(assembler.consume(
            hidChunk(reconnected, R"json({"method":"fresh"})json"), completed)));
    TEST_ASSERT_EQUAL_MEMORY(R"json({"method":"fresh"})json", completed.data,
                             completed.length);
}

void test_hid_reconnect_generation_rejects_prior_handle_chunks() {
    HidStreamTracker<3> streams;
    HidRpcAssembler assembler;
    HidRpcView completed{};

    const auto firstConnection = streams.connect(2);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(HidConsumeResult::Incomplete),
        static_cast<std::uint8_t>(assembler.consume(
            hidChunk(firstConnection, R"json({"method":"stale")json"), completed)));
    streams.disconnect(2);
    const auto secondConnection = streams.connect(2);
    streams.noteEnqueueResult(firstConnection, EnqueueResult::QueueFull);

    TEST_ASSERT_FALSE(streams.isCurrent(
        hidChunk(firstConnection, R"json(})json")));
    TEST_ASSERT_TRUE(streams.current(2).acceptingChunks);
    TEST_ASSERT_TRUE(secondConnection.connectionGeneration !=
                     firstConnection.connectionGeneration);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<std::uint8_t>(HidConsumeResult::Complete),
        static_cast<std::uint8_t>(assembler.consume(
            hidChunk(secondConnection, R"json({"method":"fresh"})json"), completed)));
    TEST_ASSERT_EQUAL_MEMORY(R"json({"method":"fresh"})json", completed.data,
                             completed.length);
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_accept_duplicate_busy_and_decide);
    RUN_TEST(test_expiry_and_disconnect_are_not_approval);
    RUN_TEST(test_decodes_the_canonical_v2_request);
    RUN_TEST(test_rejects_invalid_request_id_card_agent_and_ttl);
    RUN_TEST(test_enforces_utf8_and_byte_boundaries);
    RUN_TEST(test_rejects_embedded_nuls_in_protocol_strings);
    RUN_TEST(test_rejects_oversized_payload_and_gates_legacy_shape);
    RUN_TEST(test_encodes_the_canonical_v2_decision);
    RUN_TEST(test_rejects_a_non_terminated_decision_id);
    RUN_TEST(test_quota_defaults_apply_validation_and_freshness);
    RUN_TEST(test_quota_snapshot_truthfully_reports_startup_and_staleness);
    RUN_TEST(test_agent_label_scale_preserves_action_label_scale);
    RUN_TEST(test_quota_presentation_marks_only_stale_data_as_stale);
    RUN_TEST(test_quota_accepts_a_snapshot_without_credit_fields);
    RUN_TEST(test_wraparound_expiry_and_quota_freshness);
    RUN_TEST(test_ingress_buffer_is_bounded_and_fifo);
    RUN_TEST(test_ingress_buffer_rejects_oversized_payloads);
    RUN_TEST(test_ingress_buffer_rejects_missing_payload_bytes);
    RUN_TEST(test_hid_rpc_assembly_is_connection_scoped_and_main_loop_parsed);
    RUN_TEST(test_encodes_bounded_v2_protocol_error);
    RUN_TEST(test_new_approval_cannot_expire_in_accepting_iteration_at_wrap);
    RUN_TEST(test_hid_rpc_rejects_all_legacy_approval_method_aliases);
    RUN_TEST(test_indication_timeout_requires_active_disconnect);
    RUN_TEST(test_hid_overflow_rejects_suffix_until_new_connection_generation);
    RUN_TEST(test_hid_reconnect_generation_rejects_prior_handle_chunks);
    UNITY_END();
}

void loop() {}

int main(int, char**) {
    setup();
    return 0;
}

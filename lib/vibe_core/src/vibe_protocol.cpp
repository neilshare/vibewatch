#include "vibe_protocol.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstring>

namespace vibe {

namespace {

constexpr std::size_t kMaxApprovalPayloadLength = 512;

bool isCanonicalUuid(const char* value) {
    if (value == nullptr || std::strlen(value) != kRequestIdLength - 1) {
        return false;
    }

    for (std::size_t index = 0; index < kRequestIdLength - 1; ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') {
                return false;
            }
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool isContinuationByte(unsigned char value) {
    return value >= 0x80 && value <= 0xBF;
}

bool isValidUtf8(const char* value, std::size_t length) {
    for (std::size_t index = 0; index < length;) {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        if (lead <= 0x7F) {
            ++index;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            if (index + 1 >= length || !isContinuationByte(static_cast<unsigned char>(value[index + 1]))) {
                return false;
            }
            index += 2;
        } else if (lead == 0xE0) {
            if (index + 2 >= length || static_cast<unsigned char>(value[index + 1]) < 0xA0 ||
                static_cast<unsigned char>(value[index + 1]) > 0xBF ||
                !isContinuationByte(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
        } else if ((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) {
            if (index + 2 >= length || !isContinuationByte(static_cast<unsigned char>(value[index + 1])) ||
                !isContinuationByte(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
        } else if (lead == 0xED) {
            if (index + 2 >= length || static_cast<unsigned char>(value[index + 1]) < 0x80 ||
                static_cast<unsigned char>(value[index + 1]) > 0x9F ||
                !isContinuationByte(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
        } else if (lead == 0xF0) {
            if (index + 3 >= length || static_cast<unsigned char>(value[index + 1]) < 0x90 ||
                static_cast<unsigned char>(value[index + 1]) > 0xBF ||
                !isContinuationByte(static_cast<unsigned char>(value[index + 2])) ||
                !isContinuationByte(static_cast<unsigned char>(value[index + 3]))) {
                return false;
            }
            index += 4;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            if (index + 3 >= length || !isContinuationByte(static_cast<unsigned char>(value[index + 1])) ||
                !isContinuationByte(static_cast<unsigned char>(value[index + 2])) ||
                !isContinuationByte(static_cast<unsigned char>(value[index + 3]))) {
                return false;
            }
            index += 4;
        } else if (lead == 0xF4) {
            if (index + 3 >= length || static_cast<unsigned char>(value[index + 1]) < 0x80 ||
                static_cast<unsigned char>(value[index + 1]) > 0x8F ||
                !isContinuationByte(static_cast<unsigned char>(value[index + 2])) ||
                !isContinuationByte(static_cast<unsigned char>(value[index + 3]))) {
                return false;
            }
            index += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool parseCard(const char* value, AgentCardId& card) {
    if (value == nullptr) {
        return false;
    }

    const std::size_t length = std::strlen(value);
    if (length == 0 || length >= 16) {
        return false;
    }

    char normalized[16]{};
    for (std::size_t index = 0; index < length; ++index) {
        normalized[index] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[index])));
    }
    if (std::strcmp(normalized, "codex") == 0) {
        card = AgentCardId::Codex;
        return true;
    }
    if (std::strcmp(normalized, "workbuddy") == 0) {
        card = AgentCardId::Workbuddy;
        return true;
    }
    if (std::strcmp(normalized, "antigravity") == 0) {
        card = AgentCardId::Antigravity;
        return true;
    }
    return false;
}

bool copyText(char* destination, std::size_t capacity, const char* source) {
    const std::size_t length = std::strlen(source);
    if (length >= capacity) {
        return false;
    }
    std::memcpy(destination, source, length);
    return true;
}

ProtocolError validateText(const char* value, std::size_t capacity, ProtocolError error) {
    if (value == nullptr) {
        return error;
    }
    const std::size_t length = std::strlen(value);
    if (length == 0 || length >= capacity || !isValidUtf8(value, length)) {
        return error;
    }
    return ProtocolError::None;
}

ApprovalDecodeResult decodeV2(JsonObjectConst root) {
    ApprovalDecodeResult result{};
    const JsonVariantConst version = root["version"];
    if (!version.is<std::uint32_t>()) {
        return result;
    }
    if (version.as<std::uint32_t>() != 2) {
        result.error = ProtocolError::UnsupportedVersion;
        return result;
    }

    const JsonVariantConst kind = root["kind"];
    if (!kind.is<const char*>() || std::strcmp(kind.as<const char*>(), "approval_request") != 0) {
        return result;
    }

    const JsonVariantConst requestId = root["request_id"];
    if (!requestId.is<const char*>() || !isCanonicalUuid(requestId.as<const char*>())) {
        result.error = ProtocolError::InvalidRequestId;
        return result;
    }

    const JsonVariantConst card = root["card"];
    if (!card.is<const char*>() || !parseCard(card.as<const char*>(), result.request.card)) {
        result.error = ProtocolError::InvalidCard;
        return result;
    }

    const JsonVariantConst agentId = root["agent_id"];
    if (!agentId.is<std::uint8_t>() || agentId.as<std::uint8_t>() > 5) {
        result.error = ProtocolError::InvalidAgentId;
        return result;
    }
    result.request.agentId = agentId.as<std::uint8_t>();

    const JsonVariantConst operation = root["operation_type"];
    if (!operation.is<const char*>()) {
        result.error = ProtocolError::InvalidOperationType;
        return result;
    }
    if (const auto error = validateText(operation.as<const char*>(), kOperationTypeLength,
                                        ProtocolError::InvalidOperationType);
        error != ProtocolError::None) {
        result.error = error;
        return result;
    }

    const JsonVariantConst summary = root["summary"];
    if (!summary.is<const char*>()) {
        result.error = ProtocolError::InvalidSummary;
        return result;
    }
    if (const auto error = validateText(summary.as<const char*>(), kApprovalSummaryLength,
                                        ProtocolError::InvalidSummary);
        error != ProtocolError::None) {
        result.error = error;
        return result;
    }

    const JsonVariantConst ttl = root["ttl_ms"];
    if (!ttl.is<std::uint32_t>() || ttl.as<std::uint32_t>() < kMinApprovalTtlMs ||
        ttl.as<std::uint32_t>() > kMaxApprovalTtlMs) {
        result.error = ProtocolError::InvalidTtl;
        return result;
    }

    copyText(result.request.requestId, sizeof(result.request.requestId), requestId.as<const char*>());
    copyText(result.request.operationType, sizeof(result.request.operationType), operation.as<const char*>());
    copyText(result.request.summary, sizeof(result.request.summary), summary.as<const char*>());
    result.request.ttlMs = ttl.as<std::uint32_t>();
    result.error = ProtocolError::None;
    return result;
}

ApprovalDecodeResult decodeLegacy(JsonObjectConst root) {
    ApprovalDecodeResult result{};
    const JsonVariantConst method = root["method"];
    const JsonVariantConst params = root["params"];
    if (!method.is<const char*>() || std::strcmp(method.as<const char*>(), "v.oai.approval_req") != 0 ||
        !params.is<JsonObjectConst>()) {
        return result;
    }

    const JsonObjectConst values = params.as<JsonObjectConst>();
    const JsonVariantConst active = values["active"];
    const JsonVariantConst type = values["type"];
    const JsonVariantConst summary = values["summary"];
    if (!active.is<bool>() || !active.as<bool>() || !type.is<const char*>() || !summary.is<const char*>()) {
        return result;
    }

    if (const auto error = validateText(type.as<const char*>(), kOperationTypeLength,
                                        ProtocolError::InvalidOperationType);
        error != ProtocolError::None) {
        result.error = error;
        return result;
    }
    if (const auto error = validateText(summary.as<const char*>(), kApprovalSummaryLength,
                                        ProtocolError::InvalidSummary);
        error != ProtocolError::None) {
        result.error = error;
        return result;
    }

    result.request.card = AgentCardId::Codex;
    const JsonVariantConst card = values["card"];
    if (!card.isNull() && (!card.is<const char*>() || !parseCard(card.as<const char*>(), result.request.card))) {
        result.error = ProtocolError::InvalidCard;
        return result;
    }

    const JsonVariantConst agent = values["agentId"].isNull() ? values["agent"] : values["agentId"];
    if (!agent.isNull()) {
        if (!agent.is<std::uint8_t>() || agent.as<std::uint8_t>() > 5) {
            result.error = ProtocolError::InvalidAgentId;
            return result;
        }
        result.request.agentId = agent.as<std::uint8_t>();
    }

    copyText(result.request.operationType, sizeof(result.request.operationType), type.as<const char*>());
    copyText(result.request.summary, sizeof(result.request.summary), summary.as<const char*>());
    result.request.ttlMs = 30000;
    result.error = ProtocolError::None;
    result.legacy = true;
    return result;
}

const char* decisionName(ApprovalChoice choice) {
    switch (choice) {
        case ApprovalChoice::Approve: return "approve";
        case ApprovalChoice::Reject: return "reject";
        case ApprovalChoice::Expired: return "expired";
        case ApprovalChoice::Cancelled: return "cancelled";
    }
    return nullptr;
}

}  // namespace

ApprovalDecodeResult decodeApprovalRequest(const std::uint8_t* data, std::size_t length,
                                            bool allowLegacy) {
    if (data == nullptr || length == 0 || length > kMaxApprovalPayloadLength) {
        return {};
    }

    JsonDocument document;
    if (deserializeJson(document, data, length)) {
        return {};
    }
    const JsonObjectConst root = document.as<JsonObjectConst>();
    if (root.isNull()) {
        return {};
    }

    if (!root["version"].isNull()) {
        return decodeV2(root);
    }
    return allowLegacy ? decodeLegacy(root) : ApprovalDecodeResult{};
}

bool encodeApprovalDecision(const ApprovalDecisionV2& decision, char* output,
                            std::size_t capacity, std::size_t& written) {
    written = 0;
    const char* choice = decisionName(decision.choice);
    if (output == nullptr || capacity == 0 || choice == nullptr || !isCanonicalUuid(decision.requestId)) {
        return false;
    }

    JsonDocument document;
    document["version"] = 2;
    document["kind"] = "approval_decision";
    document["request_id"] = decision.requestId;
    document["decision"] = choice;
    document["decided_at_ms"] = decision.decidedAtMs;
    const std::size_t required = measureJson(document);
    if (required + 1 > capacity) {
        return false;
    }
    written = serializeJson(document, output, capacity);
    return written == required;
}

}  // namespace vibe

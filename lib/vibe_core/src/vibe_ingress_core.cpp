#include "vibe_ingress_core.h"

#include <ArduinoJson.h>

namespace vibe {

bool hidRpcMethodAllowed(const char* method) {
    if (method == nullptr) {
        return false;
    }
    return std::strcmp(method, "v.oai.approval_req") != 0 &&
           std::strcmp(method, "approval") != 0 &&
           std::strcmp(method, "v.oai.prompt") != 0;
}

HidRpcAssembler::Assembly* HidRpcAssembler::findOrCreate(
    const HidChunk& chunk) {
    for (auto& assembly : assemblies_) {
        if (assembly.active &&
            assembly.connectionHandle == chunk.connectionHandle) {
            if (assembly.connectionGeneration != chunk.connectionGeneration ||
                assembly.streamEpoch != chunk.streamEpoch) {
                assembly = {};
                assembly.active = true;
                assembly.connectionHandle = chunk.connectionHandle;
                assembly.connectionGeneration = chunk.connectionGeneration;
                assembly.streamEpoch = chunk.streamEpoch;
            }
            return &assembly;
        }
    }
    for (auto& assembly : assemblies_) {
        if (!assembly.active) {
            assembly = {};
            assembly.active = true;
            assembly.connectionHandle = chunk.connectionHandle;
            assembly.connectionGeneration = chunk.connectionGeneration;
            assembly.streamEpoch = chunk.streamEpoch;
            return &assembly;
        }
    }
    return nullptr;
}

HidConsumeResult HidRpcAssembler::consume(const HidChunk& chunk,
                                          HidRpcView& completed) {
    completed = {};
    if (chunk.length == 0 || chunk.length > chunk.payload.size()) {
        return HidConsumeResult::InvalidPayload;
    }

    Assembly* assembly = findOrCreate(chunk);
    if (assembly == nullptr) {
        return HidConsumeResult::ConnectionLimit;
    }
    if (assembly->completed) {
        assembly->length = 0;
        assembly->completed = false;
    }
    if (assembly->length + chunk.length > assembly->payload.size()) {
        *assembly = {};
        return HidConsumeResult::PayloadTooLarge;
    }

    std::memcpy(assembly->payload.data() + assembly->length,
                chunk.payload.data(), chunk.length);
    assembly->length += chunk.length;

    JsonDocument document;
    const DeserializationError error = deserializeJson(
        document, assembly->payload.data(), assembly->length);
    if (error == DeserializationError::IncompleteInput) {
        return HidConsumeResult::Incomplete;
    }
    if (error) {
        *assembly = {};
        return HidConsumeResult::InvalidPayload;
    }

    assembly->completed = true;
    completed.connectionHandle = assembly->connectionHandle;
    completed.data = assembly->payload.data();
    completed.length = assembly->length;
    return HidConsumeResult::Complete;
}

void HidRpcAssembler::clear(std::uint16_t connectionHandle) {
    for (auto& assembly : assemblies_) {
        if (assembly.active && assembly.connectionHandle == connectionHandle) {
            assembly = {};
            return;
        }
    }
}

}  // namespace vibe

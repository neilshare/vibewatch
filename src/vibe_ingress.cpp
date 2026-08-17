#include "vibe_ingress.h"

#include <Arduino.h>

#include <cstring>

namespace vibe {

namespace {

constexpr std::size_t kIngressQueueCapacity = 6;
constexpr std::size_t kHidChunkQueueCapacity = 12;

QueueHandle_t g_ingressQueue = nullptr;
QueueHandle_t g_hidChunkQueue = nullptr;

}  // namespace

bool initializeIngressQueue() {
    if (g_ingressQueue == nullptr) {
        g_ingressQueue = xQueueCreate(kIngressQueueCapacity,
                                      sizeof(IngressMessage));
    }
    if (g_hidChunkQueue == nullptr) {
        g_hidChunkQueue = xQueueCreate(kHidChunkQueueCapacity,
                                       sizeof(HidChunk));
    }
    return g_ingressQueue != nullptr && g_hidChunkQueue != nullptr;
}

EnqueueResult enqueueGattWrite(IngressKind kind,
                               std::uint16_t connectionHandle,
                               std::uint32_t connectionGeneration,
                               const std::uint8_t* data,
                               std::size_t length,
                               bool authorized) {
    if (!authorized) {
        return EnqueueResult::Unauthorized;
    }
    if (length > kIngressPayloadLength ||
        (length > 0 && data == nullptr)) {
        return EnqueueResult::PayloadTooLarge;
    }
    if (g_ingressQueue == nullptr) {
        return EnqueueResult::QueueFull;
    }

    IngressMessage message{};
    message.kind = kind;
    message.connectionHandle = connectionHandle;
    message.connectionGeneration = connectionGeneration;
    message.length = static_cast<std::uint16_t>(length);
    if (length > 0) {
        std::memcpy(message.payload.data(), data, length);
    }
    return xQueueSend(g_ingressQueue, &message, 0) == pdTRUE
               ? EnqueueResult::Accepted
               : EnqueueResult::QueueFull;
}

EnqueueResult enqueueHidChunk(std::uint16_t connectionHandle,
                              std::uint32_t connectionGeneration,
                              std::uint32_t streamEpoch,
                              const std::uint8_t* data,
                              std::size_t length) {
    if (length == 0 || length > kHidChunkPayloadLength || data == nullptr) {
        return EnqueueResult::PayloadTooLarge;
    }
    if (g_hidChunkQueue == nullptr) {
        return EnqueueResult::QueueFull;
    }

    HidChunk chunk{};
    chunk.connectionHandle = connectionHandle;
    chunk.connectionGeneration = connectionGeneration;
    chunk.streamEpoch = streamEpoch;
    chunk.length = static_cast<std::uint8_t>(length);
    std::memcpy(chunk.payload.data(), data, length);
    return xQueueSend(g_hidChunkQueue, &chunk, 0) == pdTRUE
               ? EnqueueResult::Accepted
               : EnqueueResult::QueueFull;
}

bool dequeueIngress(IngressMessage& message) {
    return g_ingressQueue != nullptr &&
           xQueueReceive(g_ingressQueue, &message, 0) == pdTRUE;
}

bool dequeueHidChunk(HidChunk& chunk) {
    return g_hidChunkQueue != nullptr &&
           xQueueReceive(g_hidChunkQueue, &chunk, 0) == pdTRUE;
}

}  // namespace vibe

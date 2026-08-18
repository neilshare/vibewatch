#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "vibe_approval.h"

namespace vibe {

constexpr std::size_t kIngressPayloadLength = 512;
constexpr std::size_t kHidChunkPayloadLength = 61;
constexpr std::size_t kHidRpcAssemblyLength = 2048;

enum class IngressKind : std::uint8_t { Quota, Approval, Disconnect };

struct IngressMessage {
    IngressKind kind{IngressKind::Quota};
    std::uint16_t connectionHandle{0};
    std::uint32_t connectionGeneration{0};
    std::uint16_t length{0};
    std::array<std::uint8_t, kIngressPayloadLength> payload{};
};

struct HidChunk {
    std::uint16_t connectionHandle{0};
    std::uint32_t connectionGeneration{0};
    std::uint32_t streamEpoch{0};
    std::uint8_t length{0};
    std::array<std::uint8_t, kHidChunkPayloadLength> payload{};
};

enum class EnqueueResult : std::uint8_t {
    Accepted,
    PayloadTooLarge,
    QueueFull,
    Unauthorized
};

class ApprovalIteration {
  public:
    explicit ApprovalIteration(std::uint32_t nowMs) : nowMs_(nowMs) {}

    ApprovalAcceptResult accept(ApprovalController& controller,
                                const ApprovalRequestV2& request) const {
        return controller.accept(request, nowMs_);
    }

    OptionalValue<ApprovalDecisionV2> expire(
        ApprovalController& controller) const {
        return controller.expireIfNeeded(nowMs_);
    }

    std::uint32_t nowMs() const { return nowMs_; }

  private:
    std::uint32_t nowMs_;
};

bool hidRpcMethodAllowed(const char* method);
bool indicationTimeoutRequiresDisconnect(bool active, std::uint32_t nowMs,
                                         std::uint32_t deadlineMs);

struct HidStreamToken {
    std::uint16_t connectionHandle{0};
    std::uint32_t connectionGeneration{0};
    std::uint32_t streamEpoch{0};
    bool active{false};
    bool acceptingChunks{false};
};

template <std::size_t Capacity>
class HidStreamTracker {
  public:
    HidStreamToken connect(std::uint16_t connectionHandle) {
        Slot* slot = find(connectionHandle);
        if (slot == nullptr) {
            slot = findInactive();
        }
        if (slot == nullptr) {
            return {};
        }
        slot->connectionHandle = connectionHandle;
        slot->connectionGeneration = nextGeneration_++;
        if (nextGeneration_ == 0) {
            nextGeneration_ = 1;
        }
        slot->streamEpoch = 1;
        slot->active = true;
        slot->acceptingChunks = true;
        return token(*slot);
    }

    void disconnect(std::uint16_t connectionHandle) {
        Slot* slot = find(connectionHandle);
        if (slot != nullptr) {
            slot->active = false;
            slot->acceptingChunks = false;
            ++slot->streamEpoch;
        }
    }

    void noteEnqueueResult(const HidStreamToken& attempted,
                           EnqueueResult result) {
        if (result != EnqueueResult::QueueFull) {
            return;
        }
        Slot* slot = find(attempted.connectionHandle);
        if (slot != nullptr && slot->active && slot->acceptingChunks &&
            slot->connectionGeneration == attempted.connectionGeneration &&
            slot->streamEpoch == attempted.streamEpoch) {
            slot->acceptingChunks = false;
            ++slot->streamEpoch;
            if (slot->streamEpoch == 0) {
                slot->streamEpoch = 1;
            }
        }
    }

    HidStreamToken current(std::uint16_t connectionHandle) const {
        const Slot* slot = find(connectionHandle);
        return slot == nullptr ? HidStreamToken{} : token(*slot);
    }

    bool isCurrent(const HidChunk& chunk) const {
        const HidStreamToken value = current(chunk.connectionHandle);
        return value.active && value.acceptingChunks &&
               value.connectionGeneration == chunk.connectionGeneration &&
               value.streamEpoch == chunk.streamEpoch;
    }

  private:
    struct Slot {
        bool active{false};
        std::uint16_t connectionHandle{0};
        std::uint32_t connectionGeneration{0};
        std::uint32_t streamEpoch{0};
        bool acceptingChunks{false};
    };

    Slot* find(std::uint16_t connectionHandle) {
        for (auto& slot : slots_) {
            if (slot.connectionGeneration != 0 &&
                slot.connectionHandle == connectionHandle) {
                return &slot;
            }
        }
        return nullptr;
    }

    const Slot* find(std::uint16_t connectionHandle) const {
        for (const auto& slot : slots_) {
            if (slot.connectionGeneration != 0 &&
                slot.connectionHandle == connectionHandle) {
                return &slot;
            }
        }
        return nullptr;
    }

    Slot* findInactive() {
        for (auto& slot : slots_) {
            if (!slot.active) {
                return &slot;
            }
        }
        return nullptr;
    }

    static HidStreamToken token(const Slot& slot) {
        HidStreamToken value;
        value.connectionHandle = slot.connectionHandle;
        value.connectionGeneration = slot.connectionGeneration;
        value.streamEpoch = slot.streamEpoch;
        value.active = slot.active;
        value.acceptingChunks = slot.acceptingChunks;
        return value;
    }

    std::array<Slot, Capacity> slots_{};
    std::uint32_t nextGeneration_{1};
};

template <std::size_t Capacity, std::size_t MaxPayloadLength>
class IngressBuffer {
    static_assert(Capacity > 0, "IngressBuffer requires non-zero capacity");
    static_assert(MaxPayloadLength <= kIngressPayloadLength,
                  "IngressBuffer payload exceeds IngressMessage capacity");

  public:
    EnqueueResult push(IngressKind kind, std::uint16_t connectionHandle,
                       const std::uint8_t* data, std::size_t length) {
        if (length > MaxPayloadLength || length > kIngressPayloadLength ||
            (length > 0 && data == nullptr)) {
            return EnqueueResult::PayloadTooLarge;
        }
        if (size_ == Capacity) {
            return EnqueueResult::QueueFull;
        }

        IngressMessage& message = messages_[tail_];
        message = {};
        message.kind = kind;
        message.connectionHandle = connectionHandle;
        message.length = static_cast<std::uint16_t>(length);
        if (length > 0 && data != nullptr) {
            std::memcpy(message.payload.data(), data, length);
        }
        tail_ = (tail_ + 1) % Capacity;
        ++size_;
        return EnqueueResult::Accepted;
    }

    bool pop(IngressMessage& message) {
        if (size_ == 0) {
            return false;
        }
        message = messages_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }

    std::size_t size() const { return size_; }

  private:
    std::array<IngressMessage, Capacity> messages_{};
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
};

enum class HidConsumeResult : std::uint8_t {
    Incomplete,
    Complete,
    InvalidPayload,
    PayloadTooLarge,
    ConnectionLimit
};

struct HidRpcView {
    std::uint16_t connectionHandle{0};
    const std::uint8_t* data{nullptr};
    std::size_t length{0};
};

class HidRpcAssembler {
  public:
    HidConsumeResult consume(const HidChunk& chunk, HidRpcView& completed);
    void clear(std::uint16_t connectionHandle);

  private:
    static constexpr std::size_t kConnectionSlots = 4;

    struct Assembly {
        bool active{false};
        bool completed{false};
        std::uint16_t connectionHandle{0};
        std::uint32_t connectionGeneration{0};
        std::uint32_t streamEpoch{0};
        std::size_t length{0};
        std::array<std::uint8_t, kHidRpcAssemblyLength> payload{};
    };

    Assembly* findOrCreate(const HidChunk& chunk);
    std::array<Assembly, kConnectionSlots> assemblies_{};
};

}  // namespace vibe

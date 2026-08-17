#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vibe {

constexpr std::size_t kIngressPayloadLength = 512;
constexpr std::size_t kHidChunkPayloadLength = 61;
constexpr std::size_t kHidRpcAssemblyLength = 2048;

enum class IngressKind : std::uint8_t { Quota, Approval, Disconnect };

struct IngressMessage {
    IngressKind kind{IngressKind::Quota};
    std::uint16_t connectionHandle{0};
    std::uint16_t length{0};
    std::array<std::uint8_t, kIngressPayloadLength> payload{};
};

struct HidChunk {
    std::uint16_t connectionHandle{0};
    std::uint8_t length{0};
    std::array<std::uint8_t, kHidChunkPayloadLength> payload{};
};

enum class EnqueueResult : std::uint8_t {
    Accepted,
    PayloadTooLarge,
    QueueFull,
    Unauthorized
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
        std::size_t length{0};
        std::array<std::uint8_t, kHidRpcAssemblyLength> payload{};
    };

    Assembly* findOrCreate(std::uint16_t connectionHandle);
    std::array<Assembly, kConnectionSlots> assemblies_{};
};

}  // namespace vibe

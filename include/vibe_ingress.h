#pragma once

#include <cstddef>
#include <cstdint>

#include "vibe_ingress_core.h"

namespace vibe {

constexpr char kQuotaServiceUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01";
constexpr char kQuotaWriteUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02";
constexpr char kApprovalWriteUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c03";
constexpr char kApprovalResultUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c04";

bool initializeIngressQueue();
EnqueueResult enqueueGattWrite(IngressKind kind,
                               std::uint16_t connectionHandle,
                               std::uint32_t connectionGeneration,
                               const std::uint8_t* data,
                               std::size_t length,
                               bool authorized = true);
EnqueueResult enqueueHidChunk(std::uint16_t connectionHandle,
                              std::uint32_t connectionGeneration,
                              std::uint32_t streamEpoch,
                              const std::uint8_t* data,
                              std::size_t length);
bool dequeueIngress(IngressMessage& message);
bool dequeueHidChunk(HidChunk& chunk);

}  // namespace vibe

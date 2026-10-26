#pragma once

#include <cstdint>
#include <functional>
#include <type_traits>
#include <vulkan/vulkan.h>

namespace vkfix {
// Vulkan permits non-dispatchable handle values to coincide across devices.
// On 32-bit ABIs those handles are still 64-bit integers; never truncate them
// through uintptr_t, which is only wide enough for dispatchable pointers.
struct DeviceObjectKey {
    VkDevice device{};
    uint64_t handle{};
    template <typename T> static DeviceObjectKey make(VkDevice device, T handle) {
        if constexpr (std::is_pointer_v<T>)
            return {device, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle))};
        else
            return {device, static_cast<uint64_t>(handle)};
    }
    bool operator==(const DeviceObjectKey &other) const {
        return device == other.device && handle == other.handle;
    }
};
struct DeviceObjectKeyHash {
    size_t operator()(const DeviceObjectKey &key) const {
        const auto device = std::hash<VkDevice>{}(key.device);
        const auto handle = std::hash<uint64_t>{}(key.handle);
        return device ^ (handle + 0x9e3779b9u + (device << 6) + (device >> 2));
    }
};
} // namespace vkfix

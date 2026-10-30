#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkfix {
// The legacy API writes tightly packed payloads, whereas the *2 API writes
// payloads inside caller-owned structures. Never overwrite sType/pNext or
// turn a zero-capacity output request into a count-only query through data().
template <class Payload, class Output, class Query, class Copy>
void query_legacy_array(uint32_t *count, Output *output, Query query, Copy copy) {
    if (!count) return;
    if (!output) {
        query(count, nullptr);
        return;
    }
    const uint32_t capacity = *count;
    if (!capacity) return;
    std::vector<Payload> payloads(capacity);
    uint32_t written = capacity;
    query(&written, payloads.data());
    written = std::min(written, capacity);
    for (uint32_t i = 0; i < written; ++i) copy(output[i], payloads[i]);
    *count = written;
}

// A 1.0 implementation can expose singleton device groups. Use the caller's
// capacity for the native enumeration: this preserves VK_INCOMPLETE/errors
// without a count/fill race and includes every physical device, not just GPU 0.
inline VkResult enumerate_singleton_device_groups(
    VkInstance instance, uint32_t *count, VkPhysicalDeviceGroupProperties *groups,
    PFN_vkEnumeratePhysicalDevices enumerate) {
    if (!count || !enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    if (!groups) return enumerate(instance, count, nullptr);
    const uint32_t capacity = *count;
    // A non-null pointer is required even when capacity == 0. A null pointer
    // would request the total count instead of the number actually written.
    std::vector<VkPhysicalDevice> devices(std::max(1u, capacity));
    uint32_t written = capacity;
    const VkResult result = enumerate(instance, &written, devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return result;
    written = std::min(written, capacity);
    for (uint32_t i = 0; i < written; ++i) {
        groups[i].physicalDeviceCount = 1;
        std::fill(std::begin(groups[i].physicalDevices),
                  std::end(groups[i].physicalDevices), VK_NULL_HANDLE);
        groups[i].physicalDevices[0] = devices[i];
        groups[i].subsetAllocation = VK_FALSE;
    }
    *count = written;
    return result;
}
} // namespace vkfix

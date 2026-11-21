#pragma once
#include <vulkan/vulkan.h>

namespace vkfix {
// Ordinary queues have the Vulkan 1.0 retrieval route. Creation flags such
// as PROTECTED are part of the queue identity and must use the native 1.1 path.
inline VkQueue get_created_queue(VkDevice device, const VkDeviceQueueInfo2 &info,
                                 PFN_vkGetDeviceQueue legacy, PFN_vkGetDeviceQueue2 native2) {
    VkQueue queue = VK_NULL_HANDLE;
    if (!info.flags && legacy)
        legacy(device, info.queueFamilyIndex, info.queueIndex, &queue);
    else if (info.flags && native2)
        native2(device, &info, &queue);
    return queue;
}
} // namespace vkfix

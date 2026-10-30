#pragma once

#include "driver_loader.h"
#include "vk_common.h"

namespace vkfix {
// Vulkan guarantees that the 1.1 *2 query returns the same base payload as the
// legacy query when no extension input is chained. A native driver that
// disagrees would otherwise leak a wrong size/alignment into every allocation.
inline bool same_memory_requirements(const VkMemoryRequirements &a,
                                     const VkMemoryRequirements &b) {
    return a.size == b.size && a.alignment == b.alignment &&
           a.memoryTypeBits == b.memoryTypeBits;
}

inline void repair_buffer_memory_requirements(VkDevice device, VkBuffer buffer,
                                              VkMemoryRequirements2 *output) {
    if (!output) return;
    auto legacy = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
        get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements"));
    if (!legacy) return;
    VkMemoryRequirements reference{};
    legacy(device, buffer, &reference);
    if (same_memory_requirements(output->memoryRequirements, reference)) return;
    LOGW("vkGetBufferMemoryRequirements2 mismatch repaired from legacy query (size %llu/%llu, "
         "alignment %llu/%llu)",
         (unsigned long long)output->memoryRequirements.size,
         (unsigned long long)reference.size,
         (unsigned long long)output->memoryRequirements.alignment,
         (unsigned long long)reference.alignment);
    output->memoryRequirements = reference;
}

inline void repair_image_memory_requirements(VkDevice device, VkImage image,
                                             VkMemoryRequirements2 *output) {
    if (!output) return;
    auto legacy = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
        get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements"));
    if (!legacy) return;
    VkMemoryRequirements reference{};
    legacy(device, image, &reference);
    if (same_memory_requirements(output->memoryRequirements, reference)) return;
    LOGW("vkGetImageMemoryRequirements2 mismatch repaired from legacy query (size %llu/%llu, "
         "alignment %llu/%llu)",
         (unsigned long long)output->memoryRequirements.size,
         (unsigned long long)reference.size,
         (unsigned long long)output->memoryRequirements.alignment,
         (unsigned long long)reference.alignment);
    output->memoryRequirements = reference;
}
} // namespace vkfix

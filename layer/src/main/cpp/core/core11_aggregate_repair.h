#pragma once

#include <vulkan/vulkan.h>

namespace vkfix {
// On a native Vulkan 1.1+ driver both the aggregate structs and their
// promoted children are valid. Rebuild the aggregate from the children so a
// driver that reports inconsistent values cannot leak that inconsistency to
// the application. No-op when the aggregate is absent or the driver is 1.0.
void repair_vulkan11_features(VkPhysicalDevice physical,
                              VkPhysicalDeviceFeatures2 *features);
void repair_vulkan11_properties(VkPhysicalDevice physical,
                                VkPhysicalDeviceProperties2 *properties);
} // namespace vkfix

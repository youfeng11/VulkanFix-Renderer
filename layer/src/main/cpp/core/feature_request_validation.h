#pragma once
#include <vulkan/vulkan.h>

namespace vkfix {
// Validate the original public request against public query results, before
// emulation modules can strip or rewrite an unsupported feature request.
VkResult validate_device_features(VkPhysicalDevice, const VkDeviceCreateInfo&,
                                 PFN_vkGetPhysicalDeviceFeatures2 query);
}

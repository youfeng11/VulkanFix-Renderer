#pragma once
#include <vulkan/vulkan.h>

namespace vkfix {
inline VkImageCopy array_to_volume_slice(const VkImageCopy &region, uint32_t slice) {
    auto result = region;
    result.srcSubresource.baseArrayLayer += slice;
    result.srcSubresource.layerCount = 1;
    result.dstOffset.z += static_cast<int32_t>(slice);
    result.extent.depth = 1;
    return result;
}
} // namespace vkfix

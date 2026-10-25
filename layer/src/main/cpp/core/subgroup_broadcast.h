#pragma once
#include "vk_common.h"
namespace vkfix {
struct SubgroupBroadcastSupport {
    uint32_t max_size = 0;
    bool shuffle = false;
    explicit operator bool() const {
        return max_size != 0;
    }
};
// Queries raw native properties, never the layer's promoted API version.
SubgroupBroadcastSupport subgroup_broadcast_support(VkPhysicalDevice physical);
} // namespace vkfix

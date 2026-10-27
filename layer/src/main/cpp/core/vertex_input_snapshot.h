#pragma once
#include "shader/vertex_store_lowering.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace vkfix {
// One snapshot per draw: different draws must observe intervening GPU writes.
class VertexInputSnapshot {
public:
    struct Copy {
        VkBuffer source;
        VkDeviceSize size, destination;
    };
    explicit VertexInputSnapshot(VkDeviceSize max_range)
        : limit(std::min<VkDeviceSize>(max_range, UINT32_MAX)) {}

    bool append(VkBuffer buffer, VkDeviceSize buffer_size, VkDeviceSize offset, uint32_t& address) {
        if (!buffer || offset >= buffer_size || size > limit) return false;
        for (uint32_t i = 0; i < copy_count; ++i) {
            if (copies[i].source == buffer) {
                if (copies[i].size != buffer_size) return false;
                address = uint32_t(copies[i].destination + offset);
                return true;
            }
        }
        // Check before addition/alignment; shader byte addresses are 32-bit.
        if (copy_count == copies.size() || buffer_size > limit - size) return false;
        const VkDeviceSize aligned_size = (buffer_size + 3) & ~VkDeviceSize(3);
        if (aligned_size > limit - size) return false;
        address = uint32_t(size + offset);
        copies[copy_count++] = {buffer, buffer_size, size};
        size += aligned_size;
        return true;
    }

    std::array<Copy, kVertexStoreBindings + 1> copies{};
    uint32_t copy_count = 0;
    VkDeviceSize size = kVertexStoreHeaderWords * sizeof(uint32_t);
private:
    VkDeviceSize limit;
};
}

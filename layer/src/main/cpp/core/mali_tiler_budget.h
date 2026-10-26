#pragma once
#include <cstdint>
#include <limits>

namespace vkfix {
// Indirect vertex counts are GPU data. Bound whole API commands instead of
// splitting multi-draw commands, which would change the shader's DrawIndex.
struct MaliTilerBudget {
    uint64_t vertices = 0;
    uint64_t indirect_draws = 0;
    uint32_t indirect_commands = 0;
    bool has_work = false;
    bool needs_split(uint64_t vertex_count, uint32_t draw_count) const {
        if (!has_work || (!vertex_count && !draw_count))
            return false;
        return vertices >= 262144 || indirect_commands >= 4 || indirect_draws >= 128 ||
               vertex_count > 262144 - vertices || draw_count > 128 - indirect_draws;
    }
    void add(uint64_t vertex_count, uint32_t draw_count) {
        if (!vertex_count && !draw_count)
            return;
        has_work = true;
        vertices = vertex_count > std::numeric_limits<uint64_t>::max() - vertices
                       ? std::numeric_limits<uint64_t>::max()
                       : vertices + vertex_count;
        indirect_draws = draw_count > std::numeric_limits<uint64_t>::max() - indirect_draws
                             ? std::numeric_limits<uint64_t>::max()
                             : indirect_draws + draw_count;
        if (draw_count && indirect_commands != std::numeric_limits<uint32_t>::max())
            ++indirect_commands;
    }
};
} // namespace vkfix

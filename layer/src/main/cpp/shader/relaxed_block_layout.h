#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace vkfix {
enum class BlockLayoutLowering { Unchanged, Changed, Invalid };
// Keep descriptor bindings, byte offsets, array/matrix strides and value types.
// Buffer-structure vector members become scalar members in the memory types.
BlockLayoutLowering lower_relaxed_block_layout(std::vector<uint32_t>&, std::string& error);
bool has_block_vectors(const uint32_t*, size_t words);
}

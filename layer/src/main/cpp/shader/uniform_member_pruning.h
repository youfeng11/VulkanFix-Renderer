#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkfix {
enum class UniformPruneResult { Unchanged, Changed, Invalid };

// Cheap preflight for direct Uniform Block structs with more than 256 members.
// The observed Iris frame block has 305 members; this is a conservative trigger,
// not an advertised hardware limit. Storage buffers and push constants do not
// trigger this workaround.
bool has_large_uniform_block(const uint32_t* words, size_t word_count);

// Preserve byte offsets, bindings, matrix/array strides and shader calculations.
// On failure, leave code unchanged and let the existing native path handle it.
UniformPruneResult prune_unused_uniform_members(std::vector<uint32_t>& code,
                                                uint32_t max_spirv_version,
                                                std::string& error);
} // namespace vkfix

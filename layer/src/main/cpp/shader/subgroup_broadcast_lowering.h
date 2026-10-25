#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace vkfix {
enum class BroadcastLowering { Unchanged, Changed, Invalid };
// Keeps module version and entry-point interfaces intact. Run the normal
// version downgrade afterwards. On failure the input is unchanged.
BroadcastLowering lower_subgroup_broadcasts(std::vector<uint32_t> &code, uint32_t max_subgroup_size,
                                            bool native_shuffle, std::string &error);
} // namespace vkfix

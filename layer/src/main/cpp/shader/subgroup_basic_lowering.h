#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace vkfix {
enum class BasicSubgroupLowering { Unchanged, Changed, Invalid };
bool has_basic_subgroups(const uint32_t*,size_t word_count);
// Implement one invocation per logical subgroup, without a workgroup-wide
// execution barrier. Only the compute stage is advertised by this fallback.
BasicSubgroupLowering lower_basic_subgroups(std::vector<uint32_t>&,std::string& error);
}

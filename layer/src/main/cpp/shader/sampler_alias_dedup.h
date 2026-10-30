#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkfix {
enum class SamplerAliasResult { Unchanged, Changed, Invalid };

// Selection belongs to core/driver_trust; this file only rewrites shaders.
bool has_sampler_aliases(const uint32_t* words, size_t word_count);
SamplerAliasResult dedup_sampler_aliases(std::vector<uint32_t>& code,
                                        uint32_t max_spirv_version, std::string& error);
} // namespace vkfix

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkfix {
enum class SamplerAliasResult { Unchanged, Changed, Invalid };

// Match native properties, before VulkanFix's feature/version promotion.
bool sampler_alias_dedup_enabled(uint32_t vendor, uint32_t device, uint32_t driver,
                                 const char* override_value = nullptr);
bool has_sampler_aliases(const uint32_t* words, size_t word_count);
SamplerAliasResult dedup_sampler_aliases(std::vector<uint32_t>& code,
                                        uint32_t max_spirv_version, std::string& error);
} // namespace vkfix

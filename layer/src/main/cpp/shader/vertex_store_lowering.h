#pragma once
#include "core/vk_common.h"
#include <string>
#include <vector>

namespace vkfix {
// The private input buffer begins with eight draw parameters followed by
// {byteOffset, stride} pairs for each vertex binding. Raw input data follows.
constexpr uint32_t kVertexStoreBindings = 16;
constexpr uint32_t kVertexStoreHeaderWords = 8 + 2 * kVertexStoreBindings;
struct VertexStoreAttribute {
    uint32_t location, binding, offset;
    VkFormat format;
    VkVertexInputRate rate;
};
struct VertexStoreProgram {
    std::vector<uint32_t> compute;
    std::vector<uint32_t> replay;
    uint32_t output_words = 0;
    std::vector<uint32_t> resource_sets;
    std::string error;
};
// Lowers the complete vertex invocation, including image/SSBO side effects and
// atomic return values, to compute. The replay shader only reads saved outputs.
// Unsupported interfaces are rejected before either program reaches a driver.
bool lower_vertex_stores(const std::vector<uint32_t> &source, const char *entry,
                         uint32_t private_set, const std::vector<VertexStoreAttribute> &attributes,
                         VertexStoreProgram &result);
bool vertex_has_storage_effects(const std::vector<uint32_t> &source);
} // namespace vkfix

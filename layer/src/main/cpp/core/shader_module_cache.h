#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vkfix {
// Process-local copy of shader modules for internal rewrites. The layer never
// assumes ownership of caller memory and never rewrites the application's
// module in place.
class ShaderModuleCache {
public:
    static ShaderModuleCache &get();
    void remember(VkDevice, VkShaderModule, const VkShaderModuleCreateInfo *,
                  bool specialize_at_pipeline = false, bool select_view_at_pipeline = false);
    void forget(VkDevice, VkShaderModule);
    void forget_device(VkDevice);
    bool copy(VkDevice, VkShaderModule, std::vector<uint32_t> &) const;
    std::shared_ptr<const std::vector<uint32_t>> acquire(VkDevice, VkShaderModule) const;
    std::shared_ptr<const std::vector<uint32_t>> acquire_deferred(VkDevice, VkShaderModule) const;
    std::shared_ptr<const std::vector<uint32_t>> acquire_view_deferred(VkDevice, VkShaderModule) const;

private:
    struct Key {
        VkDevice device;
        VkShaderModule module;
        bool operator==(const Key &other) const {
            return device == other.device && module == other.module;
        }
    };
    struct Hash {
        size_t operator()(const Key &key) const {
            size_t h = std::hash<VkDevice>()(key.device);
            h ^= std::hash<VkShaderModule>()(key.module) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    mutable std::mutex mutex_;
    // Live Vulkan objects, rather than an arbitrary shader-count threshold,
    // determine lifetime. Pipelines can keep a snapshot after module deletion.
    struct Module {
        std::shared_ptr<const std::vector<uint32_t>> words;
        bool specialize_at_pipeline;
        bool select_view_at_pipeline;
    };
    std::unordered_map<Key, Module, Hash> modules_;
};
} // namespace vkfix

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vkfix {
// Process-local copy of shader modules for internal rewrites. The layer never
// assumes ownership of caller memory and never rewrites the application's
// module in place.
class ShaderModuleCache {
public:
    static ShaderModuleCache &get();
    void remember(VkDevice, VkShaderModule, const VkShaderModuleCreateInfo *);
    void forget(VkDevice, VkShaderModule);
    void forget_device(VkDevice);
    bool copy(VkDevice, VkShaderModule, std::vector<uint32_t> &) const;

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
            size_t h = std::hash<uintptr_t>()(reinterpret_cast<uintptr_t>(key.device));
            h ^= std::hash<uintptr_t>()(reinterpret_cast<uintptr_t>(key.module)) +
                 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    mutable std::mutex mutex_;
    std::unordered_map<Key, std::vector<uint32_t>, Hash> modules_;
    static constexpr size_t kMaxModules = 4096;
};
} // namespace vkfix

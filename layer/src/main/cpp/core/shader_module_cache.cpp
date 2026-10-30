#include "shader_module_cache.h"

namespace vkfix {
ShaderModuleCache &ShaderModuleCache::get() {
    static ShaderModuleCache instance;
    return instance;
}

void ShaderModuleCache::remember(VkDevice device, VkShaderModule module,
                                 const VkShaderModuleCreateInfo *ci) {
    if (!device || !module || !ci || !ci->pCode || ci->codeSize < 5 * sizeof(uint32_t) ||
        ci->codeSize % sizeof(uint32_t) != 0)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (modules_.size() >= kMaxModules && modules_.count({device, module}) == 0)
        modules_.clear();
    modules_[{device, module}].assign(ci->pCode, ci->pCode + ci->codeSize / sizeof(uint32_t));
}

void ShaderModuleCache::forget(VkDevice device, VkShaderModule module) {
    std::lock_guard<std::mutex> lock(mutex_);
    modules_.erase({device, module});
}

void ShaderModuleCache::forget_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = modules_.begin(); it != modules_.end();) {
        if (it->first.device == device)
            it = modules_.erase(it);
        else
            ++it;
    }
}

bool ShaderModuleCache::copy(VkDevice device, VkShaderModule module,
                             std::vector<uint32_t> &output) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find({device, module});
    if (it == modules_.end()) return false;
    output = it->second;
    return true;
}
} // namespace vkfix

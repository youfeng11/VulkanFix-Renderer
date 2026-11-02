#include "shader_module_cache.h"

namespace vkfix {
ShaderModuleCache &ShaderModuleCache::get() {
    static ShaderModuleCache instance;
    return instance;
}

void ShaderModuleCache::remember(VkDevice device, VkShaderModule module,
                                 const VkShaderModuleCreateInfo *ci, bool specialize_at_pipeline,
                                 bool select_view_at_pipeline) {
    if (!device || !module || !ci || !ci->pCode || ci->codeSize < 5 * sizeof(uint32_t) ||
        ci->codeSize % sizeof(uint32_t) != 0)
        return;
    auto words = std::make_shared<const std::vector<uint32_t>>(
        ci->pCode, ci->pCode + ci->codeSize / sizeof(uint32_t));
    std::lock_guard<std::mutex> lock(mutex_);
    modules_[{device, module}] = {std::move(words), specialize_at_pipeline, select_view_at_pipeline};
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
    auto words = acquire(device, module);
    if (!words) return false;
    output = *words;
    return true;
}
std::shared_ptr<const std::vector<uint32_t>> ShaderModuleCache::acquire(VkDevice device, VkShaderModule module) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find({device, module});
    return it == modules_.end() ? nullptr : it->second.words;
}
std::shared_ptr<const std::vector<uint32_t>> ShaderModuleCache::acquire_deferred(VkDevice device, VkShaderModule module) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it=modules_.find({device,module});
    return it==modules_.end() || (!it->second.specialize_at_pipeline && !it->second.select_view_at_pipeline)
        ? nullptr : it->second.words;
}
std::shared_ptr<const std::vector<uint32_t>> ShaderModuleCache::acquire_view_deferred(VkDevice device, VkShaderModule module) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find({device, module});
    return it == modules_.end() || !it->second.select_view_at_pipeline ? nullptr : it->second.words;
}
} // namespace vkfix

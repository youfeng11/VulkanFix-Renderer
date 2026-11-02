#include "specialized_shader_stages.h"
#include "shader_module_cache.h"
#include "created_shader_module.h"
#include "shader/specialize_constants.h"
#include "shader/constant_input_lowering.h"
#include "vk_common.h"

namespace vkfix {
SpecializedShaderStages::~SpecializedShaderStages() {
    for (auto module:modules_) if (module) destroy_(device_,module,nullptr);
}
std::shared_ptr<const std::vector<uint32_t>> SpecializedShaderStages::source(VkShaderModule module, std::optional<uint32_t> view) const {
    auto words = ShaderModuleCache::get().acquire_deferred(device_, module);
    if (words || !view) return words;
    words = ShaderModuleCache::get().acquire(device_, module);
    return words && has_view_index(words->data(), words->size()) ? words : nullptr;
}
VkResult SpecializedShaderStages::stage(VkPipelineShaderStageCreateInfo& stage, std::optional<uint32_t> view, bool from_device) {
    auto words = source(stage.module, view);
    if (!words) return VK_SUCCESS;
    const bool select_view = view.has_value() ||
        bool(ShaderModuleCache::get().acquire_view_deferred(device_, stage.module));
    auto code=*words;
    std::string error;
    if (!specialize_constants(code,stage.pSpecializationInfo,error)) {
        LOGE("Pipeline shader specialization failed for %s: %s",stage.pName?stage.pName:"<unnamed>",error.c_str());
        return VK_ERROR_UNKNOWN;
    }
    const auto lowered = !select_view ? ConstantInputLowering::Unchanged :
        from_device && native_device_group_ ? lower_view_index_from_device(code, error) :
        lower_view_index(code, view.value_or(0), error);
    if (lowered == ConstantInputLowering::Invalid) {
        LOGE("Pipeline ViewIndex selection failed for %s: %s", stage.pName ? stage.pName : "<unnamed>", error.c_str());
        return VK_ERROR_UNKNOWN;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize=code.size()*sizeof(uint32_t); ci.pCode=code.data();
    CreatedShaderModule module(device_,destroy_);
    auto result=module.create(create_,&ci);
    if (result!=VK_SUCCESS) return result;
    modules_.push_back(module.get());
    stage.module=module.release();
    stage.pSpecializationInfo=nullptr;
    return VK_SUCCESS;
}
VkResult SpecializedShaderStages::prepare(uint32_t count,const VkGraphicsPipelineCreateInfo* infos) {
    if (!infos) return VK_SUCCESS;
    for (uint32_t i=0;i<count;++i) {
        const bool from_device = (infos[i].flags & VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT) != 0;
        const auto view = from_device && !native_device_group_ ? std::optional<uint32_t>(0) : view_;
        if (from_device && (!native_device_group_ || view_)) {
            if (graphics_.empty()) { graphics_.assign(infos,infos+count); stages_.resize(count); }
            graphics_[i].flags &= ~VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT;
        }
        for (uint32_t s=0;s<infos[i].stageCount;++s) {
            if (!source(infos[i].pStages[s].module, view)) continue;
            if (graphics_.empty()) { graphics_.assign(infos,infos+count); stages_.resize(count); }
            if (stages_[i].empty()) {
                stages_[i].assign(infos[i].pStages,infos[i].pStages+infos[i].stageCount);
                graphics_[i].pStages=stages_[i].data();
            }
            auto result=stage(stages_[i][s], view, from_device);
            if (result!=VK_SUCCESS) return result;
        }
    }
    return VK_SUCCESS;
}
VkResult SpecializedShaderStages::prepare(uint32_t count,const VkComputePipelineCreateInfo* infos) {
    if (!infos) return VK_SUCCESS;
    for (uint32_t i=0;i<count;++i) {
        if (!source(infos[i].stage.module, view_)) continue;
        if (compute_.empty()) compute_.assign(infos,infos+count);
        auto result=stage(compute_[i].stage, view_);
        if (result!=VK_SUCCESS) return result;
    }
    return VK_SUCCESS;
}
}

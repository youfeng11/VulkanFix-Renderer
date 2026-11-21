#include "descriptor_layout_query.h"
#include "vk_pnext.h"
#include <new>

namespace vkfix {
VkResult DescriptorLayoutQuery::prepare(const VkDescriptorSetLayoutCreateInfo& info, bool vertex_compute) {
    info_ = info;
    bindings_.clear();
    const auto flags = vku::find_pnext<VkDescriptorSetLayoutBindingFlagsCreateInfo>(
        info.pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    bool changed = false;
    for (uint32_t i = 0; i < info.bindingCount; ++i) {
        const auto& binding = info.pBindings[i];
        const bool variable = flags && i < flags->bindingCount && flags->pBindingFlags &&
            (flags->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT);
        changed |= (variable && binding.descriptorCount == 0) ||
            (vertex_compute && (binding.stageFlags & VK_SHADER_STAGE_VERTEX_BIT));
    }
    if (!changed) return VK_SUCCESS;
    try {
        bindings_.assign(info.pBindings, info.pBindings + info.bindingCount);
        for (uint32_t i = 0; i < info.bindingCount; ++i) {
            auto& binding = bindings_[i];
            if (vertex_compute && (binding.stageFlags & VK_SHADER_STAGE_VERTEX_BIT))
                binding.stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT;
            if (binding.descriptorCount == 0 && flags && i < flags->bindingCount && flags->pBindingFlags &&
                (flags->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT))
                binding.descriptorCount = binding.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? 4 : 1;
        }
        info_.pBindings = bindings_.data();
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
}

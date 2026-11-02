#pragma once
#include <vulkan/vulkan.h>
#include <memory>
#include <optional>
#include <vector>

namespace vkfix {
// Private modules live through all pipeline modifiers and their post callbacks.
// Callbacks use the layer entry points so downstream emulators see these bytes.
class SpecializedShaderStages {
public:
    SpecializedShaderStages(VkDevice device, PFN_vkCreateShaderModule create, PFN_vkDestroyShaderModule destroy,
                            std::optional<uint32_t> view = std::nullopt, bool native_device_group = false)
        : device_(device), create_(create), destroy_(destroy), view_(view), native_device_group_(native_device_group) {}
    ~SpecializedShaderStages();
    SpecializedShaderStages(const SpecializedShaderStages&)=delete;
    SpecializedShaderStages& operator=(const SpecializedShaderStages&)=delete;
    VkResult prepare(uint32_t, const VkGraphicsPipelineCreateInfo*);
    VkResult prepare(uint32_t, const VkComputePipelineCreateInfo*);
    const VkGraphicsPipelineCreateInfo* graphics(const VkGraphicsPipelineCreateInfo* original) const {
        return graphics_.empty()?original:graphics_.data();
    }
    const VkComputePipelineCreateInfo* compute(const VkComputePipelineCreateInfo* original) const {
        return compute_.empty()?original:compute_.data();
    }
private:
    std::shared_ptr<const std::vector<uint32_t>> source(VkShaderModule, std::optional<uint32_t>) const;
    VkResult stage(VkPipelineShaderStageCreateInfo&, std::optional<uint32_t>, bool from_device = false);
    VkDevice device_;
    PFN_vkCreateShaderModule create_;
    PFN_vkDestroyShaderModule destroy_;
    std::optional<uint32_t> view_;
    bool native_device_group_;
    std::vector<VkShaderModule> modules_;
    std::vector<VkGraphicsPipelineCreateInfo> graphics_;
    std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages_;
    std::vector<VkComputePipelineCreateInfo> compute_;
};
}

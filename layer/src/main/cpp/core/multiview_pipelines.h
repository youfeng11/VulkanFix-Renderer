#pragma once
#include "multiview_render_pass.h"
#include <array>
#include <atomic>

namespace vkfix {
// Every variant is compiled before the application's creation call returns.
// No application CI pointers, shader handles or specialization pointers are
// borrowed for later compilation.
class MultiviewPipelineVariants {
public:
    MultiviewPipelineVariants(VkDevice device, std::shared_ptr<MultiviewPassResources> pass,
        uint32_t subpass, PFN_vkDestroyPipeline destroy, const VkAllocationCallbacks* allocator);
    ~MultiviewPipelineVariants();
    VkPipeline for_view(uint32_t view) const;
    const std::shared_ptr<MultiviewPassResources>& pass() const { return pass_; }
    uint32_t subpass() const { return subpass_; }
    uint32_t view_mask() const { return view_mask_; }
    void release();
private:
    friend class MultiviewPipelines;
    mutable std::mutex mutex_;
    VkDevice device_;
    std::shared_ptr<MultiviewPassResources> pass_;
    uint32_t subpass_, view_mask_;
    PFN_vkDestroyPipeline destroy_;
    std::optional<VkAllocationCallbacks> allocator_;
    std::array<VkPipeline, 32> pipelines_{};
    VkPipeline representative_ = VK_NULL_HANDLE;
    uint32_t representative_view_ = 32;
    bool published_ = false;
};

class MultiviewPipelines {
public:
    static MultiviewPipelines& get();
    // Returns false when the complete batch can use the ordinary path.
    bool create(VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
        VkPipeline* pipelines, VkResult& result, PFN_vkCreateGraphicsPipelines create,
        PFN_vkDestroyPipeline destroy, PFN_vkCreateShaderModule create_shader,
        PFN_vkDestroyShaderModule destroy_shader, uint32_t routes);
    std::shared_ptr<MultiviewPipelineVariants> acquire(VkDevice device, VkPipeline pipeline);
    void forget(VkDevice device, VkPipeline pipeline);
    void forget_device(VkDevice device);
private:
    VkResult create_one(VkDevice device, VkPipelineCache cache,
        const VkGraphicsPipelineCreateInfo& info, const VkAllocationCallbacks* allocator,
        VkPipeline& pipeline, PFN_vkCreateGraphicsPipelines create, PFN_vkDestroyPipeline destroy,
        PFN_vkCreateShaderModule create_shader, PFN_vkDestroyShaderModule destroy_shader, uint32_t routes);
    void publish(VkDevice device, const std::shared_ptr<MultiviewPipelineVariants>& variants);
    std::mutex mutex_;
    std::unordered_map<DeviceObjectKey, std::shared_ptr<MultiviewPipelineVariants>, DeviceObjectKeyHash> pipelines_;
    std::map<VkDevice, std::vector<std::weak_ptr<MultiviewPipelineVariants>>> live_;
    std::atomic<uint32_t> count_{0};
};
}

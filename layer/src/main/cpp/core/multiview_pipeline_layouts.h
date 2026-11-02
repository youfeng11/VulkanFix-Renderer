#pragma once
#include "multiview_render_pass.h"

namespace vkfix {
class MultiviewPipelineLayout {
public:
    MultiviewPipelineLayout(VkDevice, PFN_vkDestroyPipelineLayout);
    ~MultiviewPipelineLayout();
    VkPipelineLayout native();
    void release();
private:
    friend class MultiviewPipelineLayouts;
    std::mutex mutex_;
    VkDevice device_;
    VkPipelineLayout native_{};
    PFN_vkDestroyPipelineLayout destroy_;
};
class MultiviewPipelineLayouts {
public:
    static MultiviewPipelineLayouts& get();
    VkResult remember(VkDevice, VkPipelineLayout, const VkPipelineLayoutCreateInfo&,
        PFN_vkCreatePipelineLayout create = nullptr, PFN_vkDestroyPipelineLayout destroy = nullptr);
    std::shared_ptr<MultiviewPipelineLayout> acquire(VkDevice, VkPipelineLayout);
    void forget(VkDevice, VkPipelineLayout);
    void forget_device(VkDevice);
private:
    std::mutex mutex_;
    std::unordered_map<DeviceObjectKey, std::shared_ptr<MultiviewPipelineLayout>, DeviceObjectKeyHash> layouts_;
    std::map<VkDevice, std::vector<std::weak_ptr<MultiviewPipelineLayout>>> live_;
};
}

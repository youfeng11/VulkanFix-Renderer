#pragma once
#include "device_object_key.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace vkfix {
class DiscardStateRepair {
public:
    static DiscardStateRepair &get();
    void configure_device(VkInstance, VkPhysicalDevice, PFN_vkGetInstanceProcAddr, VkDevice, const VkDeviceCreateInfo &);
    void enable(VkDevice, uint32_t max_viewports);
    bool enabled(VkDevice) const;
    void forget_device(VkDevice);
    void remember_pipelines(VkDevice, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkPipeline *);
    void forget_pipeline(VkDevice, VkPipeline);
    void viewport(VkDevice, VkCommandBuffer, uint32_t first, uint32_t count, const VkViewport *);
    void scissor(VkDevice, VkCommandBuffer, uint32_t first, uint32_t count, const VkRect2D *);
    void forget_command(VkCommandBuffer);
    void invalidate_viewport(VkCommandBuffer);
    void invalidate_scissor(VkCommandBuffer);
    // Called after the native graphics bind. Extra state setters are emitted
    // only on a measured device after leaving a static discard pipeline.
    void bound(VkDevice, VkCommandBuffer, VkPipelineBindPoint, VkPipeline,
               PFN_vkCmdSetViewport, PFN_vkCmdSetScissor);
private:
    struct Pipeline {
        bool discard = false;
        bool viewport_dynamic = false, scissor_dynamic = false;
        uint32_t viewports = 0, scissors = 0;
    };
    struct Command {
        VkDevice device{};
        bool restore = false;
        std::vector<std::optional<VkViewport>> viewports;
        std::vector<std::optional<VkRect2D>> scissors;
    };
    std::atomic<bool> any_enabled_{false};
    mutable std::mutex mutex_;
    std::unordered_map<VkDevice, uint32_t> devices_;
    std::unordered_map<DeviceObjectKey, Pipeline, DeviceObjectKeyHash> pipelines_;
    std::unordered_map<VkCommandBuffer, Command> commands_;
};
} // namespace vkfix

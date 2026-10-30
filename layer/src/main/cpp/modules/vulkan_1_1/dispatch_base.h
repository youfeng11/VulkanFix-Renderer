#pragma once

#include "layer_module.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class DispatchBaseModule : public IVulkanLayerModule {
public:
    DispatchBaseModule() = default;
    const char* get_name() const override { return "VK_FEATURE_dispatchBase"; }
    bool is_enabled() const override { return true; }

    void on_post_create_device(VkPhysicalDevice physicalDevice, VkDevice device,
                               VkResult result, void* pUserData) override;
    void on_destroy_device(VkDevice device) override;
    void on_post_create_compute_pipelines(
        VkDevice device, uint32_t count,
        const VkComputePipelineCreateInfo* pCreateInfos,
        const VkPipeline* pPipelines) override;
    void on_destroy_pipeline(VkDevice device, VkPipeline pipeline) override;
    void on_begin_command_buffer(VkCommandBuffer commandBuffer,
                                 const VkCommandBufferBeginInfo*) override;
    void on_reset_command_buffer(VkCommandBuffer commandBuffer,
                                 VkCommandBufferResetFlags) override;
    void on_cmd_bind_pipeline(VkCommandBuffer commandBuffer,
                              VkPipelineBindPoint bindPoint, VkPipeline pipeline) override;
    bool on_cmd_dispatch_base(VkCommandBuffer commandBuffer, uint32_t baseGroupX,
                              uint32_t baseGroupY, uint32_t baseGroupZ,
                              uint32_t groupCountX, uint32_t groupCountY,
                              uint32_t groupCountZ) override;

private:
    struct ComputeInfo {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipelineCreateFlags flags = 0;
        VkShaderStageFlags stage = VK_SHADER_STAGE_COMPUTE_BIT;
        VkShaderModule module = VK_NULL_HANDLE;
        std::string entry = "main";
        std::vector<VkSpecializationMapEntry> entries;
        std::vector<uint8_t> data;
    };
    bool is_emulating(VkDevice device) const;
    bool native_support(VkPhysicalDevice physicalDevice) const;
    VkPipeline variant_for(VkDevice device, VkPipeline pipeline,
                           const uint32_t base[3], bool* no_offset);

    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_emulate;
    std::unordered_map<uint64_t, ComputeInfo> m_infos;
    std::unordered_map<uint64_t, VkPipeline> m_variants;
    std::unordered_map<uint64_t, uint64_t> m_variant_owner;
    std::unordered_map<uint64_t, VkDevice> m_variant_device;
    std::unordered_map<uint64_t, VkPipeline> m_cmd_pipeline;
};

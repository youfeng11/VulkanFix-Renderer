#pragma once

#include "layer_module.h"
#include <mutex>
#include <set>
#include <unordered_map>

class NegativeViewportModule : public IVulkanLayerModule {
public:
    NegativeViewportModule() = default;
    const char* get_name() const override { return "VK_FEATURE_negativeViewport"; }
    bool is_enabled() const override { return true; }
    bool is_device_emulating(VkDevice device) const;

    void on_post_create_device(VkPhysicalDevice physicalDevice, VkDevice device,
                               VkResult result, void* pUserData) override;
    void on_destroy_device(VkDevice device) override;

    bool needs_pipeline_interception(
        VkDevice device, uint32_t createInfoCount,
        const VkGraphicsPipelineCreateInfo* pCreateInfos) override;
    void on_modify_pipeline_create_info(
        VkDevice device, uint32_t index,
        VkGraphicsPipelineCreateInfo& createInfo,
        VkPipelineVertexInputStateCreateInfo& viState,
        std::vector<void*>& allocationsToFree) override;
    void on_post_create_graphics_pipelines(
        VkDevice device, uint32_t count,
        const VkGraphicsPipelineCreateInfo* pCreateInfos,
        const VkPipeline* pPipelines) override;
    void on_destroy_pipeline(VkDevice device, VkPipeline pipeline) override;
    void on_begin_command_buffer(VkCommandBuffer commandBuffer,
                                 const VkCommandBufferBeginInfo*) override;
    void on_reset_command_buffer(VkCommandBuffer commandBuffer,
                                 VkCommandBufferResetFlags) override;
    void on_cmd_bind_pipeline(VkCommandBuffer commandBuffer,
                              VkPipelineBindPoint bindPoint, VkPipeline pipeline) override;
    bool on_cmd_set_viewport(VkCommandBuffer commandBuffer, uint32_t firstViewport,
                             uint32_t count, const VkViewport* pViewports) override;
    bool on_cmd_draw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
                     uint32_t instanceCount, uint32_t firstVertex,
                     uint32_t firstInstance) override;
    bool on_cmd_draw_indexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
                             uint32_t instanceCount, uint32_t firstIndex,
                             int32_t vertexOffset, uint32_t firstInstance) override;
    bool on_cmd_draw_indirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                              VkDeviceSize offset, uint32_t drawCount,
                              uint32_t stride) override;
    bool on_cmd_draw_indexed_indirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                      VkDeviceSize offset, uint32_t drawCount,
                                      uint32_t stride) override;
    bool on_cmd_draw_indirect_count(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                    VkDeviceSize offset, VkBuffer countBuffer,
                                    VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                    uint32_t stride) override;
    bool on_cmd_draw_indexed_indirect_count(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                            VkDeviceSize offset, VkBuffer countBuffer,
                                            VkDeviceSize countBufferOffset,
                                            uint32_t maxDrawCount, uint32_t stride) override;

private:
    struct Pending {
        VkDevice device = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        bool dynamic_variant = false;
        bool uses_viewport_index = false;
        uint32_t constant_viewport = UINT32_MAX;
    };
    bool native_support(VkPhysicalDevice physicalDevice) const;
    bool has_negative_static_viewport(const VkGraphicsPipelineCreateInfo&) const;
    bool has_dynamic_viewport(const VkGraphicsPipelineCreateInfo&) const;
    bool bind_variant(VkCommandBuffer, VkDevice&, VkPipeline&, VkPipeline&);

    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_emulate;
    std::unordered_map<const VkGraphicsPipelineCreateInfo*, Pending> m_pending;
    std::unordered_map<uint64_t, VkPipeline> m_variants;
    std::unordered_map<uint64_t, bool> m_pipeline_uses_viewport_index;
    std::unordered_map<uint64_t, uint32_t> m_pipeline_constant_viewport;
    std::unordered_map<uint64_t, VkPipeline> m_cmd_pipeline;
    std::unordered_map<uint64_t, std::set<uint32_t>> m_cmd_negative;
    std::unordered_map<uint64_t, std::set<uint32_t>> m_cmd_positive;
};

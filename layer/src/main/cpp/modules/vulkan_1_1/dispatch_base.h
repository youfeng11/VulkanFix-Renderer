#pragma once

#include "layer_module.h"
#include "device_object_key.h"
#include <array>
#include <memory>
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
    void on_modify_compute_pipeline_create_info(VkDevice, VkComputePipelineCreateInfo&) override;
    void on_post_create_pipeline_layout(VkDevice, const VkPipelineLayoutCreateInfo*, VkResult, VkPipelineLayout) override;
    void on_destroy_pipeline_layout(VkDevice, VkPipelineLayout) override;
    void on_post_create_compute_pipelines(
        VkDevice device, uint32_t count,
        const VkComputePipelineCreateInfo* pCreateInfos,
        const VkPipeline* pPipelines) override;
    void on_destroy_pipeline(VkDevice device, VkPipeline pipeline) override;
    void on_begin_command_buffer(VkCommandBuffer commandBuffer,
                                 const VkCommandBufferBeginInfo*) override;
    void on_reset_command_buffer(VkCommandBuffer commandBuffer,
                                 VkCommandBufferResetFlags) override;
    void on_free_command_buffers(VkDevice, uint32_t, const VkCommandBuffer*) override;
    VkResult command_recording_error(VkCommandBuffer) override;
    void on_cmd_bind_pipeline(VkCommandBuffer commandBuffer,
                              VkPipelineBindPoint bindPoint, VkPipeline pipeline) override;
    bool on_cmd_dispatch_base(VkCommandBuffer commandBuffer, uint32_t baseGroupX,
                              uint32_t baseGroupY, uint32_t baseGroupZ,
                              uint32_t groupCountX, uint32_t groupCountY,
                              uint32_t groupCountZ) override;

private:
    using Key = vkfix::DeviceObjectKey;
    using KeyHash = vkfix::DeviceObjectKeyHash;
    struct VariantKey {
        Key pipeline;
        std::array<uint32_t, 3> base;
        bool operator==(const VariantKey& other) const { return pipeline == other.pipeline && base == other.base; }
    };
    struct VariantHash {
        size_t operator()(const VariantKey& value) const {
            size_t hash = KeyHash{}(value.pipeline);
            for (auto component : value.base) hash ^= std::hash<uint32_t>{}(component) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    struct RetainedLayout {
        VkDevice device{};
        VkPipelineLayout handle{};
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        ~RetainedLayout();
    };
    struct ComputeInfo {
        std::shared_ptr<RetainedLayout> layout;
        VkPipelineCreateFlags flags = 0;
        VkPipelineShaderStageCreateFlags stage_flags = 0;
        std::shared_ptr<const std::vector<uint32_t>> code;
        std::string entry = "main";
        std::vector<VkSpecializationMapEntry> entries;
        std::vector<uint8_t> data;
    };
    bool is_emulating(VkDevice device) const;
    bool native_support(VkPhysicalDevice physicalDevice) const;
    VkPipeline variant_for(VkDevice device, VkPipeline pipeline,
                           const uint32_t base[3], bool* no_offset, VkResult& result);

    mutable std::mutex m_mutex;
    std::unordered_map<VkDevice, bool> m_emulate;
    std::unordered_map<Key, ComputeInfo, KeyHash> m_infos;
    std::unordered_map<Key, std::shared_ptr<RetainedLayout>, KeyHash> m_layouts;
    std::unordered_map<VariantKey, VkPipeline, VariantHash> m_variants;
    std::unordered_map<VkCommandBuffer, Key> m_cmd_pipeline;
    std::unordered_map<VkCommandBuffer, VkResult> m_errors;
};

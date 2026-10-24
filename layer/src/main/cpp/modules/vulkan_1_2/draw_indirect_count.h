#ifndef DRAW_INDIRECT_COUNT_H
#define DRAW_INDIRECT_COUNT_H

#include "extension_module_base.h"

#include <mutex>
#include <unordered_map>
#include <vector>

class DrawIndirectCountModule : public ExtensionModuleBase {
public:
    DrawIndirectCountModule();
    ~DrawIndirectCountModule() override = default;

    bool on_cmd_draw_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride) override;

    bool on_cmd_draw_indexed_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride) override;

    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    void on_pre_create_device_custom(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData) override;

    void on_free_command_buffers(
        VkDevice device,
        uint32_t count,
        const VkCommandBuffer* pCommandBuffers) override;

    void on_begin_command_buffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo) override;

    void on_reset_command_buffer(
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags) override;

    void on_destroy_device(VkDevice device) override;

private:
    struct ScratchBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize capacity{0};
    };

    struct CmdResources {
        VkDevice device{VK_NULL_HANDLE};
        std::vector<ScratchBuffer> scratch;
        size_t current{0};
        VkDeviceSize offset{0};
        std::vector<VkDescriptorPool> pools;
        uint32_t pool_allocated{0};
    };

    struct DeviceResources {
        VkDevice device{VK_NULL_HANDLE};
        VkPhysicalDevice physical{VK_NULL_HANDLE};
        VkDescriptorSetLayout set_layout{VK_NULL_HANDLE};
        VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
        VkPipeline draw_pipeline{VK_NULL_HANDLE};
        VkPipeline indexed_pipeline{VK_NULL_HANDLE};

        PFN_vkCreateBuffer CreateBuffer{nullptr};
        PFN_vkDestroyBuffer DestroyBuffer{nullptr};
        PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements{nullptr};
        PFN_vkAllocateMemory AllocateMemory{nullptr};
        PFN_vkFreeMemory FreeMemory{nullptr};
        PFN_vkBindBufferMemory BindBufferMemory{nullptr};
        PFN_vkCreateDescriptorPool CreateDescriptorPool{nullptr};
        PFN_vkDestroyDescriptorPool DestroyDescriptorPool{nullptr};
        PFN_vkAllocateDescriptorSets AllocateDescriptorSets{nullptr};
        PFN_vkResetDescriptorPool ResetDescriptorPool{nullptr};
        PFN_vkUpdateDescriptorSets UpdateDescriptorSets{nullptr};
        PFN_vkCmdBindPipeline CmdBindPipeline{nullptr};
        PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets{nullptr};
        PFN_vkCmdPushConstants CmdPushConstants{nullptr};
        PFN_vkCmdDispatch CmdDispatch{nullptr};
        PFN_vkCmdPipelineBarrier CmdPipelineBarrier{nullptr};
        PFN_vkDestroyPipeline DestroyPipeline{nullptr};
        PFN_vkDestroyPipelineLayout DestroyPipelineLayout{nullptr};
        PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout{nullptr};
        PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties{nullptr};

        bool ready{false};
    };

    void emulate(
        VkDevice device,
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride,
        bool indexed);

    DeviceResources* ensure_device_locked(VkDevice device);
    CmdResources& ensure_cmd_locked(VkDevice device, VkCommandBuffer commandBuffer);
    bool ensure_scratch_locked(
        DeviceResources& device,
        CmdResources& cmd,
        VkDeviceSize size,
        VkBuffer& out_buffer,
        VkDeviceSize& out_offset);
    VkDescriptorSet acquire_set_locked(DeviceResources& device, CmdResources& cmd);
    void reset_cmd_locked(CmdResources& cmd);
    void destroy_cmd_locked(CmdResources& cmd);
    void destroy_device_locked(DeviceResources& device);

    std::mutex m_mutex;
    std::unordered_map<uint64_t, DeviceResources> m_devices;
    std::unordered_map<uint64_t, CmdResources> m_cmds;
};

#endif  // DRAW_INDIRECT_COUNT_H

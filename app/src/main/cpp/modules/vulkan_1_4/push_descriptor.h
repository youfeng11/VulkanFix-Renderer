#ifndef PUSH_DESCRIPTOR_H
#define PUSH_DESCRIPTOR_H

#include "layer_module.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <atomic>

#ifndef VK_KHR_push_descriptor
#define VK_KHR_push_descriptor 1
#define VK_KHR_PUSH_DESCRIPTOR_SPEC_VERSION 2
#define VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME "VK_KHR_push_descriptor"

#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR ((VkStructureType)1000080000)
#define VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR ((VkDescriptorSetLayoutCreateFlags)0x00000001)
#define VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR ((VkDescriptorUpdateTemplateType)1)

typedef struct VkPhysicalDevicePushDescriptorPropertiesKHR {
    VkStructureType sType;
    void*           pNext;
    uint32_t        maxPushDescriptors;
} VkPhysicalDevicePushDescriptorPropertiesKHR;

typedef void (VKAPI_PTR *PFN_vkCmdPushDescriptorSetKHR)(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t set,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites);

typedef void (VKAPI_PTR *PFN_vkCmdPushDescriptorSetWithTemplateKHR)(
    VkCommandBuffer commandBuffer,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    VkPipelineLayout layout,
    uint32_t set,
    const void* pData);
#endif

class PushDescriptorModule : public IVulkanLayerModule {
public:
    PushDescriptorModule();
    virtual ~PushDescriptorModule() = default;

    const char* get_name() const override { return "VK_KHR_push_descriptor"; }

    // 1. Extensions
    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    // 2. Properties
    void on_pre_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void*& pUserData) override;

    void on_post_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void* pUserData) override;

    // 3. Logical Device
    void on_pre_create_device(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData) override;

    void on_post_create_device(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkResult result,
        void* pUserData) override;

    void on_destroy_device(VkDevice device) override;

    // 4. Descriptor Set Layout
    void on_pre_create_descriptor_set_layout(
        VkDevice device,
        VkDescriptorSetLayoutCreateInfo& createInfo) override;

    void on_post_create_descriptor_set_layout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        VkResult result,
        VkDescriptorSetLayout setLayout) override;

    void on_destroy_descriptor_set_layout(
        VkDevice device,
        VkDescriptorSetLayout setLayout) override;

    // 5. Pipeline Layout
    void on_post_create_pipeline_layout(
        VkDevice device,
        const VkPipelineLayoutCreateInfo* pCreateInfo,
        VkResult result,
        VkPipelineLayout pipelineLayout) override;

    void on_destroy_pipeline_layout(
        VkDevice device,
        VkPipelineLayout pipelineLayout) override;

    // 6. Command Buffers
    void on_post_allocate_command_buffers(
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkResult result,
        VkCommandBuffer* pCommandBuffers) override;

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

    // 7. Descriptor Update Template
    void on_pre_create_descriptor_update_template(
        VkDevice device,
        VkDescriptorUpdateTemplateCreateInfo& createInfo) override;

    // 8. Push Descriptors
    bool on_cmd_push_descriptor_set(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipelineLayout layout,
        uint32_t set,
        uint32_t descriptorWriteCount,
        const VkWriteDescriptorSet* pDescriptorWrites) override;

    bool on_cmd_push_descriptor_set_with_template(
        VkCommandBuffer commandBuffer,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        VkPipelineLayout layout,
        uint32_t set,
        const void* pData) override;

private:
    bool is_phys_device_native(VkPhysicalDevice physDev);
    bool is_device_native(VkDevice device);
    VkDevice get_device_for_cmd(VkCommandBuffer cmd);
    VkDescriptorSetLayout get_set_layout(VkPipelineLayout layout, uint32_t set);
    VkDescriptorSet allocate_push_set(VkDevice device, VkCommandBuffer cmd, VkDescriptorSetLayout setLayout);
    VkDescriptorPool create_pool(VkDevice device, uint32_t maxSets);
    void reset_cmd_pools(VkCommandBuffer cmd);

    struct CmdPushState {
        VkDevice device = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> pools;
        size_t current_pool_idx = 0;
        uint32_t current_pool_allocated = 0;
    };

    std::mutex m_mutex;
    std::atomic<VkDevice> m_primary_dev{VK_NULL_HANDLE};
    std::atomic<bool> m_primary_native{false};
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, bool> m_device_native_support;
    std::unordered_set<uint64_t> m_push_layouts;
    std::unordered_map<uint64_t, std::vector<VkDescriptorSetLayout>> m_pipeline_layouts;
    std::unordered_map<uint64_t, CmdPushState> m_cmd_states;
};

#endif // PUSH_DESCRIPTOR_H

#ifndef LAYER_MODULE_H
#define LAYER_MODULE_H

#include "vk_common.h"
#include <vector>

/**
 * Interface IVulkanLayerModule
 * Represents a modular Vulkan extension emulation, feature supplement,
 * or driver compatibility layer.
 */
class IVulkanLayerModule {
public:
    virtual ~IVulkanLayerModule() = default;

    // Unique module name
    virtual const char* get_name() const = 0;

    // Check if module is active
    virtual bool is_enabled() const { return true; }

    // 1. Device Extension Enumeration:
    virtual void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) {}

    // 2. Physical Device Features Query:
    // Core Vulkan 1.0 features
    virtual void on_get_features(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures* pFeatures) {}

    // Vulkan 1.1+ features query
    virtual void on_pre_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void*& pUserData) {}

    virtual void on_post_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void* pUserData) {}

    // 3. Physical Device Properties Query:
    virtual void on_pre_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void*& pUserData) {}

    virtual void on_post_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void* pUserData) {}

    // 4. Logical Device Creation:
    virtual void on_pre_create_device(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData) {}

    virtual void on_post_create_device(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkResult result,
        void* pUserData) {}

    // 5. Logical Device Destruction:
    virtual void on_destroy_device(VkDevice device) {}

    // 6. Graphics Pipeline Creation:
    // Fast check: returns true if this module needs to inspect/modify any pipeline in this batch.
    virtual bool needs_pipeline_interception(
        VkDevice device,
        uint32_t createInfoCount,
        const VkGraphicsPipelineCreateInfo* pCreateInfos) {
        return false;
    }

    // Called for each pipeline in the batch to apply necessary state modifications.
    virtual void on_modify_pipeline_create_info(
        VkDevice device,
        uint32_t index,
        VkGraphicsPipelineCreateInfo& createInfo,
        VkPipelineVertexInputStateCreateInfo& viState,
        std::vector<void*>& allocationsToFree) {}

    // 7. Descriptor Set Layout:
    virtual void on_pre_create_descriptor_set_layout(
        VkDevice device,
        VkDescriptorSetLayoutCreateInfo& createInfo) {}

    virtual void on_post_create_descriptor_set_layout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        VkResult result,
        VkDescriptorSetLayout setLayout) {}

    virtual void on_destroy_descriptor_set_layout(
        VkDevice device,
        VkDescriptorSetLayout setLayout) {}

    // 8. Pipeline Layout:
    virtual void on_post_create_pipeline_layout(
        VkDevice device,
        const VkPipelineLayoutCreateInfo* pCreateInfo,
        VkResult result,
        VkPipelineLayout pipelineLayout) {}

    virtual void on_destroy_pipeline_layout(
        VkDevice device,
        VkPipelineLayout pipelineLayout) {}

    // 9. Command Buffers:
    virtual void on_post_allocate_command_buffers(
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkResult result,
        VkCommandBuffer* pCommandBuffers) {}

    virtual void on_free_command_buffers(
        VkDevice device,
        uint32_t count,
        const VkCommandBuffer* pCommandBuffers) {}

    virtual void on_begin_command_buffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo) {}

    virtual void on_reset_command_buffer(
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags) {}

    virtual void on_pre_begin_command_buffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo,
        VkCommandBufferBeginInfo& modBeginInfo,
        VkCommandBufferInheritanceInfo& modInheritanceInfo,
        bool& modifiedInheritance) {}

    // 10. Descriptor Update Template:
    virtual void on_pre_create_descriptor_update_template(
        VkDevice device,
        VkDescriptorUpdateTemplateCreateInfo& createInfo) {}

    virtual void on_destroy_descriptor_update_template(
        VkDevice device,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate) {}

    // 11. Push Descriptors:
    virtual bool on_cmd_push_descriptor_set(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipelineLayout layout,
        uint32_t set,
        uint32_t descriptorWriteCount,
        const VkWriteDescriptorSet* pDescriptorWrites) { return false; }

    virtual bool on_cmd_push_descriptor_set_with_template(
        VkCommandBuffer commandBuffer,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        VkPipelineLayout layout,
        uint32_t set,
        const void* pData) { return false; }

    // 12. Images and Image Views:
    virtual void on_post_create_image(
        VkDevice device,
        const VkImageCreateInfo* pCreateInfo,
        VkResult result,
        VkImage image) {}

    virtual void on_destroy_image(
        VkDevice device,
        VkImage image) {}

    virtual void on_post_create_image_view(
        VkDevice device,
        const VkImageViewCreateInfo* pCreateInfo,
        VkResult result,
        VkImageView imageView) {}

    virtual void on_destroy_image_view(
        VkDevice device,
        VkImageView imageView) {}

    // 13. Dynamic Rendering:
    virtual bool on_cmd_begin_rendering(
        VkCommandBuffer commandBuffer,
        const VkRenderingInfo* pRenderingInfo) { return false; }

    virtual bool on_cmd_end_rendering(
        VkCommandBuffer commandBuffer) { return false; }
};

#endif // LAYER_MODULE_H

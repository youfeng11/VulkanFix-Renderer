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
};

#endif // LAYER_MODULE_H

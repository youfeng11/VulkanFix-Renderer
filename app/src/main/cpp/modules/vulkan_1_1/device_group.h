#ifndef DEVICE_GROUP_H
#define DEVICE_GROUP_H

#include "extension_module_base.h"

class DeviceGroupModule : public ExtensionModuleBase {
public:
    DeviceGroupModule();
    ~DeviceGroupModule() override = default;

    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    void on_pre_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void*& pUserData) override;

    void on_post_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void* pUserData) override;

    void on_pre_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void*& pUserData) override;

    void on_post_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void* pUserData) override;

    void on_cmd_set_device_mask(
        VkCommandBuffer commandBuffer,
        uint32_t deviceMask) override;

    bool on_get_device_group_peer_memory_features(
        VkDevice device,
        uint32_t heapIndex,
        uint32_t localDeviceIndex,
        uint32_t remoteDeviceIndex,
        VkPeerMemoryFeatureFlags* pPeerMemoryFeatures) override;

    bool on_cmd_dispatch_base(
        VkCommandBuffer commandBuffer,
        uint32_t baseGroupX,
        uint32_t baseGroupY,
        uint32_t baseGroupZ,
        uint32_t groupCountX,
        uint32_t groupCountY,
        uint32_t groupCountZ) override;

    bool on_enumerate_physical_device_groups(
        VkInstance instance,
        uint32_t* pPhysicalDeviceGroupCount,
        VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties,
        VkResult& outResult) override;

protected:
    void on_pre_create_device_custom(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData) override;
};

#endif // DEVICE_GROUP_H

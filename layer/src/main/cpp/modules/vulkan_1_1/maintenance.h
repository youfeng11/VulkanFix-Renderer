#ifndef MAINTENANCE_H
#define MAINTENANCE_H

#include "extension_module_base.h"

class MaintenanceModule : public ExtensionModuleBase {
public:
    MaintenanceModule();
    ~MaintenanceModule() override = default;

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

    void on_trim_command_pool(
        VkDevice device,
        VkCommandPool commandPool,
        VkCommandPoolTrimFlags flags) override;

    bool on_get_descriptor_set_layout_support(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        VkDescriptorSetLayoutSupport* pSupport) override;

protected:
    void on_pre_create_device_custom(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData) override;
};

#endif // MAINTENANCE_H

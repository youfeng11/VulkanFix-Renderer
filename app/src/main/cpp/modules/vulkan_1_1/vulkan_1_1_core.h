#ifndef VULKAN_1_1_CORE_H
#define VULKAN_1_1_CORE_H

#include "layer_module.h"
#include <mutex>
#include <unordered_map>
#include <vector>

class Vulkan11CoreModule : public IVulkanLayerModule {
public:
    Vulkan11CoreModule();
    ~Vulkan11CoreModule() override = default;

    const char* get_name() const override { return "Vulkan11Core"; }

    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    void on_get_properties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties* pProperties) override;

    void on_pre_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void*& pUserData) override;

    void on_post_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void* pUserData) override;

    void on_get_features(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures* pFeatures) override;

    void on_pre_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void*& pUserData) override;

    void on_post_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void* pUserData) override;

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

    bool is_phys_device_native(VkPhysicalDevice physDev);
    bool is_device_native(VkDevice device);
    uint32_t get_phys_real_api_version(VkPhysicalDevice physDev);

private:
    std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, uint32_t> m_phys_real_api_version;
    std::unordered_map<uint64_t, bool> m_device_needs_emulation;
};

#endif // VULKAN_1_1_CORE_H

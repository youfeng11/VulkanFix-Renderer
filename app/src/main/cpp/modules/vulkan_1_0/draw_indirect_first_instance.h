#ifndef DRAW_INDIRECT_FIRST_INSTANCE_H
#define DRAW_INDIRECT_FIRST_INSTANCE_H

#include "layer_module.h"
#include <unordered_map>
#include <mutex>

/**
 * Emulates the Vulkan core 1.0 feature 'drawIndirectFirstInstance'
 * on devices whose hardware drivers (e.g. some mobile GPUs) do not natively support non-zero firstInstance in indirect draws.
 */
class DrawIndirectFirstInstanceModule : public IVulkanLayerModule {
public:
    DrawIndirectFirstInstanceModule();
    virtual ~DrawIndirectFirstInstanceModule() = default;

    const char* get_name() const override { return "VK_FEATURE_drawIndirectFirstInstance"; }

    void on_get_features(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures* pFeatures) override;

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

    bool is_device_native(VkDevice device);

private:
    bool is_phys_device_native(VkPhysicalDevice physDev);

    std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, bool> m_device_native_support;
};

#endif // DRAW_INDIRECT_FIRST_INSTANCE_H

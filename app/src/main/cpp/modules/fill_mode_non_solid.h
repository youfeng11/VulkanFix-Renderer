#ifndef FILL_MODE_NON_SOLID_H
#define FILL_MODE_NON_SOLID_H

#include "layer_module.h"
#include <unordered_map>
#include <mutex>

/**
 * Emulates the Vulkan core feature 'fillModeNonSolid' (VK_POLYGON_MODE_LINE, VK_POLYGON_MODE_POINT)
 * on devices whose hardware drivers (e.g. ARM Mali, PowerVR) do not natively support it.
 */
class FillModeNonSolidModule : public IVulkanLayerModule {
public:
    FillModeNonSolidModule();
    virtual ~FillModeNonSolidModule() = default;

    const char* get_name() const override { return "VK_FEATURE_fillModeNonSolid"; }

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

    bool needs_pipeline_interception(
        VkDevice device,
        uint32_t createInfoCount,
        const VkGraphicsPipelineCreateInfo* pCreateInfos) override;

    void on_modify_pipeline_create_info(
        VkDevice device,
        uint32_t index,
        VkGraphicsPipelineCreateInfo& createInfo,
        VkPipelineVertexInputStateCreateInfo& viState,
        std::vector<void*>& allocationsToFree) override;

private:
    bool is_device_native(VkPhysicalDevice physDev);

    std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_native_support;
};

#endif // FILL_MODE_NON_SOLID_H

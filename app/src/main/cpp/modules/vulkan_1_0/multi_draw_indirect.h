#ifndef MULTI_DRAW_INDIRECT_H
#define MULTI_DRAW_INDIRECT_H

#include "layer_module.h"
#include <unordered_map>
#include <mutex>

/**
 * Emulates the Vulkan core 1.0 feature 'multiDrawIndirect'
 * on devices whose hardware drivers (e.g. some mobile GPUs) do not natively support it (maxDrawIndirectCount == 1).
 */
class MultiDrawIndirectModule : public IVulkanLayerModule {
public:
    MultiDrawIndirectModule();
    virtual ~MultiDrawIndirectModule() = default;

    const char* get_name() const override { return "VK_FEATURE_multiDrawIndirect"; }

    void on_get_features(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures* pFeatures) override;

    void on_post_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void* pUserData) override;

    void on_get_properties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties* pProperties) override;

    void on_post_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
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

    bool on_cmd_draw_indirect(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        uint32_t drawCount,
        uint32_t stride) override;

    bool on_cmd_draw_indexed_indirect(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        uint32_t drawCount,
        uint32_t stride) override;

private:
    bool is_phys_device_native(VkPhysicalDevice physDev);
    bool is_device_native(VkDevice device);

    std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, bool> m_device_native_support;
};

#endif // MULTI_DRAW_INDIRECT_H

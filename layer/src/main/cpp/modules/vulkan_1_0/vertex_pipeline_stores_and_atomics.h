#ifndef VERTEX_PIPELINE_STORES_AND_ATOMICS_H
#define VERTEX_PIPELINE_STORES_AND_ATOMICS_H

#include "device_native_cache.h"
#include "layer_module.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

/**
 * Compute/replay fallback for vertex-stage storage side effects.
 * Auto emulates missing support; skip preserves native support; force emulates.
 */
class VertexPipelineStoresAndAtomicsModule : public IVulkanLayerModule {
  public:
    VertexPipelineStoresAndAtomicsModule();
    virtual ~VertexPipelineStoresAndAtomicsModule() = default;

    const char *get_name() const override {
        return "VK_FEATURE_vertexPipelineStoresAndAtomics";
    }

    void on_get_features(VkPhysicalDevice physicalDevice,
                         VkPhysicalDeviceFeatures *pFeatures) override;

    void on_post_get_features2(VkPhysicalDevice physicalDevice,
                               VkPhysicalDeviceFeatures2 *pFeatures, void *pUserData) override;

    void on_get_properties(VkPhysicalDevice physicalDevice,
                           VkPhysicalDeviceProperties *pProperties) override;

    void on_post_get_properties2(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceProperties2 *pProperties,
                                 void *pUserData) override;

    void on_pre_create_device(VkPhysicalDevice physicalDevice, VkDeviceCreateInfo *pCreateInfo,
                              VkPhysicalDeviceFeatures *pEnabledFeatures,
                              std::vector<const char *> &enabledExtensions,
                              void *&pUserData) override;

    void on_post_create_device(VkPhysicalDevice physicalDevice, VkDevice device, VkResult result,
                               void *pUserData) override;

    void on_destroy_device(VkDevice device) override;

    bool is_device_native(VkDevice device) override;

  private:
    bool is_phys_device_native(VkPhysicalDevice physDev);
    bool should_emulate(VkPhysicalDevice physicalDevice);

    std::mutex m_mutex;
    DeviceNativeCache m_native_cache;
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, bool> m_device_native_support;
};

#endif // VERTEX_PIPELINE_STORES_AND_ATOMICS_H

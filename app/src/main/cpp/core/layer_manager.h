#ifndef LAYER_MANAGER_H
#define LAYER_MANAGER_H

#include "layer_module.h"
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_set>

class LayerManager {
public:
    static LayerManager& get();

    void register_module(std::unique_ptr<IVulkanLayerModule> module);

    void add_emulated_device(VkDevice device);
    void remove_emulated_device(VkDevice device);

    inline bool is_emulated_device(VkDevice device) {
        if (__builtin_expect(device == m_primary_emulated_device.load(std::memory_order_relaxed), 1)) {
            return true;
        }
        std::lock_guard<std::mutex> lock(m_state_mutex);
        return m_emulated_devices.find((uint64_t)(uintptr_t)device) != m_emulated_devices.end();
    }

    // Intercepted dispatchers
    VkResult dispatch_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        const char* pLayerName,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties);

    void dispatch_get_physical_device_features(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures* pFeatures);

    void dispatch_get_physical_device_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures);

    void dispatch_get_physical_device_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties);

    VkResult dispatch_create_device(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice);

    void dispatch_destroy_device(
        VkDevice device,
        const VkAllocationCallbacks* pAllocator);

    VkResult dispatch_create_graphics_pipelines(
        VkDevice device,
        VkPipelineCache pipelineCache,
        uint32_t createInfoCount,
        const VkGraphicsPipelineCreateInfo* pCreateInfos,
        const VkAllocationCallbacks* pAllocator,
        VkPipeline* pPipelines);

private:
    LayerManager() = default;
    ~LayerManager() = default;
    LayerManager(const LayerManager&) = delete;
    LayerManager& operator=(const LayerManager&) = delete;

    std::vector<std::unique_ptr<IVulkanLayerModule>> m_modules;
    std::mutex m_modules_mutex;

    std::mutex m_state_mutex;
    std::unordered_set<uint64_t> m_emulated_devices;
    std::atomic<VkDevice> m_primary_emulated_device{VK_NULL_HANDLE};
};

#endif // LAYER_MANAGER_H

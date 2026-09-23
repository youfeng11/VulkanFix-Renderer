#ifndef SYNCHRONIZATION2_H
#define SYNCHRONIZATION2_H

#include "layer_module.h"
#include <mutex>
#include <unordered_map>
#include <vector>

class Synchronization2Module : public IVulkanLayerModule {
public:
    Synchronization2Module();
    ~Synchronization2Module() override = default;

    const char* get_name() const override { return "VK_KHR_synchronization2"; }

    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

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

    bool on_cmd_set_event2(
        VkCommandBuffer commandBuffer,
        VkEvent event,
        const VkDependencyInfo* pDependencyInfo) override;

    bool on_cmd_reset_event2(
        VkCommandBuffer commandBuffer,
        VkEvent event,
        VkPipelineStageFlags2 stageMask) override;

    bool on_cmd_wait_events2(
        VkCommandBuffer commandBuffer,
        uint32_t eventCount,
        const VkEvent* pEvents,
        const VkDependencyInfo* pDependencyInfos) override;

    bool on_cmd_pipeline_barrier2(
        VkCommandBuffer commandBuffer,
        const VkDependencyInfo* pDependencyInfo) override;

    bool on_cmd_write_timestamp2(
        VkCommandBuffer commandBuffer,
        VkPipelineStageFlags2 stage,
        VkQueryPool queryPool,
        uint32_t query) override;

    bool on_queue_submit2(
        VkQueue queue,
        uint32_t submitCount,
        const VkSubmitInfo2* pSubmits,
        VkFence fence,
        VkResult& outResult) override;

    bool is_phys_device_native(VkPhysicalDevice physDev);
    bool is_device_native(VkDevice device);

private:
    std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, bool> m_device_needs_emulation;

    std::atomic<VkDevice> m_primary_dev{VK_NULL_HANDLE};
    std::atomic<bool> m_primary_native{false};
};

#endif // SYNCHRONIZATION2_H

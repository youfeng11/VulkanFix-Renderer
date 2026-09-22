#ifndef VULKAN_1_2_H
#define VULKAN_1_2_H

#include "layer_module.h"
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <vector>
#include <memory>

class Vulkan12Module : public IVulkanLayerModule {
public:
    Vulkan12Module();
    ~Vulkan12Module() override = default;

    const char* get_name() const override { return "Vulkan12Emulation"; }

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

    // Semaphore & Timeline Semaphore
    void on_pre_create_semaphore(
        VkDevice device,
        VkSemaphoreCreateInfo& createInfo,
        void*& pUserData) override;

    void on_post_create_semaphore(
        VkDevice device,
        const VkSemaphoreCreateInfo* pCreateInfo,
        VkResult result,
        VkSemaphore semaphore,
        void* pUserData) override;

    void on_destroy_semaphore(
        VkDevice device,
        VkSemaphore semaphore) override;

    bool on_get_semaphore_counter_value(
        VkDevice device,
        VkSemaphore semaphore,
        uint64_t* pValue,
        VkResult& outResult) override;

    bool on_wait_semaphores(
        VkDevice device,
        const VkSemaphoreWaitInfo* pWaitInfo,
        uint64_t timeout,
        VkResult& outResult) override;

    bool on_signal_semaphore(
        VkDevice device,
        const VkSemaphoreSignalInfo* pSignalInfo,
        VkResult& outResult) override;

    bool on_queue_submit(
        VkQueue queue,
        uint32_t submitCount,
        const VkSubmitInfo* pSubmits,
        VkFence fence,
        VkResult& outResult) override;

    void on_queue_wait_idle(VkQueue queue) override;
    void on_device_wait_idle(VkDevice device) override;
    bool is_timeline_semaphore(VkSemaphore semaphore) override;

    // Host Query Reset
    bool on_reset_query_pool(
        VkDevice device,
        VkQueryPool queryPool,
        uint32_t firstQuery,
        uint32_t queryCount) override;

    // RenderPass2
    bool on_create_render_pass2(
        VkDevice device,
        const VkRenderPassCreateInfo2* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkRenderPass* pRenderPass,
        VkResult& outResult) override;

    bool on_cmd_begin_render_pass2(
        VkCommandBuffer commandBuffer,
        const VkRenderPassBeginInfo* pRenderPassBegin,
        const VkSubpassBeginInfo* pSubpassBeginInfo) override;

    bool on_cmd_next_subpass2(
        VkCommandBuffer commandBuffer,
        const VkSubpassBeginInfo* pSubpassBeginInfo,
        const VkSubpassEndInfo* pSubpassEndInfo) override;

    bool on_cmd_end_render_pass2(
        VkCommandBuffer commandBuffer,
        const VkSubpassEndInfo* pSubpassEndInfo) override;

    // Draw Indirect Count
    bool on_cmd_draw_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride) override;

    bool on_cmd_draw_indexed_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride) override;

    // Buffer Device Address
    bool on_get_buffer_device_address(
        VkDevice device,
        const VkBufferDeviceAddressInfo* pInfo,
        VkDeviceAddress& outAddress) override;

    bool is_phys_device_native(VkPhysicalDevice physDev);
    bool is_device_native(VkDevice device);

private:
    std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, bool> m_device_needs_emulation;

    // Timeline semaphores
    struct FenceHolder {
        VkDevice device{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        bool isInternal{false};
        ~FenceHolder();
    };

    struct PendingSignal {
        uint64_t targetValue{0};
        std::shared_ptr<FenceHolder> fenceHolder;
    };

    struct TimelineSemaphoreState {
        std::atomic<uint64_t> counter{0};
        std::condition_variable cv;
        std::vector<PendingSignal> pendingSignals;
    };

    void check_pending_signals_locked(std::shared_ptr<TimelineSemaphoreState>& state);

    std::mutex m_semaphore_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<TimelineSemaphoreState>> m_timeline_semaphores;
};

#endif // VULKAN_1_2_H

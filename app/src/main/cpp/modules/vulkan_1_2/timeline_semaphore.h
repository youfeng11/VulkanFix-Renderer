#ifndef TIMELINE_SEMAPHORE_H
#define TIMELINE_SEMAPHORE_H

#include "extension_module_base.h"
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <memory>

class TimelineSemaphoreModule : public ExtensionModuleBase {
public:
    TimelineSemaphoreModule();
    ~TimelineSemaphoreModule() override = default;

    bool is_phys_device_native(VkPhysicalDevice physDev) override;
    bool is_device_native(VkDevice device) override;

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

protected:
    bool query_native_support(VkPhysicalDevice physDev) override;

private:
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

#endif // TIMELINE_SEMAPHORE_H

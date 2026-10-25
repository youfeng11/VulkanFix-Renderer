#pragma once
#include "vk_common.h"
#include <functional>
#include <memory>
namespace vkfix {
// Queue submissions own their inputs until native dispatch. Every GPU signal
// carries a distinct binary release token for each queue/physical device; its
// first acquire waits at ALL_COMMANDS and covers its later submissions too.
class TimelineScheduler {
  public:
    static TimelineScheduler &get();
    TimelineScheduler();
    ~TimelineScheduler();
    VkResult start(VkDevice device, const VkDeviceCreateInfo &info);
    void stop(VkDevice device);
    bool active(VkDevice device);
    VkResult error(VkDevice device);
    void add(VkDevice device, VkSemaphore semaphore, uint64_t initial);
    void add_binary(VkDevice device, VkSemaphore semaphore);
    void remove(VkDevice device, VkSemaphore semaphore);
    bool contains(VkDevice device, VkSemaphore semaphore);
    bool counter(VkDevice device, VkSemaphore semaphore, uint64_t *value, VkResult &result);
    bool signal(VkDevice device, const VkSemaphoreSignalInfo *info, VkResult &result);
    bool wait(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout, VkResult &result);
    bool submit(VkQueue queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence,
                VkResult &result);
    bool bind_sparse(VkQueue queue, uint32_t count, const VkBindSparseInfo *infos, VkFence fence,
                     VkResult &result);
    bool queue_idle(VkQueue queue, VkResult &result);
    bool device_idle(VkDevice device, VkResult &result);
    VkResult drain(VkQueue queue);
    VkResult before_present(VkQueue queue, const VkPresentInfoKHR *info);
    void after_present(VkQueue queue, const VkPresentInfoKHR *info, VkResult result = VK_SUCCESS);
    // Internal maintenance calls bypass staged application dependencies, but
    // share native queue external synchronization with the worker.
    bool submit_internal(VkQueue queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence,
                         VkResult &result);
    VkResult native_queue_call(VkQueue queue, const std::function<VkResult()> &call);
    // Deferred application-fence submission must not overlap native host
    // reads. Callbacks must be bounded (host waits use zero-timeout probes).
    VkResult native_fence_call(VkDevice device, const std::function<VkResult()> &call);

  private:
    struct State;
    std::unique_ptr<State> state;
};
} // namespace vkfix

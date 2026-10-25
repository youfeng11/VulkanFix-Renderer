#include "core/driver_loader.h"
#include "core/timeline_scheduler.h"
#include "core/vk_pnext.h"
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
namespace {
using Key = std::pair<uint64_t, uint64_t>;
Key key(VkDevice d, uint64_t h) {
    return {(uint64_t)(uintptr_t)d, h};
}
std::mutex mutex;
struct Binary {
    bool signaled = false;
    uint64_t signals = 0, waits = 0;
};
std::map<Key, Binary> binaries;
std::map<Key, VkResult> fences;
std::map<Key, uint64_t> native_counters;
std::map<uint64_t, VkDevice> queues;
struct Item {
    VkDevice device;
    VkFence fence;
    std::vector<VkSemaphore> waits, signals;
    std::vector<uint64_t> wvalues, svalues;
    std::vector<VkCommandBuffer> commands;
};
std::map<uint64_t, std::deque<Item>> pending;
std::vector<uint64_t> executed;
uint64_t next_handle = 0x10000;
unsigned failures = 0, invalid_native = 0, submissions = 0;
unsigned semaphore_destructions = 0, fence_destructions = 0;
std::atomic<uint64_t> monitored_fence{0};
std::atomic<bool> host_fence_read{false}, fence_overlap{false};
std::atomic<unsigned> monitored_submits{0};
VkResult semaphore_allocation = VK_SUCCESS, fence_allocation = VK_SUCCESS,
         submit_result = VK_SUCCESS;
uint64_t sparse_buffer = 0, sparse_opaque = 0, sparse_image = 0, frame_image = 0;
uint8_t frame_tag = 0;
std::vector<uint32_t> observed_wait_devices, observed_signal_devices;
uint32_t observed_resource_device = 0;
void check(bool ok, const char *name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
void pump() {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &queue : pending) {
            if (queue.second.empty())
                continue;
            auto &job = queue.second.front();
            bool ready = true;
            for (size_t i = 0; i < job.waits.size(); ++i) {
                auto k = key(job.device, (uint64_t)job.waits[i]);
                if (native_counters.count(k))
                    ready &= native_counters[k] >= job.wvalues[i];
                else
                    ready &= binaries[k].signaled;
            }
            if (!ready)
                continue;
            for (auto sem : job.waits) {
                auto k = key(job.device, (uint64_t)sem);
                if (!native_counters.count(k))
                    binaries[k].signaled = false;
            }
            for (auto command : job.commands)
                executed.push_back((uint64_t)(uintptr_t)command);
            for (size_t i = 0; i < job.signals.size(); ++i) {
                auto k = key(job.device, (uint64_t)job.signals[i]);
                if (native_counters.count(k))
                    native_counters[k] = job.svalues[i];
                else {
                    if (binaries[k].signaled)
                        ++invalid_native;
                    binaries[k].signaled = true;
                }
            }
            if (job.fence)
                fences[key(job.device, (uint64_t)job.fence)] = VK_SUCCESS;
            queue.second.pop_front();
            changed = true;
        }
    }
}
static VKAPI_ATTR void VKAPI_CALL get_queue(VkDevice d, uint32_t family, uint32_t index,
                                            VkQueue *out) {
    *out = (VkQueue)((uintptr_t)d * 256 + family * 16 + index + 1);
    queues[(uint64_t)(uintptr_t)*out] = d;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_sem(VkDevice d, const VkSemaphoreCreateInfo *,
                                                 const VkAllocationCallbacks *, VkSemaphore *out) {
    std::lock_guard<std::mutex> lock(mutex);
    if (semaphore_allocation != VK_SUCCESS)
        return semaphore_allocation;
    *out = (VkSemaphore)next_handle++;
    binaries[key(d, (uint64_t)*out)] = {};
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_sem(VkDevice d, VkSemaphore sem,
                                              const VkAllocationCallbacks *) {
    std::lock_guard<std::mutex> lock(mutex);
    ++semaphore_destructions;
    binaries.erase(key(d, (uint64_t)sem));
}
static VKAPI_ATTR VkResult VKAPI_CALL create_fence(VkDevice d, const VkFenceCreateInfo *,
                                                   const VkAllocationCallbacks *, VkFence *out) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fence_allocation != VK_SUCCESS)
        return fence_allocation;
    *out = (VkFence)next_handle++;
    fences[key(d, (uint64_t)*out)] = VK_NOT_READY;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_fence(VkDevice d, VkFence f,
                                                const VkAllocationCallbacks *) {
    std::lock_guard<std::mutex> lock(mutex);
    ++fence_destructions;
    fences.erase(key(d, (uint64_t)f));
}
static VKAPI_ATTR VkResult VKAPI_CALL fence_status(VkDevice d, VkFence f) {
    std::lock_guard<std::mutex> lock(mutex);
    pump();
    return fences[key(d, (uint64_t)f)];
}
static VKAPI_ATTR VkResult VKAPI_CALL counter(VkDevice d, VkSemaphore sem, uint64_t *out) {
    std::lock_guard<std::mutex> lock(mutex);
    *out = native_counters[key(d, (uint64_t)sem)];
    return VK_SUCCESS;
}
template <class T> std::vector<T> copy(const T *p, size_t n) {
    return p && n ? std::vector<T>(p, p + n) : std::vector<T>{};
}
VkResult enqueue(VkQueue q, uint32_t wc, const VkSemaphore *waits, uint32_t sc,
                 const VkSemaphore *signals, const void *chain, VkFence f,
                 const std::vector<VkCommandBuffer> &commands) {
    if (submit_result != VK_SUCCESS)
        return submit_result;
    if (f && (uint64_t)f == monitored_fence.load()) {
        ++monitored_submits;
        if (host_fence_read.load()) fence_overlap = true;
    }
    Item job{queues[(uint64_t)(uintptr_t)q],
             f,
             copy(waits, wc),
             copy(signals, sc),
             std::vector<uint64_t>(wc),
             std::vector<uint64_t>(sc),
             commands};
    if (auto timeline = vku::find_pnext<VkTimelineSemaphoreSubmitInfo>(
            chain, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)) {
        for (size_t i = 0; i < wc; ++i)
            job.wvalues[i] = timeline->pWaitSemaphoreValues[i];
        for (size_t i = 0; i < sc; ++i)
            job.svalues[i] = timeline->pSignalSemaphoreValues[i];
    }
    for (auto sem : job.waits) {
        auto k = key(job.device, (uint64_t)sem);
        if (!native_counters.count(k)) {
            auto &state = binaries[k];
            if (state.signals <= state.waits)
                ++invalid_native;
            ++state.waits;
        }
    }
    for (auto sem : job.signals) {
        auto k = key(job.device, (uint64_t)sem);
        if (!native_counters.count(k))
            ++binaries[k].signals;
    }
    if (auto frame =
            vku::find_pnext<VkFrameBoundaryEXT>(chain, VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT)) {
        frame_image = (uint64_t)frame->pImages[0];
        frame_tag = static_cast<const uint8_t *>(frame->pTag)[0];
    }
    if (auto group = vku::find_pnext<VkDeviceGroupSubmitInfo>(
            chain, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO)) {
        observed_wait_devices = copy(group->pWaitSemaphoreDeviceIndices, group->waitSemaphoreCount);
        observed_signal_devices =
            copy(group->pSignalSemaphoreDeviceIndices, group->signalSemaphoreCount);
    }
    if (auto group = vku::find_pnext<VkDeviceGroupBindSparseInfo>(
            chain, VK_STRUCTURE_TYPE_DEVICE_GROUP_BIND_SPARSE_INFO))
        observed_resource_device = group->resourceDeviceIndex;
    pending[(uint64_t)(uintptr_t)q].push_back(std::move(job));
    ++submissions;
    pump();
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q, uint32_t count, const VkSubmitInfo *infos,
                                             VkFence f) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!count)
        return enqueue(q, 0, nullptr, 0, nullptr, nullptr, f, {});
    for (uint32_t i = 0; i < count; ++i) {
        auto &s = infos[i];
        auto r = enqueue(q, s.waitSemaphoreCount, s.pWaitSemaphores, s.signalSemaphoreCount,
                         s.pSignalSemaphores, s.pNext, i + 1 == count ? f : VK_NULL_HANDLE,
                         copy(s.pCommandBuffers, s.commandBufferCount));
        if (r != VK_SUCCESS)
            return r;
    }
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL sparse(VkQueue q, uint32_t count,
                                             const VkBindSparseInfo *infos, VkFence f) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!count)
        return enqueue(q, 0, nullptr, 0, nullptr, nullptr, f, {});
    for (uint32_t i = 0; i < count; ++i) {
        auto &s = infos[i];
        if (s.bufferBindCount)
            sparse_buffer = s.pBufferBinds[0].pBinds[0].memoryOffset;
        if (s.imageOpaqueBindCount)
            sparse_opaque = s.pImageOpaqueBinds[0].pBinds[0].memoryOffset;
        if (s.imageBindCount)
            sparse_image = s.pImageBinds[0].pBinds[0].memoryOffset;
        auto r = enqueue(q, s.waitSemaphoreCount, s.pWaitSemaphores, s.signalSemaphoreCount,
                         s.pSignalSemaphores, s.pNext, i + 1 == count ? f : VK_NULL_HANDLE, {});
        if (r != VK_SUCCESS)
            return r;
    }
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL queue_idle(VkQueue q) {
    std::lock_guard<std::mutex> lock(mutex);
    pump();
    return pending[(uint64_t)(uintptr_t)q].empty() ? VK_SUCCESS : VK_TIMEOUT;
}
static VKAPI_ATTR VkResult VKAPI_CALL device_idle(VkDevice) {
    std::lock_guard<std::mutex> lock(mutex);
    pump();
    return VK_SUCCESS;
}
} // namespace
extern "C" {
VkInstance get_last_instance() {
    return VK_NULL_HANDLE;
}
void *get_real_proc(VkInstance, VkDevice, const char *name) {
#define MOCK(n, f)                                                                                 \
    if (!strcmp(name, n))                                                                          \
    return (void *)f
    MOCK("vkGetDeviceQueue", get_queue);
    MOCK("vkCreateSemaphore", create_sem);
    MOCK("vkDestroySemaphore", destroy_sem);
    MOCK("vkCreateFence", create_fence);
    MOCK("vkDestroyFence", destroy_fence);
    MOCK("vkGetFenceStatus", fence_status);
    MOCK("vkQueueSubmit", submit);
    MOCK("vkQueueBindSparse", sparse);
    MOCK("vkQueueWaitIdle", queue_idle);
    MOCK("vkDeviceWaitIdle", device_idle);
    MOCK("vkGetSemaphoreCounterValue", counter);
#undef MOCK
    return nullptr;
}
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto &scheduler = vkfix::TimelineScheduler::get();
    auto a = (VkDevice)(uintptr_t)0x10, b = (VkDevice)(uintptr_t)0x20;
    float priorities[]{1, 1};
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 2;
    qci.pQueuePriorities = priorities;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    check(scheduler.start(a, ci) == VK_SUCCESS && scheduler.start(b, ci) == VK_SUCCESS,
          "start independent per-device queue schedulers");
    VkQueue q0{}, q1{};
    get_queue(a, 0, 0, &q0);
    get_queue(a, 0, 1, &q1);
    VkSemaphore gate = (VkSemaphore)0x100, done = (VkSemaphore)0x101;
    scheduler.add(a, gate, 0);
    scheduler.add(a, done, 0);
    scheduler.add(b, gate, 77);
    uint64_t value = 0;
    VkResult result{};
    scheduler.counter(b, gate, &value, result);
    check(result == VK_SUCCESS && value == 77,
          "same-valued handles stay isolated between asynchronous devices");
    auto host_signal = [&](VkSemaphore sem, uint64_t v) {
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signal.semaphore = sem;
        signal.value = v;
        VkResult r{};
        scheduler.signal(a, &signal, r);
        return r;
    };
    auto wait_for = [&](VkSemaphore sem, uint64_t target) {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount = 1;
        wait.pSemaphores = &sem;
        wait.pValues = &target;
        VkResult r{};
        scheduler.wait(a, &wait, 2000000000ULL, r);
        return r;
    };
    uint64_t future = 5, target = 10;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = timeline.signalSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &future;
    timeline.pSignalSemaphoreValues = &target;
    VkImage image = (VkImage)0x222;
    uint8_t tag[]{0x5a, 1, 2};
    VkFrameBoundaryEXT frame{VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT, &timeline};
    frame.imageCount = 1;
    frame.pImages = &image;
    frame.tagSize = 3;
    frame.pTag = tag;
    VkCommandBuffer command = (VkCommandBuffer)(uintptr_t)0xabc;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo job{VK_STRUCTURE_TYPE_SUBMIT_INFO, &frame};
    job.waitSemaphoreCount = job.signalSemaphoreCount = job.commandBufferCount = 1;
    job.pWaitSemaphores = &gate;
    job.pWaitDstStageMask = &stage;
    job.pSignalSemaphores = &done;
    job.pCommandBuffers = &command;
    auto began = std::chrono::steady_clock::now();
    scheduler.submit(q0, 1, &job, VK_NULL_HANDLE, result);
    check(result == VK_SUCCESS &&
              std::chrono::steady_clock::now() - began < std::chrono::milliseconds(100),
          "future wait is accepted without waiting for any signal");
    future = 999;
    target = 999;
    image = (VkImage)0x999;
    tag[0] = 0;
    command = (VkCommandBuffer)(uintptr_t)0xdef;
    {
        std::lock_guard<std::mutex> lock(mutex);
        check(submissions == 0, "unmet timeline dependencies prevent native dispatch");
    }
    check(host_signal(gate, 5) == VK_SUCCESS && wait_for(done, 10) == VK_SUCCESS,
          "host signal wakes and completes a queued future-value submission");
    {
        std::lock_guard<std::mutex> lock(mutex);
        check(
            executed == std::vector<uint64_t>{0xabc} && frame_image == 0x222 && frame_tag == 0x5a,
            "command, frame arrays, opaque bytes, and values survive immediate caller input reuse");
    }
    // Sparse bindings and their nested pBinds must also remain owned.
    future = 6;
    target = 20;
    timeline.pNext = nullptr;
    VkSparseMemoryBind buffer_bind{};
    buffer_bind.memoryOffset = 512;
    VkSparseMemoryBind opaque_bind{};
    opaque_bind.memoryOffset = 256;
    VkSparseImageMemoryBind image_bind{};
    image_bind.memoryOffset = 768;
    VkSparseBufferMemoryBindInfo buffer_info{(VkBuffer)0x777, 1, &buffer_bind};
    VkSparseImageOpaqueMemoryBindInfo opaque_info{(VkImage)0x888, 1, &opaque_bind};
    VkSparseImageMemoryBindInfo image_info{(VkImage)0x999, 1, &image_bind};
    VkBindSparseInfo bind{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, &timeline};
    bind.waitSemaphoreCount = bind.signalSemaphoreCount = 1;
    bind.pWaitSemaphores = &gate;
    bind.pSignalSemaphores = &done;
    bind.bufferBindCount = bind.imageOpaqueBindCount = bind.imageBindCount = 1;
    bind.pBufferBinds = &buffer_info;
    bind.pImageOpaqueBinds = &opaque_info;
    bind.pImageBinds = &image_info;
    scheduler.bind_sparse(q1, 1, &bind, VK_NULL_HANDLE, result);
    check(result == VK_SUCCESS, "sparse submission accepts an unresolved timeline dependency");
    buffer_bind.memoryOffset = opaque_bind.memoryOffset = image_bind.memoryOffset = 999;
    buffer_info.pBinds = nullptr;
    opaque_info.pBinds = nullptr;
    image_info.pBinds = nullptr;
    check(host_signal(gate, 6) == VK_SUCCESS && wait_for(done, 20) == VK_SUCCESS,
          "sparse future-value submission runs after its host signal");
    {
        std::lock_guard<std::mutex> lock(mutex);
        check(sparse_buffer == 512 && sparse_opaque == 256 && sparse_image == 768,
              "all nested sparse binding arrays are deep-copied");
    }
    // A native binary wait cannot be issued before its deferred producer.
    VkSemaphore binary{};
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    create_sem(a, &sci, nullptr, &binary);
    scheduler.add_binary(a, binary);
    uint64_t seven = 7, zero = 0, thirty = 30;
    VkTimelineSemaphoreSubmitInfo producer_values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    producer_values.waitSemaphoreValueCount = producer_values.signalSemaphoreValueCount = 1;
    producer_values.pWaitSemaphoreValues = &seven;
    producer_values.pSignalSemaphoreValues = &zero;
    VkSubmitInfo producer{VK_STRUCTURE_TYPE_SUBMIT_INFO, &producer_values};
    producer.waitSemaphoreCount = producer.signalSemaphoreCount = 1;
    producer.pWaitSemaphores = &gate;
    producer.pWaitDstStageMask = &stage;
    producer.pSignalSemaphores = &binary;
    VkTimelineSemaphoreSubmitInfo consumer_values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    consumer_values.waitSemaphoreValueCount = consumer_values.signalSemaphoreValueCount = 1;
    consumer_values.pWaitSemaphoreValues = &zero;
    consumer_values.pSignalSemaphoreValues = &thirty;
    VkSubmitInfo consumer{VK_STRUCTURE_TYPE_SUBMIT_INFO, &consumer_values};
    consumer.waitSemaphoreCount = consumer.signalSemaphoreCount = 1;
    consumer.pWaitSemaphores = &binary;
    consumer.pWaitDstStageMask = &stage;
    consumer.pSignalSemaphores = &done;
    unsigned before;
    {
        std::lock_guard<std::mutex> lock(mutex);
        before = submissions;
    }
    scheduler.submit(q0, 1, &producer, VK_NULL_HANDLE, result);
    scheduler.submit(q1, 1, &consumer, VK_NULL_HANDLE, result);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        std::lock_guard<std::mutex> lock(mutex);
        check(submissions == before,
              "native binary waits stay deferred until their wrapped producer is dispatched");
    }
    check(host_signal(gate, 7) == VK_SUCCESS && wait_for(done, 30) == VK_SUCCESS,
          "binary producer and consumer advance after a future timeline dependency");
    {
        std::lock_guard<std::mutex> lock(mutex);
        check(invalid_native == 0, "native binary submissions always have an earlier signal and "
                                   "never double-consume a token");
    }
    // Reservation/allocation failures leave no queued work or changed epochs.
    VkSemaphore fresh{};
    create_sem(a, &sci, nullptr, &fresh);
    scheduler.add_binary(a, fresh);
    VkSemaphore mixed_signals[]{done, fresh};
    uint64_t mixed_values[]{40, 0};
    VkTimelineSemaphoreSubmitInfo allocation_values{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    allocation_values.signalSemaphoreValueCount = 2;
    allocation_values.pSignalSemaphoreValues = mixed_values;
    VkSubmitInfo allocation{VK_STRUCTURE_TYPE_SUBMIT_INFO, &allocation_values};
    allocation.signalSemaphoreCount = 2;
    allocation.pSignalSemaphores = mixed_signals;
    semaphore_allocation = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    scheduler.submit(q0, 1, &allocation, VK_NULL_HANDLE, result);
    check(result == VK_ERROR_OUT_OF_DEVICE_MEMORY,
          "binary release-token allocation failure is returned before acceptance");
    semaphore_allocation = VK_SUCCESS;
    fence_allocation = VK_ERROR_OUT_OF_HOST_MEMORY;
    scheduler.submit(q0, 1, &allocation, VK_NULL_HANDLE, result);
    check(result == VK_ERROR_OUT_OF_HOST_MEMORY,
          "completion-fence allocation failure rolls back the whole queued transaction");
    fence_allocation = VK_SUCCESS;
    scheduler.submit(q0, 1, &allocation, VK_NULL_HANDLE, result);
    check(result == VK_SUCCESS && wait_for(done, 40) == VK_SUCCESS,
          "a successful retry is not blocked by failed binary epoch reservations");
    VkSemaphore render{};
    create_sem(a, &sci, nullptr, &render);
    scheduler.add_binary(a, render);
    uint64_t eight = 8, fifty = 50;
    VkSemaphore render_signals[]{done, render};
    uint64_t render_values[]{50, 0};
    VkTimelineSemaphoreSubmitInfo render_timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    render_timeline.waitSemaphoreValueCount = 1;
    render_timeline.pWaitSemaphoreValues = &eight;
    render_timeline.signalSemaphoreValueCount = 2;
    render_timeline.pSignalSemaphoreValues = render_values;
    VkSubmitInfo render_job{VK_STRUCTURE_TYPE_SUBMIT_INFO, &render_timeline};
    render_job.waitSemaphoreCount = 1;
    render_job.pWaitSemaphores = &gate;
    render_job.pWaitDstStageMask = &stage;
    render_job.signalSemaphoreCount = 2;
    render_job.pSignalSemaphores = render_signals;
    scheduler.submit(q0, 1, &render_job, VK_NULL_HANDLE, result);
    VkCommandBuffer maintenance = (VkCommandBuffer)(uintptr_t)0xfff;
    VkSubmitInfo internal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    internal.commandBufferCount = 1;
    internal.pCommandBuffers = &maintenance;
    check(scheduler.native_queue_call(q0,
                                      [&] {
                                          scheduler.submit_internal(q0, 1, &internal,
                                                                    VK_NULL_HANDLE, result);
                                          return result;
                                      }) == VK_SUCCESS,
          "internal maintenance has reentrant synchronized native access without waiting for "
          "staged application dependencies");
    check(scheduler.native_queue_call(
              q1,
              [&] {
                  VkResult accepted{};
                  scheduler.submit(q1, 1, &internal, VK_NULL_HANDLE, accepted);
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
                  return accepted == VK_SUCCESS ? scheduler.error(a) : accepted;
              }) == VK_SUCCESS,
          "a native callback can reenter scheduler state while ready worker work waits for queue "
          "access");
    const auto fence_device = (VkDevice)(uintptr_t)0x3030;
    check(scheduler.start(fence_device, ci) == VK_SUCCESS,
          "start an independent device for deferred application fence host access");
    VkQueue fence_queue{};
    get_queue(fence_device, 0, 0, &fence_queue);
    VkFence app_fence{};
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    create_fence(fence_device, &fence_info, nullptr, &app_fence);
    monitored_fence = (uint64_t)app_fence;
    bool stayed_unsignaled = false;
    check(scheduler.native_fence_call(fence_device, [&] {
              host_fence_read = true;
              VkResult accepted{};
              scheduler.submit(fence_queue, 0, nullptr, app_fence, accepted);
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
              stayed_unsignaled = fence_status(fence_device, app_fence) == VK_NOT_READY;
              // Reentry must remain possible while the worker is ready to
              // submit this very fence, otherwise the lock order deadlocks.
              auto state = scheduler.error(fence_device);
              host_fence_read = false;
              return accepted == VK_SUCCESS ? state : accepted;
          }) == VK_SUCCESS && stayed_unsignaled,
          "a reentrant native fence read excludes deferred fence submission without locking scheduler state");
    scheduler.queue_idle(fence_queue, result);
    check(result == VK_SUCCESS && !fence_overlap && monitored_submits == 1 &&
              fence_status(fence_device, app_fence) == VK_SUCCESS,
          "releasing host fence access allows exactly one native submission and completion");
    monitored_fence = 0;
    scheduler.stop(fence_device);
    destroy_fence(fence_device, app_fence, nullptr);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render;
    VkResult present_result = VK_ERROR_UNKNOWN;
    std::thread present_thread([&] {
        present_result = scheduler.before_present(q1, &present);
        if (present_result == VK_SUCCESS) {
            VkSubmitInfo consume{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            consume.waitSemaphoreCount = 1;
            consume.pWaitSemaphores = &render;
            consume.pWaitDstStageMask = &stage;
            scheduler.submit_internal(q1, 1, &consume, VK_NULL_HANDLE, present_result);
            scheduler.after_present(q1, &present);
        }
    });
    check(host_signal(gate, 8) == VK_SUCCESS && wait_for(done, fifty) == VK_SUCCESS,
          "a presentation dependency can await a producer staged on another queue");
    present_thread.join();
    check(present_result == VK_SUCCESS,
          "presentation waits are recorded after their native signal is submitted");
    render_job.waitSemaphoreCount = 0;
    render_values[0] = 60;
    scheduler.submit(q0, 1, &render_job, VK_NULL_HANDLE, result);
    check(result == VK_SUCCESS && wait_for(done, 60) == VK_SUCCESS,
          "a render-finished binary semaphore can be reused after a presentation wait");
    scheduler.queue_idle(q0, result);
    check(result == VK_SUCCESS, "queue idle drains accepted queue work");
    scheduler.device_idle(a, result);
    check(result == VK_SUCCESS, "device idle drains every accepted queue");
    check(scheduler.before_present(q1, &present) == VK_SUCCESS,
          "presentation reserves its wait before native queue access");
    render_values[0] = 70;
    scheduler.submit(q0, 1, &render_job, VK_NULL_HANDLE, result);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    scheduler.counter(a, done, &value, result);
    check(result == VK_SUCCESS && value == 60,
          "a later queue signal cannot overtake a presentation wait not yet submitted natively");
    VkSubmitInfo final_present{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    final_present.waitSemaphoreCount = 1;
    final_present.pWaitSemaphores = &render;
    final_present.pWaitDstStageMask = &stage;
    scheduler.submit_internal(q1, 1, &final_present, VK_NULL_HANDLE, result);
    scheduler.after_present(q1, &present);
    check(result == VK_SUCCESS && wait_for(done, 70) == VK_SUCCESS,
          "the next binary signal advances after native presentation wait submission");
    scheduler.device_idle(a, result);
    check(scheduler.before_present(q1, &present) == VK_SUCCESS,
          "a presentation wait is reserved before an out-of-memory result");
    scheduler.after_present(q1, &present, VK_ERROR_OUT_OF_HOST_MEMORY);
    check(scheduler.before_present(q1, &present) == VK_SUCCESS,
          "an out-of-memory presentation rolls back its unsubmitted wait for retry");
    scheduler.submit_internal(q1, 1, &final_present, VK_NULL_HANDLE, result);
    scheduler.after_present(q1, &present, VK_ERROR_OUT_OF_DATE_KHR);
    render_values[0] = 80;
    scheduler.submit(q0, 1, &render_job, VK_NULL_HANDLE, result);
    check(result == VK_SUCCESS && wait_for(done, 80) == VK_SUCCESS && invalid_native == 0,
          "an out-of-date presentation still consumes its native wait and permits later binary "
          "reuse");
    scheduler.device_idle(a, result);
    scheduler.remove(a, render);
    destroy_sem(a, render, nullptr);
    scheduler.remove(a, binary);
    scheduler.remove(a, fresh);
    destroy_sem(a, binary, nullptr);
    destroy_sem(a, fresh, nullptr);
    scheduler.stop(a);
    check(!scheduler.active(a) && scheduler.active(b),
          "stopping one scheduler preserves the other device");
    scheduler.stop(b);
    auto group_device = (VkDevice)(uintptr_t)0x40;
    VkPhysicalDevice physicals[]{(VkPhysicalDevice)(uintptr_t)1, (VkPhysicalDevice)(uintptr_t)2};
    VkDeviceGroupDeviceCreateInfo group_create{VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO};
    group_create.physicalDeviceCount = 2;
    group_create.pPhysicalDevices = physicals;
    ci.pNext = &group_create;
    check(scheduler.start(group_device, ci) == VK_SUCCESS, "start a two-physical-device scheduler");
    VkQueue gq{};
    get_queue(group_device, 0, 0, &gq);
    scheduler.add(group_device, gate, 0);
    scheduler.add(group_device, done, 0);
    uint32_t gpu_one = 1, gpu_zero = 0;
    uint64_t group_value = 1;
    VkDeviceGroupSubmitInfo group_info{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO};
    group_info.signalSemaphoreCount = 1;
    group_info.pSignalSemaphoreDeviceIndices = &gpu_one;
    VkTimelineSemaphoreSubmitInfo group_timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                 &group_info};
    group_timeline.signalSemaphoreValueCount = 1;
    group_timeline.pSignalSemaphoreValues = &group_value;
    VkSubmitInfo group_job{VK_STRUCTURE_TYPE_SUBMIT_INFO, &group_timeline};
    group_job.signalSemaphoreCount = 1;
    group_job.pSignalSemaphores = &gate;
    scheduler.submit(gq, 1, &group_job, VK_NULL_HANDLE, result);
    auto group_wait = [&](VkSemaphore sem, uint64_t target) {
        VkSemaphoreWaitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        w.semaphoreCount = 1;
        w.pSemaphores = &sem;
        w.pValues = &target;
        VkResult r{};
        scheduler.wait(group_device, &w, 2000000000ULL, r);
        return r;
    };
    check(result == VK_SUCCESS && group_wait(gate, 1) == VK_SUCCESS &&
              observed_signal_devices == std::vector<uint32_t>({1, 1, 1, 1}),
          "all queue/device release tokens preserve the original signal device index");
    uint64_t source_group_value = 1;
    group_value = 2;
    group_timeline.waitSemaphoreValueCount = 1;
    group_timeline.pWaitSemaphoreValues = &source_group_value;
    group_info.waitSemaphoreCount = 1;
    group_info.pWaitSemaphoreDeviceIndices = &gpu_one;
    group_job.waitSemaphoreCount = 1;
    group_job.pWaitSemaphores = &gate;
    group_job.pWaitDstStageMask = &stage;
    group_job.pSignalSemaphores = &done;
    scheduler.submit(gq, 1, &group_job, VK_NULL_HANDLE, result);
    check(result == VK_SUCCESS && group_wait(done, 2) == VK_SUCCESS &&
              observed_wait_devices == std::vector<uint32_t>{1},
          "an acquire token keeps the requested physical wait device");
    group_value = 3;
    group_info.pWaitSemaphoreDeviceIndices = &gpu_zero;
    scheduler.submit(gq, 1, &group_job, VK_NULL_HANDLE, result);
    check(result == VK_SUCCESS && group_wait(done, 3) == VK_SUCCESS &&
              observed_wait_devices == std::vector<uint32_t>{0},
          "the same logical queue independently acquires for a different physical device");
    group_value = 4;
    group_info.pWaitSemaphoreDeviceIndices = &gpu_one;
    scheduler.submit(gq, 1, &group_job, VK_NULL_HANDLE, result);
    check(result == VK_SUCCESS && group_wait(done, 4) == VK_SUCCESS &&
              observed_wait_devices.empty(),
          "repeat acquisition on one physical device never consumes the token again");
    VkDeviceGroupBindSparseInfo sparse_group{VK_STRUCTURE_TYPE_DEVICE_GROUP_BIND_SPARSE_INFO};
    sparse_group.resourceDeviceIndex = sparse_group.memoryDeviceIndex = 1;
    VkTimelineSemaphoreSubmitInfo sparse_values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                &sparse_group};
    sparse_values.waitSemaphoreValueCount = sparse_values.signalSemaphoreValueCount = 1;
    sparse_values.pWaitSemaphoreValues = &source_group_value;
    sparse_values.pSignalSemaphoreValues = &group_value;
    VkBindSparseInfo sparse_job{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, &sparse_values};
    sparse_job.waitSemaphoreCount = sparse_job.signalSemaphoreCount = 1;
    sparse_job.pWaitSemaphores = &gate;
    sparse_job.pSignalSemaphores = &done;
    group_value = 5;
    scheduler.bind_sparse(gq, 1, &sparse_job, VK_NULL_HANDLE, result);
    bool sparse_first = result == VK_SUCCESS && group_wait(done, 5) == VK_SUCCESS;
    group_value = 6;
    scheduler.bind_sparse(gq, 1, &sparse_job, VK_NULL_HANDLE, result);
    check(sparse_first && result == VK_SUCCESS && group_wait(done, 6) == VK_SUCCESS &&
              observed_resource_device == 1,
          "repeated sparse waits preserve resource-device scope without consuming a persistent "
          "command acquire");
    scheduler.stop(group_device);
    ci.pNext = nullptr;
    auto c = (VkDevice)(uintptr_t)0x30;
    check(scheduler.start(c, ci) == VK_SUCCESS, "start failure-propagation device");
    scheduler.add(c, done, 0);
    VkQueue cq{};
    get_queue(c, 0, 0, &cq);
    target = 1;
    VkTimelineSemaphoreSubmitInfo error_values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    error_values.signalSemaphoreValueCount = 1;
    error_values.pSignalSemaphoreValues = &target;
    VkSubmitInfo error_job{VK_STRUCTURE_TYPE_SUBMIT_INFO, &error_values};
    error_job.signalSemaphoreCount = 1;
    error_job.pSignalSemaphores = &done;
    submit_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    scheduler.submit(cq, 1, &error_job, VK_NULL_HANDLE, result);
    check(result == VK_SUCCESS, "backend execution occurs after asynchronous acceptance");
    VkSemaphoreWaitInfo lost_wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    lost_wait.semaphoreCount = 1;
    lost_wait.pSemaphores = &done;
    lost_wait.pValues = &target;
    scheduler.wait(c, &lost_wait, 2000000000ULL, result);
    check(result == VK_ERROR_DEVICE_LOST && scheduler.error(c) == VK_ERROR_DEVICE_LOST,
          "late native submission failure becomes device loss instead of an unsignaled-fence hang");
    auto sem_before_stop = semaphore_destructions, fence_before_stop = fence_destructions;
    scheduler.stop(c);
    check(semaphore_destructions == sem_before_stop && fence_destructions == fence_before_stop,
          "logical device loss leaves private native objects to device destruction instead of "
          "freeing pending GPU references");
    submit_result = VK_SUCCESS;
    printf("Timeline scheduler failures: %u\n", failures);
    return failures ? 1 : 0;
}

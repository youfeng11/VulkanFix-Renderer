#include "core/input_pnext_copy.h"
#include "core/layer_manager.h"
#include "core/vk_pnext.h"
#include "modules/vulkan_1_2/timeline_semaphore.h"
#include "modules/vulkan_1_3/synchronization2.h"
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace {
using Key = std::pair<uint64_t, uint64_t>;
Key key(VkDevice d, uint64_t h) {
    return {(uint64_t)(uintptr_t)d, h};
}
std::mutex mutex;
std::map<Key, VkResult> fences;
std::map<Key, uint64_t> native_values;
std::map<uint64_t, VkDevice> queue_devices;
uint64_t next_fence = 0x10000;
VkResult create_fence_result = VK_SUCCESS, native_counter_result = VK_SUCCESS;
VkResult create_semaphore_result = VK_SUCCESS;
unsigned native_submits = 0, fail_submit = 0, destroyed_fences = 0;
unsigned native_submit2_calls = 0;
static VKAPI_ATTR VkResult VKAPI_CALL submit2(VkQueue, uint32_t, const VkSubmitInfo2 *, VkFence) {
    ++native_submit2_calls;
    return VK_SUCCESS;
}
class NativeSynchronization2 : public Synchronization2Module {
    bool is_device_native(VkDevice) override {
        return true;
    }
};
bool creation_chain_clean = false;
bool last_export_missing = false;
VkSemaphore created_semaphore = (VkSemaphore)0x7788;
uintptr_t next_device = 0x5000;
static VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice, const VkDeviceCreateInfo *,
                                                    const VkAllocationCallbacks *, VkDevice *out) {
    *out = (VkDevice)next_device;
    next_device += 0x1000;
    return VK_SUCCESS;
}
struct Snapshot {
    VkQueue queue;
    VkFence fence;
    uint32_t count;
    std::vector<VkSemaphore> waits, signals;
    std::vector<uint64_t> wait_values, signal_values;
    std::vector<uint32_t> wait_devices, signal_devices;
    const void *chain = nullptr;
};
std::vector<Snapshot> snapshots;
static VKAPI_ATTR void VKAPI_CALL get_queue(VkDevice d, uint32_t, uint32_t index, VkQueue *out) {
    *out = (VkQueue)((uintptr_t)d * 16 + index + 1);
    queue_devices[(uint64_t)(uintptr_t)*out] = d;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_fence(VkDevice d, const VkFenceCreateInfo *,
                                                   const VkAllocationCallbacks *, VkFence *out) {
    std::lock_guard<std::mutex> lock(mutex);
    if (create_fence_result != VK_SUCCESS)
        return create_fence_result;
    *out = (VkFence)next_fence++;
    fences[key(d, (uint64_t)*out)] = VK_NOT_READY;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL reset_fences(VkDevice d, uint32_t count,
                                                   const VkFence *values) {
    std::lock_guard<std::mutex> lock(mutex);
    for (uint32_t i = 0; i < count; ++i)
        fences[key(d, (uint64_t)values[i])] = VK_NOT_READY;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_fence(VkDevice d, VkFence f,
                                                const VkAllocationCallbacks *) {
    std::lock_guard<std::mutex> lock(mutex);
    fences.erase(key(d, (uint64_t)f));
    ++destroyed_fences;
}
static VKAPI_ATTR VkResult VKAPI_CALL fence_status(VkDevice d, VkFence f) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = fences.find(key(d, (uint64_t)f));
    return it == fences.end() ? VK_ERROR_UNKNOWN : it->second;
}
static VKAPI_ATTR VkResult VKAPI_CALL counter(VkDevice d, VkSemaphore semaphore, uint64_t *value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (native_counter_result != VK_SUCCESS)
        return native_counter_result;
    *value = native_values[key(d, (uint64_t)semaphore)];
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue, uint32_t count,
                                             const VkSubmitInfo *infos, VkFence fence) {
    ++native_submits;
    if (fail_submit == native_submits)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    Snapshot snapshot{queue, fence, count};
    if (count) {
        auto &info = infos[0];
        snapshot.chain = info.pNext;
        if (info.waitSemaphoreCount)
            snapshot.waits.assign(info.pWaitSemaphores,
                                  info.pWaitSemaphores + info.waitSemaphoreCount);
        if (info.signalSemaphoreCount)
            snapshot.signals.assign(info.pSignalSemaphores,
                                    info.pSignalSemaphores + info.signalSemaphoreCount);
        if (auto t = vku::find_pnext<VkTimelineSemaphoreSubmitInfo>(
                info.pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)) {
            if (t->waitSemaphoreValueCount)
                snapshot.wait_values.assign(t->pWaitSemaphoreValues,
                                            t->pWaitSemaphoreValues + t->waitSemaphoreValueCount);
            if (t->signalSemaphoreValueCount)
                snapshot.signal_values.assign(t->pSignalSemaphoreValues,
                                              t->pSignalSemaphoreValues +
                                                  t->signalSemaphoreValueCount);
        }
        if (auto g = vku::find_pnext<VkDeviceGroupSubmitInfo>(
                info.pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO)) {
            if (g->waitSemaphoreCount)
                snapshot.wait_devices.assign(g->pWaitSemaphoreDeviceIndices,
                                             g->pWaitSemaphoreDeviceIndices +
                                                 g->waitSemaphoreCount);
            if (g->signalSemaphoreCount)
                snapshot.signal_devices.assign(g->pSignalSemaphoreDeviceIndices,
                                               g->pSignalSemaphoreDeviceIndices +
                                                   g->signalSemaphoreCount);
        }
    }
    snapshots.push_back(std::move(snapshot));
    if (fence)
        fences[key(queue_devices[(uint64_t)(uintptr_t)queue], (uint64_t)fence)] = VK_NOT_READY;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_semaphore(VkDevice, const VkSemaphoreCreateInfo *info,
                                                       const VkAllocationCallbacks *,
                                                       VkSemaphore *out) {
    last_export_missing =
        !vku::has_pnext(info->pNext, VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO);
    creation_chain_clean =
        !vku::has_pnext(info->pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) &&
        vku::has_pnext(info->pNext, VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO);
    if (create_semaphore_result == VK_SUCCESS)
        *out = created_semaphore;
    return create_semaphore_result;
}
} // namespace
void *timeline_mock_proc(const char *name) {
    if (!strcmp(name, "vkCreateDevice"))
        return (void *)create_device;
    if (!strcmp(name, "vkGetDeviceQueue"))
        return (void *)get_queue;
    if (!strcmp(name, "vkCreateFence"))
        return (void *)create_fence;
    if (!strcmp(name, "vkResetFences"))
        return (void *)reset_fences;
    if (!strcmp(name, "vkDestroyFence"))
        return (void *)destroy_fence;
    if (!strcmp(name, "vkGetFenceStatus"))
        return (void *)fence_status;
    if (!strcmp(name, "vkQueueSubmit"))
        return (void *)submit;
    if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR"))
        return (void *)submit2;
    if (!strcmp(name, "vkGetSemaphoreCounterValue") ||
        !strcmp(name, "vkGetSemaphoreCounterValueKHR"))
        return (void *)counter;
    if (!strcmp(name, "vkCreateSemaphore"))
        return (void *)create_semaphore;
    return nullptr;
}
void run_timeline_regressions(void (*check)(bool, const char *)) {
    auto &manager = LayerManager::get();
    auto owned = std::make_unique<TimelineSemaphoreModule>();
    auto &module = *owned;
    manager.register_module(std::move(owned));
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    VkDevice a{}, b{}, c{};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    auto physical = (VkPhysicalDevice)(uintptr_t)0x123;
    check(manager.dispatch_create_device(physical, &dci, nullptr, &a) == VK_SUCCESS &&
              manager.dispatch_create_device(physical, &dci, nullptr, &b) == VK_SUCCESS &&
              manager.dispatch_create_device(physical, &dci, nullptr, &c) == VK_SUCCESS,
          "multi-device timeline fixtures use actual manager device registration");
    VkQueue a0{}, a1{}, b0{}, c0{};
    manager.dispatch_get_device_queue(a, 0, 0, &a0);
    manager.dispatch_get_device_queue(a, 0, 1, &a1);
    manager.dispatch_get_device_queue(b, 0, 0, &b0);
    manager.dispatch_get_device_queue(c, 0, 0, &c0);
    auto add = [&](VkDevice d, VkSemaphore sem, uint64_t value) {
        module.on_post_create_semaphore(d, nullptr, VK_SUCCESS, sem, new uint64_t(value));
    };
    auto value = [&](VkDevice d, VkSemaphore sem) {
        uint64_t result = UINT64_MAX;
        VkResult status = VK_ERROR_UNKNOWN;
        module.on_get_semaphore_counter_value(d, sem, &result, status);
        return status == VK_SUCCESS ? result : UINT64_MAX;
    };
    auto signal_job = [&](VkQueue q, VkSemaphore sem, uint64_t number,
                          VkFence fence = VK_NULL_HANDLE) {
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &number;
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &sem;
        VkResult result = VK_ERROR_UNKNOWN;
        module.on_queue_submit(q, 1, &info, fence, result);
        return result;
    };
    VkSemaphore same = (VkSemaphore)0x100, other = (VkSemaphore)0x101;
    add(a, same, 2);
    add(b, same, 9);
    add(a, other, 0);
    check(value(a, same) == 2 && value(b, same) == 9,
          "identical semaphore handles on different devices retain independent counters");
    check(!module.is_timeline_semaphore(c, same) && module.is_timeline_semaphore(a, same),
          "timeline classification includes the owning device");
    auto low = (VkSemaphore)0x1000, wide = (VkSemaphore)0x123456781000ULL;
    add(a, low, 31);
    add(a, wide, 47);
    check(value(a, low) == 31 && value(a, wide) == 47,
          "non-dispatchable semaphore keys preserve all 64 bits");
    check(signal_job(a0, same, 3) == VK_SUCCESS && signal_job(a1, other, 4) == VK_SUCCESS &&
              signal_job(b0, same, 12) == VK_SUCCESS,
          "queue submissions register independent internal completion fences");
    module.on_queue_wait_idle(a0);
    check(value(a, same) == 3 && value(a, other) == 0 && value(b, same) == 9,
          "queue idle completes only signals submitted on that queue and device");
    module.on_device_wait_idle(a);
    check(value(a, other) == 4 && value(b, same) == 9,
          "device idle never completes another device's timeline signals");
    module.on_queue_wait_idle(b0);
    check(value(b, same) == 12, "the owning queue idle completes its pending signal");

    VkSemaphore native = (VkSemaphore)0x202;
    VkSemaphore wait_sems[]{same, native};
    uint64_t targets[]{3, 5};
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 2;
    wait.pSemaphores = wait_sems;
    wait.pValues = targets;
    auto wait_result = [&](uint64_t timeout) {
        VkResult result{};
        module.on_wait_semaphores(a, &wait, timeout, result);
        return result;
    };
    check(wait_result(0) == VK_TIMEOUT,
          "mixed WAIT_ALL does not assume an unmet native timeline is satisfied");
    wait.flags = VK_SEMAPHORE_WAIT_ANY_BIT;
    check(wait_result(0) == VK_SUCCESS, "mixed WAIT_ANY accepts a satisfied emulated timeline");
    targets[0] = 4;
    check(wait_result(0) == VK_TIMEOUT,
          "mixed WAIT_ANY times out when both native and emulated counters are too small");
    native_values[key(a, (uint64_t)native)] = 5;
    check(wait_result(0) == VK_SUCCESS, "mixed WAIT_ANY accepts a satisfied native timeline");
    wait.flags = 0;
    check(wait_result(0) == VK_TIMEOUT, "mixed WAIT_ALL still requires the emulated counter");
    VkSemaphoreSignalInfo host_signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    host_signal.semaphore = same;
    host_signal.value = 4;
    VkResult status{};
    module.on_signal_semaphore(a, &host_signal, status);
    check(wait_result(0) == VK_SUCCESS,
          "mixed WAIT_ALL succeeds only after both counters reach their targets");
    targets[1] = 6;
    std::thread native_signal([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard<std::mutex> lock(mutex);
        native_values[key(a, (uint64_t)native)] = 6;
    });
    auto asynchronous = wait_result(1000000000);
    native_signal.join();
    check(asynchronous == VK_SUCCESS, "mixed waits observe later native signals without an "
                                      "emulated condition-variable notification");
    native_counter_result = VK_ERROR_DEVICE_LOST;
    check(wait_result(0) == VK_ERROR_DEVICE_LOST,
          "mixed waits propagate native counter-query failures");
    native_counter_result = VK_SUCCESS;

    VkSemaphore native_out = (VkSemaphore)0x203;
    VkSemaphore mixed_waits[]{same, native}, mixed_signals[]{other, native_out};
    uint64_t wait_values[]{4, 6}, signal_values[]{8, 9};
    uint32_t wait_devices[]{2, 3}, signal_devices[]{0, 1};
    VkDeviceGroupSubmitInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO};
    group.waitSemaphoreCount = group.signalSemaphoreCount = 2;
    group.pWaitSemaphoreDeviceIndices = wait_devices;
    group.pSignalSemaphoreDeviceIndices = signal_devices;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                           &group};
    timeline.waitSemaphoreValueCount = timeline.signalSemaphoreValueCount = 2;
    timeline.pWaitSemaphoreValues = wait_values;
    timeline.pSignalSemaphoreValues = signal_values;
    VkProtectedSubmitInfo protected_info{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO, &timeline};
    VkSubmitInfo mixed{VK_STRUCTURE_TYPE_SUBMIT_INFO, &protected_info};
    mixed.waitSemaphoreCount = mixed.signalSemaphoreCount = 2;
    mixed.pWaitSemaphores = mixed_waits;
    mixed.pSignalSemaphores = mixed_signals;
    VkPipelineStageFlags stages[]{VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    mixed.pWaitDstStageMask = stages;
    auto saved_group = group;
    auto saved_timeline = timeline;
    auto saved_protected = protected_info;
    auto saved_mixed = mixed;
    VkFence app_fence{};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    create_fence(a, &fci, nullptr, &app_fence);
    auto before = snapshots.size();
    module.on_queue_submit(a0, 1, &mixed, app_fence, status);
    const auto cleaned = snapshots.at(before);
    check(status == VK_SUCCESS && cleaned.waits == std::vector<VkSemaphore>{native} &&
              cleaned.signals == std::vector<VkSemaphore>{native_out},
          "submission removes only emulated waits and signals");
    check(cleaned.wait_values == std::vector<uint64_t>{6} &&
              cleaned.signal_values == std::vector<uint64_t>{9},
          "native timeline submit values stay aligned after filtering");
    check(cleaned.wait_devices == std::vector<uint32_t>{3} &&
              cleaned.signal_devices == std::vector<uint32_t>{1},
          "device-group indices stay aligned with remaining semaphores");
    check(cleaned.chain != mixed.pNext && !memcmp(&group, &saved_group, sizeof(group)) &&
              !memcmp(&timeline, &saved_timeline, sizeof(timeline)) &&
              !memcmp(&protected_info, &saved_protected, sizeof(protected_info)) &&
              !memcmp(&mixed, &saved_mixed, sizeof(mixed)),
          "submission rewriting never changes application pNext nodes or links");
    check(cleaned.fence != app_fence && snapshots.back().count == 0 &&
              snapshots.back().fence == app_fence,
          "private completion precedes a separately submitted application fence");
    fences[key(a, (uint64_t)cleaned.fence)] = VK_SUCCESS;
    fences[key(a, (uint64_t)app_fence)] = VK_SUCCESS;
    reset_fences(a, 1, &app_fence);
    check(value(a, other) == 8,
          "resetting the application fence cannot hide a completed timeline signal");

    manager.register_module(std::make_unique<NativeSynchronization2>());
    manager.update_device_bypass_flags(a);
    check(manager.should_intercept_device_proc(a, "vkQueueSubmit2") &&
              manager.should_intercept_device_proc(a, "vkQueueSubmit2KHR"),
          "native synchronization2 never bypasses an emulated timeline device");
    VkSemaphoreSubmitInfo signal2{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal2.semaphore = other;
    signal2.value = 11;
    signal2.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 info2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    info2.signalSemaphoreInfoCount = 1;
    info2.pSignalSemaphoreInfos = &signal2;
    auto submit2_before = native_submit2_calls;
    auto result2 = manager.dispatch_queue_submit2(a1, 1, &info2, VK_NULL_HANDLE);
    module.on_queue_wait_idle(a1);
    check(
        result2 == VK_SUCCESS && native_submit2_calls == submit2_before && value(a, other) == 11,
        "native synchronization2 routes emulated timeline operations through timeline conversion");
    signal2.semaphore = (VkSemaphore)0x9000;
    signal2.value = 0;
    check(manager.dispatch_queue_submit2(a1, 1, &info2, VK_NULL_HANDLE) == VK_SUCCESS &&
              native_submit2_calls == submit2_before + 1,
          "native-only synchronization2 submissions retain the direct driver path");
    VkSemaphore csem = (VkSemaphore)0x300;
    add(c, csem, 0);
    create_fence_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    before = native_submits;
    check(signal_job(c0, csem, 7) == VK_ERROR_OUT_OF_DEVICE_MEMORY && native_submits == before &&
              value(c, csem) == 0,
          "completion-fence allocation failure is returned without submitting or advancing the "
          "counter");
    create_fence_result = VK_SUCCESS;
    fail_submit = native_submits + 1;
    check(signal_job(c0, csem, 7) == VK_ERROR_OUT_OF_DEVICE_MEMORY && value(c, csem) == 0,
          "native submit failure never publishes a timeline signal");
    fail_submit = 0;
    auto lost_sem = (VkSemaphore)0x301;
    add(c, lost_sem, 0);
    signal_job(c0, lost_sem, 10);
    fences[key(c, (uint64_t)snapshots.back().fence)] = VK_ERROR_DEVICE_LOST;
    uint64_t unchanged = 0x5555;
    module.on_get_semaphore_counter_value(c, lost_sem, &unchanged, status);
    check(status == VK_ERROR_DEVICE_LOST && unchanged == 0x5555,
          "completion failure propagates without publishing a counter or overwriting output");
    wait.semaphoreCount = 1;
    wait.pSemaphores = &lost_sem;
    uint64_t lost_target = 10;
    wait.pValues = &lost_target;
    module.on_wait_semaphores(c, &wait, 0, status);
    check(status == VK_ERROR_DEVICE_LOST, "host wait propagates emulated completion failure");

    VkSemaphoreTypeCreateInfo type_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = 17;
    VkExportSemaphoreCreateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                            &type_info};
    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &export_info};
    auto saved_export = export_info;
    auto saved_type = type_info;
    VkSemaphore created{};
    check(
        manager.dispatch_create_semaphore(a, &sci, nullptr, &created) == VK_SUCCESS &&
            creation_chain_clean && value(a, created) == 17 &&
            !memcmp(&export_info, &saved_export, sizeof(export_info)) &&
            !memcmp(&type_info, &saved_type, sizeof(type_info)),
        "semaphore creation strips only private type nodes and preserves the application's chain");
    create_semaphore_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    check(manager.dispatch_create_semaphore(a, &sci, nullptr, &created) ==
                  VK_ERROR_OUT_OF_HOST_MEMORY &&
              !memcmp(&export_info, &saved_export, sizeof(export_info)),
          "failed semaphore creation runs cleanup and leaves the input chain unchanged");
    create_semaphore_result = VK_SUCCESS;
    created_semaphore = (VkSemaphore)0x7789;
    export_info.handleTypes = 0;
    check(manager.dispatch_create_semaphore(a, &sci, nullptr, &created) == VK_SUCCESS &&
              last_export_missing && export_info.pNext == &type_info && !type_info.pNext,
          "zero export types are normalized only in the private native semaphore chain");
    auto unknown = VkBaseInStructure{(VkStructureType)0x7ffffffe, nullptr};
    vkfix::InputPNextCopy copy;
    check(copy.copy(&unknown, vkfix::InputChainKind::Submit) == VK_ERROR_EXTENSION_NOT_PRESENT &&
              !copy.head(),
          "unknown input nodes fail without guessed sizes or writes");
    type_info.pNext = &export_info;
    check(copy.copy(&export_info, vkfix::InputChainKind::Semaphore) ==
              VK_ERROR_INITIALIZATION_FAILED,
          "cyclic input chains are rejected without mutation");
    type_info.pNext = nullptr;
    module.on_device_wait_idle(a);
    const auto destroyed_before = destroyed_fences;
    manager.dispatch_destroy_device(a, nullptr);
    check(!module.is_timeline_semaphore(a, same) && module.is_timeline_semaphore(b, same) &&
              value(b, same) == 12 && destroyed_fences > destroyed_before,
          "device destruction retires its states and pooled fences while preserving other devices");
    module.on_device_wait_idle(b);
    manager.dispatch_destroy_device(b, nullptr);
    module.on_device_wait_idle(c);
    manager.dispatch_destroy_device(c, nullptr);
    destroy_fence(a, app_fence, nullptr);
}

#include "timeline_semaphore.h"
#include "driver_loader.h"
#include "input_pnext_copy.h"
#include "timeline_scheduler.h"
#include "layer_manager.h"
#include "vk_pnext.h"
#include <algorithm>
#include <chrono>
#include <thread>

REGISTER_LAYER_MODULE(TimelineSemaphoreModule);

TimelineSemaphoreModule::TimelineSemaphoreModule()
    : ExtensionModuleBase(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
                          VK_KHR_TIMELINE_SEMAPHORE_SPEC_VERSION, VK_API_VERSION_1_2) {
    LOGI("TimelineSemaphoreModule initialized");
}

void TimelineSemaphoreModule::on_pre_get_properties2(VkPhysicalDevice physicalDevice,
                                                     VkPhysicalDeviceProperties2 *pProperties,
                                                     void *&pUserData) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pProperties)
        return;

    pUserData = vku::unlink_pnext(pProperties->pNext,
                                  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES);
}

void TimelineSemaphoreModule::on_post_get_properties2(VkPhysicalDevice physicalDevice,
                                                      VkPhysicalDeviceProperties2 *pProperties,
                                                      void *pUserData) {
    if (!pProperties || !pUserData)
        return;

    auto *p = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreProperties *>(pUserData);
    p->maxTimelineSemaphoreValueDifference = ~0ULL;
    vku::relink_pnext(pProperties->pNext, pUserData);
}

void TimelineSemaphoreModule::on_pre_get_features2(VkPhysicalDevice physicalDevice,
                                                   VkPhysicalDeviceFeatures2 *pFeatures,
                                                   void *&pUserData) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures)
        return;

    pUserData = vku::unlink_pnext(pFeatures->pNext,
                                  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
}

void TimelineSemaphoreModule::on_post_get_features2(VkPhysicalDevice physicalDevice,
                                                    VkPhysicalDeviceFeatures2 *pFeatures,
                                                    void *pUserData) {
    if (!pFeatures || !pUserData)
        return;

    auto *f = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures *>(pUserData);
    f->timelineSemaphore = VK_TRUE;
    vku::relink_pnext(pFeatures->pNext, pUserData);
}

bool TimelineSemaphoreModule::query_native_support(VkPhysicalDevice physDev) {
    EmulationMode mode = parse_emulation_mode("VULKAN_FIX_EMULATE_TIMELINE_SEMAPHORE",
                                              "FORCE_EMULATE_TIMELINE_SEMAPHORE");
    if (mode == EmulationMode::ForceEmulate) {
        LOGI("Timeline Semaphore: FORCE EMULATE enabled for physical device %p", physDev);
        return false;
    }
    if (mode == EmulationMode::Skip) {
        LOGI("Timeline Semaphore: SKIP/BYPASS enabled for physical device %p", physDev);
        return true;
    }

    PFN_vkGetPhysicalDeviceProperties real_props = (PFN_vkGetPhysicalDeviceProperties)get_real_proc(
        get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (!real_props)
        return false;
    VkPhysicalDeviceProperties props{};
    real_props(physDev, &props);

    PFN_vkGetPhysicalDeviceFeatures2 real_feat2 = (PFN_vkGetPhysicalDeviceFeatures2)get_real_proc(
        get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2");
    if (!real_feat2) {
        real_feat2 = (PFN_vkGetPhysicalDeviceFeatures2)get_real_proc(
            get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2KHR");
    }

    if (props.apiVersion >= VK_API_VERSION_1_2 && real_feat2) {
        VkPhysicalDeviceVulkan12Features v12Feat{};
        v12Feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &v12Feat;
        real_feat2(physDev, &f2);
        if (v12Feat.timelineSemaphore) {
            return true;
        }
    }

    PFN_vkEnumerateDeviceExtensionProperties real_enum =
        (PFN_vkEnumerateDeviceExtensionProperties)get_real_proc(
            get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    if (real_enum) {
        uint32_t count = 0;
        if (real_enum(physDev, NULL, &count, NULL) == VK_SUCCESS && count > 0) {
            std::vector<VkExtensionProperties> exts(count);
            if (real_enum(physDev, NULL, &count, exts.data()) == VK_SUCCESS) {
                bool hasExt = false;
                for (const auto &e : exts) {
                    if (strcmp(e.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
                        hasExt = true;
                        break;
                    }
                }
                if (hasExt && real_feat2) {
                    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR tsFeat{};
                    tsFeat.sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
                    VkPhysicalDeviceFeatures2 f2{};
                    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    f2.pNext = &tsFeat;
                    real_feat2(physDev, &f2);
                    if (tsFeat.timelineSemaphore) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool TimelineSemaphoreModule::is_phys_device_native(VkPhysicalDevice physDev) {
    {
        std::shared_lock<std::shared_mutex> lock(m_ext_mutex);
        auto it = m_phys_native.find((uint64_t)(uintptr_t)physDev);
        if (it != m_phys_native.end()) {
            return it->second;
        }
    }
    bool native = query_native_support(physDev);
    std::unique_lock<std::shared_mutex> lock(m_ext_mutex);
    m_phys_native[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("[VK_KHR_timeline_semaphore] Device %p lacks native timelineSemaphore feature "
             "support, enabling emulation!",
             physDev);
    } else {
        LOGI("[VK_KHR_timeline_semaphore] Device %p natively supports timelineSemaphore feature",
             physDev);
    }
    return native;
}

bool TimelineSemaphoreModule::is_device_native(VkDevice device) {
    if (device == VK_NULL_HANDLE)
        return false;
    std::shared_lock<std::shared_mutex> lock(m_ext_mutex);
    auto it = m_device_native.find((uint64_t)(uintptr_t)device);
    if (it != m_device_native.end()) {
        return it->second;
    }
    return false;
}

void TimelineSemaphoreModule::on_pre_create_device(VkPhysicalDevice physicalDevice,
                                                   VkDeviceCreateInfo *pCreateInfo,
                                                   VkPhysicalDeviceFeatures *pEnabledFeatures,
                                                   std::vector<const char *> &enabledExtensions,
                                                   void *&pUserData) {
    pUserData = nullptr;
    if (!pCreateInfo)
        return;

    bool physNative = is_phys_device_native(physicalDevice);

    bool appRequestedInExts = false;
    for (const char *ext : enabledExtensions) {
        if (strcmp(ext, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
            appRequestedInExts = true;
            break;
        }
    }

    auto *v12Feat = vku::find_pnext_mut<VkPhysicalDeviceVulkan12Features>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    bool appRequestedInV12 = (v12Feat && v12Feat->timelineSemaphore);

    auto *tsFeat = vku::find_pnext_mut<VkPhysicalDeviceTimelineSemaphoreFeaturesKHR>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR);
    bool appRequestedInTsFeat = (tsFeat && tsFeat->timelineSemaphore);

    PFN_vkGetPhysicalDeviceProperties real_props = (PFN_vkGetPhysicalDeviceProperties)get_real_proc(
        get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    uint32_t realApiVer = VK_API_VERSION_1_0;
    if (real_props) {
        VkPhysicalDeviceProperties props{};
        real_props(physicalDevice, &props);
        realApiVer = props.apiVersion;
    }

    bool deviceCanBeNative = false;
    if (physNative) {
        if (realApiVer >= VK_API_VERSION_1_2 && appRequestedInV12) {
            deviceCanBeNative = true;
        } else if (appRequestedInExts && (appRequestedInTsFeat || !tsFeat)) {
            deviceCanBeNative = true;
        }
    }

    if (!deviceCanBeNative) {
        for (auto it = enabledExtensions.begin(); it != enabledExtensions.end();) {
            if (strcmp(*it, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
                it = enabledExtensions.erase(it);
                LOGI("TimelineSemaphore: stripped %s from enabledExtensions",
                     VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
            } else {
                ++it;
            }
        }
        vku::unlink_pnext(pCreateInfo->pNext,
                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR);
    }

    pUserData = reinterpret_cast<void *>(static_cast<uintptr_t>(deviceCanBeNative ? 1 : 0));
}

void TimelineSemaphoreModule::on_post_create_device(VkPhysicalDevice physicalDevice,
                                                    VkDevice device, VkResult result,
                                                    void *pUserData) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool isNative = (reinterpret_cast<uintptr_t>(pUserData) == 1);
        std::unique_lock<std::shared_mutex> lock(m_ext_mutex);
        m_device_native[(uint64_t)(uintptr_t)device] = isNative;
        LOGI("TimelineSemaphore: device %p native=%d (emulation=%s)", device, isNative ? 1 : 0,
             isNative ? "OFF" : "ON");
    }
}

void TimelineSemaphoreModule::on_destroy_device(VkDevice device) {
    vkfix::TimelineScheduler::get().stop(device);

    // Release states before draining the pool: their private fences may return
    // to that pool during cleanup. Never retain them until after native destroy.
    std::vector<std::shared_ptr<TimelineSemaphoreState>> retired;
    {
        std::unique_lock<std::shared_mutex> lock(m_table_rw_mutex);
        for (auto it = m_timeline_semaphores.begin(); it != m_timeline_semaphores.end();) {
            if (it->second->device == device) {
                retired.push_back(std::move(it->second));
                it = m_timeline_semaphores.erase(it);
                m_active_timeline_count.fetch_sub(1, std::memory_order_relaxed);
            } else
                ++it;
        }
    }
    retired.clear();
    ExtensionModuleBase::on_destroy_device(device);
    auto destroy = (PFN_vkDestroyFence)get_real_proc(get_last_instance(), device, "vkDestroyFence");
    std::lock_guard<std::mutex> lock(m_fence_pool_mutex);
    for (auto it = m_fence_pool.begin(); it != m_fence_pool.end();) {
        if (it->device == device) {
            if (destroy)
                destroy(device, it->fence, nullptr);
            it = m_fence_pool.erase(it);
        } else
            ++it;
    }
}

void TimelineSemaphoreModule::on_pre_create_semaphore(VkDevice device,
                                                      VkSemaphoreCreateInfo &createInfo,
                                                      void *&pUserData) {
    pUserData = nullptr;
    if (is_device_native(device))
        return;

    auto *typeInfo = vku::find_pnext_mut<VkSemaphoreTypeCreateInfo>(
        createInfo.pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);

    if (typeInfo && typeInfo->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
        uint64_t *initVal = new uint64_t(typeInfo->initialValue);
        pUserData = initVal;

        vku::unlink_pnext(createInfo.pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        auto *export_info = vku::find_pnext<VkExportSemaphoreCreateInfo>(
            createInfo.pNext, VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO);
        // No export handle types were requested. Some older native drivers
        // reject this no-op structure even for an ordinary binary semaphore.
        if (export_info && !export_info->handleTypes)
            vku::unlink_pnext(createInfo.pNext, VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO);
        LOG_OPT_DEBUG("TimelineSemaphore: unlinked VkSemaphoreTypeCreateInfo, emulating timeline "
                      "semaphore initialValue=%" PRIu64,
                      *initVal);
    }
}

void TimelineSemaphoreModule::on_post_create_semaphore(VkDevice device,
                                                       const VkSemaphoreCreateInfo *,
                                                       VkResult result, VkSemaphore semaphore,
                                                       void *pUserData) {
    std::unique_ptr<uint64_t> initial(static_cast<uint64_t *>(pUserData));
    if (result != VK_SUCCESS || semaphore == VK_NULL_HANDLE)
        return;
    if (!initial) {
        if(vkfix::TimelineScheduler::get().active(device))vkfix::TimelineScheduler::get().add_binary(device,semaphore);
        return;
    }
    if (vkfix::TimelineScheduler::get().active(device)) {
        vkfix::TimelineScheduler::get().add(device, semaphore, *initial);
        return;
    }
    auto state = std::make_shared<TimelineSemaphoreState>();
    state->device = device;
    state->counter.store(*initial);
    std::unique_lock<std::shared_mutex> lock(m_table_rw_mutex);
    auto inserted =
        m_timeline_semaphores.emplace(semaphore_key(device, semaphore), std::move(state));
    if (inserted.second)
        m_active_timeline_count.fetch_add(1, std::memory_order_relaxed);
}

void TimelineSemaphoreModule::on_destroy_semaphore(VkDevice device, VkSemaphore semaphore) {
    vkfix::TimelineScheduler::get().remove(device, semaphore);

    std::unique_lock<std::shared_mutex> lock(m_table_rw_mutex);
    if (m_timeline_semaphores.erase(semaphore_key(device, semaphore)) > 0) {
        m_active_timeline_count.fetch_sub(1, std::memory_order_relaxed);
    }
}

VkFence TimelineSemaphoreModule::acquire_internal_fence(VkDevice device, VkResult &outResult) {
    {
        std::lock_guard<std::mutex> lock(m_fence_pool_mutex);
        for (auto it = m_fence_pool.begin(); it != m_fence_pool.end(); ++it) {
            if (it->device == device) {
                VkFence f = it->fence;
                m_fence_pool.erase(it);
                outResult = VK_SUCCESS;
                return f;
            }
        }
    }

    const auto &dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkCreateFence real_cf = dt.CreateFence;
    if (!real_cf) {
        real_cf = (PFN_vkCreateFence)get_real_proc(get_last_instance(), device, "vkCreateFence");
    }
    VkFence internalFence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    outResult =
        real_cf ? real_cf(device, &fci, nullptr, &internalFence) : VK_ERROR_INITIALIZATION_FAILED;
    return outResult == VK_SUCCESS ? internalFence : VK_NULL_HANDLE;
}

void TimelineSemaphoreModule::release_internal_fence(VkDevice device, VkFence fence) {
    if (!device || !fence)
        return;
    auto reset = (PFN_vkResetFences)get_real_proc(get_last_instance(), device, "vkResetFences");
    if (!reset || reset(device, 1, &fence) != VK_SUCCESS) {
        auto destroy =
            (PFN_vkDestroyFence)get_real_proc(get_last_instance(), device, "vkDestroyFence");
        if (destroy)
            destroy(device, fence, nullptr);
        return;
    }
    std::lock_guard<std::mutex> lock(m_fence_pool_mutex);
    m_fence_pool.push_back({device, fence});
}

TimelineSemaphoreModule::FenceHolder::~FenceHolder() {
    if (isInternal && fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        if (module) {
            module->release_internal_fence(device, fence);
        } else {
            const auto &dt = LayerManager::get().get_dispatch_table(device);
            PFN_vkDestroyFence real_df = dt.DestroyFence;
            if (!real_df) {
                real_df = (PFN_vkDestroyFence)get_real_proc(get_last_instance(), device,
                                                            "vkDestroyFence");
            }
            if (real_df)
                real_df(device, fence, nullptr);
        }
    }
}

VkResult
TimelineSemaphoreModule::check_pending_signals(std::shared_ptr<TimelineSemaphoreState> &state) {
    if (!state)
        return VK_SUCCESS;
    std::lock_guard<std::mutex> lock(state->state_mutex);
    if (state->error.load() != VK_SUCCESS)
        return state->error.load();
    bool updated = false;
    for (auto it = state->pendingSignals.begin(); it != state->pendingSignals.end();) {
        auto holder = it->fenceHolder;
        auto status = (PFN_vkGetFenceStatus)get_real_proc(get_last_instance(), holder->device,
                                                          "vkGetFenceStatus");
        VkResult result =
            status ? status(holder->device, holder->fence) : VK_ERROR_INITIALIZATION_FAILED;
        if (result == VK_SUCCESS) {
            auto current = state->counter.load();
            while (current < it->targetValue &&
                   !state->counter.compare_exchange_weak(current, it->targetValue)) {
            }
            it = state->pendingSignals.erase(it);
            updated = true;
        } else if (result == VK_NOT_READY)
            ++it;
        else {
            state->error.store(result);
            m_global_cv.notify_all();
            return result;
        }
    }
    if (updated)
        m_global_cv.notify_all();
    return VK_SUCCESS;
}

bool TimelineSemaphoreModule::is_timeline_semaphore(VkDevice device, VkSemaphore semaphore) {
    if (vkfix::TimelineScheduler::get().contains(device, semaphore)) return true;

    if (semaphore == VK_NULL_HANDLE)
        return false;
    if (m_active_timeline_count.load(std::memory_order_relaxed) == 0)
        return false;
    std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
    return m_timeline_semaphores.find(semaphore_key(device, semaphore)) !=
           m_timeline_semaphores.end();
}

void TimelineSemaphoreModule::on_queue_wait_idle(VkQueue queue) {
    if (m_active_timeline_count.load(std::memory_order_relaxed) == 0)
        return;
    VkDevice device = LayerManager::get().get_device_for_queue(queue);
    std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
    for (auto &entry : m_timeline_semaphores) {
        auto &state = entry.second;
        if (state->device != device)
            continue;
        std::lock_guard<std::mutex> state_lock(state->state_mutex);
        for (auto it = state->pendingSignals.begin(); it != state->pendingSignals.end();) {
            if (it->queue != queue) {
                ++it;
                continue;
            }
            auto current = state->counter.load();
            while (current < it->targetValue &&
                   !state->counter.compare_exchange_weak(current, it->targetValue)) {
            }
            it = state->pendingSignals.erase(it);
        }
    }
    m_global_cv.notify_all();
}

void TimelineSemaphoreModule::on_device_wait_idle(VkDevice device) {
    if (m_active_timeline_count.load(std::memory_order_relaxed) == 0)
        return;
    std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
    for (auto &entry : m_timeline_semaphores) {
        auto &state = entry.second;
        if (state->device != device)
            continue;
        std::lock_guard<std::mutex> state_lock(state->state_mutex);
        for (auto &signal : state->pendingSignals) {
            auto current = state->counter.load();
            while (current < signal.targetValue &&
                   !state->counter.compare_exchange_weak(current, signal.targetValue)) {
            }
        }
        state->pendingSignals.clear();
    }
    m_global_cv.notify_all();
}

bool TimelineSemaphoreModule::on_get_semaphore_counter_value(VkDevice device, VkSemaphore semaphore,
                                                             uint64_t *pValue,
                                                             VkResult &outResult) {
    if (vkfix::TimelineScheduler::get().counter(device, semaphore, pValue, outResult)) return true;

    if (!pValue) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    std::shared_ptr<TimelineSemaphoreState> state;
    {
        std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
        auto it = m_timeline_semaphores.find(semaphore_key(device, semaphore));
        if (it != m_timeline_semaphores.end()) {
            state = it->second;
        }
    }

    if (state) {
        outResult = check_pending_signals(state);
        if (outResult == VK_SUCCESS)
            *pValue = state->counter.load(std::memory_order_acquire);
        return true;
    }
    return false;
}

bool TimelineSemaphoreModule::on_wait_semaphores(VkDevice device, const VkSemaphoreWaitInfo *info,
                                                 uint64_t timeout, VkResult &outResult) {
    if (vkfix::TimelineScheduler::get().wait(device, info, timeout, outResult)) return true;

    if (!info || !info->semaphoreCount) {
        outResult = VK_SUCCESS;
        return true;
    }
    std::vector<std::shared_ptr<TimelineSemaphoreState>> states(info->semaphoreCount);
    bool emulated = false, has_native = false;
    {
        std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
        for (uint32_t i = 0; i < info->semaphoreCount; ++i) {
            auto it = m_timeline_semaphores.find(semaphore_key(device, info->pSemaphores[i]));
            if (it != m_timeline_semaphores.end()) {
                states[i] = it->second;
                emulated = true;
            } else
                has_native = true;
        }
    }
    if (!emulated)
        return false;
    auto native_counter = (PFN_vkGetSemaphoreCounterValue)get_real_proc(
        get_last_instance(), device, "vkGetSemaphoreCounterValue");
    if (!native_counter)
        native_counter = (PFN_vkGetSemaphoreCounterValue)get_real_proc(
            get_last_instance(), device, "vkGetSemaphoreCounterValueKHR");
    if (has_native && !native_counter) {
        outResult = VK_ERROR_FEATURE_NOT_PRESENT;
        return true;
    }
    const auto started = std::chrono::steady_clock::now();
    const bool any = info->flags & VK_SEMAPHORE_WAIT_ANY_BIT;
    while (true) {
        bool satisfied = !any;
        for (uint32_t i = 0; i < info->semaphoreCount; ++i) {
            uint64_t value = 0;
            VkResult result = VK_SUCCESS;
            if (states[i]) {
                result = check_pending_signals(states[i]);
                value = states[i]->counter.load(std::memory_order_acquire);
            } else
                result = native_counter(device, info->pSemaphores[i], &value);
            if (result != VK_SUCCESS) {
                outResult = result;
                return true;
            }
            if (any)
                satisfied |= value >= info->pValues[i];
            else
                satisfied &= value >= info->pValues[i];
        }
        if (satisfied) {
            outResult = VK_SUCCESS;
            return true;
        }
        const auto elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - started)
                                          .count());
        if (timeout != UINT64_MAX && elapsed >= timeout) {
            outResult = VK_TIMEOUT;
            return true;
        }
        auto slice =
            timeout == UINT64_MAX ? 1000000ULL : std::min<uint64_t>(timeout - elapsed, 1000000ULL);
        std::unique_lock<std::mutex> lock(m_wait_mutex);
        m_global_cv.wait_for(lock, std::chrono::nanoseconds(slice));
    }
}

bool TimelineSemaphoreModule::on_signal_semaphore(VkDevice device,
                                                  const VkSemaphoreSignalInfo *pSignalInfo,
                                                  VkResult &outResult) {
    if (vkfix::TimelineScheduler::get().signal(device, pSignalInfo, outResult)) return true;

    if (!pSignalInfo) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    if (m_active_timeline_count.load(std::memory_order_relaxed) == 0) {
        return false;
    }

    std::shared_ptr<TimelineSemaphoreState> state;
    {
        std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
        auto it = m_timeline_semaphores.find(semaphore_key(device, pSignalInfo->semaphore));
        if (it != m_timeline_semaphores.end()) {
            state = it->second;
        }
    }

    if (state) {
        if (state->error.load() != VK_SUCCESS) {
            outResult = state->error.load();
            return true;
        }
        auto current = state->counter.load();
        while (current < pSignalInfo->value &&
               !state->counter.compare_exchange_weak(current, pSignalInfo->value)) {
        }
        m_global_cv.notify_all();
        outResult = VK_SUCCESS;
        return true;
    }
    return false;
}

bool TimelineSemaphoreModule::on_queue_submit(VkQueue queue, uint32_t count,
                                              const VkSubmitInfo *submits, VkFence fence,
                                              VkResult &outResult) {
    VkDevice device = LayerManager::get().get_device_for_queue(queue);
    if (is_device_native(device) || !count || !submits)
        return false;
    bool has_timeline = false;
    for (uint32_t s = 0; s < count; ++s) {
        has_timeline |=
            vku::has_pnext(submits[s].pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        for (uint32_t i = 0; i < submits[s].waitSemaphoreCount; ++i)
            has_timeline |= is_timeline_semaphore(device, submits[s].pWaitSemaphores[i]);
        for (uint32_t i = 0; i < submits[s].signalSemaphoreCount; ++i)
            has_timeline |= is_timeline_semaphore(device, submits[s].pSignalSemaphores[i]);
    }
    if (!has_timeline)
        return false;
    auto submit = (PFN_vkQueueSubmit)get_real_proc(get_last_instance(), device, "vkQueueSubmit");
    if (!submit) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }
    struct Signal {
        VkSemaphore semaphore;
        uint64_t value;
    };
    struct Clean {
        vkfix::InputPNextCopy chain;
        std::vector<VkSemaphore> waits, signals;
        std::vector<VkPipelineStageFlags> stages;
        std::vector<uint64_t> wait_values, signal_values;
        std::vector<uint32_t> wait_devices, signal_devices;
    };
    std::vector<Signal> signals;
    std::vector<Clean> storage(count);
    std::vector<VkSubmitInfo> cleaned(count);
    for (uint32_t s = 0; s < count; ++s) {
        const auto &original = submits[s];
        auto &copy = cleaned[s];
        auto &data = storage[s];
        outResult = data.chain.copy(original.pNext, vkfix::InputChainKind::Submit);
        if (outResult != VK_SUCCESS)
            return true;
        copy = original;
        copy.pNext = data.chain.head();
        auto timeline = vku::find_pnext_mut<VkTimelineSemaphoreSubmitInfo>(
            copy.pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        auto group = vku::find_pnext_mut<VkDeviceGroupSubmitInfo>(
            copy.pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO);
        for (uint32_t i = 0; i < original.waitSemaphoreCount; ++i) {
            auto semaphore = original.pWaitSemaphores[i];
            uint64_t value = timeline && i < timeline->waitSemaphoreValueCount
                                 ? timeline->pWaitSemaphoreValues[i]
                                 : 0;
            if (is_timeline_semaphore(device, semaphore)) {
                VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                wait.semaphoreCount = 1;
                wait.pSemaphores = &semaphore;
                wait.pValues = &value;
                // Still a synchronous dependency wait. Replaced by the queued
                // scheduler in the next implementation stage.
                on_wait_semaphores(device, &wait, UINT64_MAX, outResult);
                if (outResult != VK_SUCCESS)
                    return true;
            } else {
                data.waits.push_back(semaphore);
                data.stages.push_back(original.pWaitDstStageMask[i]);
                data.wait_values.push_back(value);
                if (group && group->waitSemaphoreCount)
                    data.wait_devices.push_back(group->pWaitSemaphoreDeviceIndices[i]);
            }
        }
        for (uint32_t i = 0; i < original.signalSemaphoreCount; ++i) {
            auto semaphore = original.pSignalSemaphores[i];
            uint64_t value = timeline && i < timeline->signalSemaphoreValueCount
                                 ? timeline->pSignalSemaphoreValues[i]
                                 : 0;
            if (is_timeline_semaphore(device, semaphore))
                signals.push_back({semaphore, value});
            else {
                data.signals.push_back(semaphore);
                data.signal_values.push_back(value);
                if (group && group->signalSemaphoreCount)
                    data.signal_devices.push_back(group->pSignalSemaphoreDeviceIndices[i]);
            }
        }
        copy.waitSemaphoreCount = uint32_t(data.waits.size());
        copy.pWaitSemaphores = data.waits.data();
        copy.pWaitDstStageMask = data.stages.data();
        copy.signalSemaphoreCount = uint32_t(data.signals.size());
        copy.pSignalSemaphores = data.signals.data();
        if (timeline) {
            timeline->waitSemaphoreValueCount = copy.waitSemaphoreCount;
            timeline->pWaitSemaphoreValues = data.wait_values.data();
            timeline->signalSemaphoreValueCount = copy.signalSemaphoreCount;
            timeline->pSignalSemaphoreValues = data.signal_values.data();
            // Keep aligned native timeline values. Removing this structure
            // would silently change a native timeline operation to value zero.
            if (std::none_of(data.wait_values.begin(), data.wait_values.end(),
                             [](uint64_t v) { return v != 0; }) &&
                std::none_of(data.signal_values.begin(), data.signal_values.end(),
                             [](uint64_t v) { return v != 0; }))
                vku::unlink_pnext(copy.pNext, timeline->sType);
        }
        if (group) {
            group->waitSemaphoreCount = uint32_t(data.wait_devices.size());
            group->pWaitSemaphoreDeviceIndices = data.wait_devices.data();
            group->signalSemaphoreCount = uint32_t(data.signal_devices.size());
            group->pSignalSemaphoreDeviceIndices = data.signal_devices.data();
        }
    }
    std::shared_ptr<FenceHolder> completion;
    VkFence native_fence = fence;
    if (!signals.empty()) {
        completion = std::make_shared<FenceHolder>();
        completion->module = this;
        completion->device = device;
        completion->isInternal = true;
        completion->fence = acquire_internal_fence(device, outResult);
        if (completion->fence == VK_NULL_HANDLE)
            return true;
        native_fence = completion->fence;
    }
    outResult = submit(queue, count, cleaned.data(), native_fence);
    if (outResult != VK_SUCCESS)
        return true;
    for (auto &signal : signals) {
        std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
        auto it = m_timeline_semaphores.find(semaphore_key(device, signal.semaphore));
        if (it != m_timeline_semaphores.end()) {
            std::lock_guard<std::mutex> state_lock(it->second->state_mutex);
            it->second->pendingSignals.push_back({signal.value, completion, queue});
        }
    }
    if (completion && fence != VK_NULL_HANDLE) {
        // A private completion fence cannot be invalidated by the application's
        // later fence reset or destruction. Its signal precedes this app fence.
        auto result = submit(queue, 0, nullptr, fence);
        if (result != VK_SUCCESS) {
            // Work is already submitted; OOM can no longer mean no side effects.
            outResult = VK_ERROR_DEVICE_LOST;
            std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
            for (auto &entry : m_timeline_semaphores)
                if (entry.second->device == device)
                    entry.second->error.store(outResult);
        }
    }
    return true;
}

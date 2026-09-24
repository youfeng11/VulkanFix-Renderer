#include "timeline_semaphore.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include <chrono>
#include <thread>
#include <algorithm>

REGISTER_LAYER_MODULE(TimelineSemaphoreModule);

TimelineSemaphoreModule::TimelineSemaphoreModule()
    : ExtensionModuleBase(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_SPEC_VERSION, VK_API_VERSION_1_2) {
    LOGI("TimelineSemaphoreModule initialized");
}

void TimelineSemaphoreModule::on_pre_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pProperties) return;

    pUserData = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES);
}

void TimelineSemaphoreModule::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties || !pUserData) return;

    auto* p = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreProperties*>(pUserData);
    p->maxTimelineSemaphoreValueDifference = ~0ULL;
    vku::relink_pnext(pProperties->pNext, pUserData);
}

void TimelineSemaphoreModule::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    pUserData = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
}

void TimelineSemaphoreModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures || !pUserData) return;

    auto* f = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(pUserData);
    f->timelineSemaphore = VK_TRUE;
    vku::relink_pnext(pFeatures->pNext, pUserData);
}

bool TimelineSemaphoreModule::query_native_support(VkPhysicalDevice physDev) {
    const char* force_emu = getenv("FORCE_EMULATE_TIMELINE_SEMAPHORE");
    if (force_emu && (strcmp(force_emu, "1") == 0 || strcasecmp(force_emu, "true") == 0)) {
        LOGI("FORCE_EMULATE_TIMELINE_SEMAPHORE set, enabling emulation for physical device %p", physDev);
        return false;
    }

    PFN_vkGetPhysicalDeviceProperties real_props =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (!real_props) return false;
    VkPhysicalDeviceProperties props{};
    real_props(physDev, &props);

    PFN_vkGetPhysicalDeviceFeatures2 real_feat2 =
        (PFN_vkGetPhysicalDeviceFeatures2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2");
    if (!real_feat2) {
        real_feat2 = (PFN_vkGetPhysicalDeviceFeatures2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2KHR");
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
        (PFN_vkEnumerateDeviceExtensionProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    if (real_enum) {
        uint32_t count = 0;
        if (real_enum(physDev, NULL, &count, NULL) == VK_SUCCESS && count > 0) {
            std::vector<VkExtensionProperties> exts(count);
            if (real_enum(physDev, NULL, &count, exts.data()) == VK_SUCCESS) {
                bool hasExt = false;
                for (const auto& e : exts) {
                    if (strcmp(e.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
                        hasExt = true;
                        break;
                    }
                }
                if (hasExt && real_feat2) {
                    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR tsFeat{};
                    tsFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
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
    std::lock_guard<std::mutex> lock(m_ext_mutex);
    auto it = m_phys_native.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native.end()) {
        return it->second;
    }
    bool native = query_native_support(physDev);
    m_phys_native[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("[VK_KHR_timeline_semaphore] Device %p lacks native timelineSemaphore feature support, enabling emulation!", physDev);
    } else {
        LOGI("[VK_KHR_timeline_semaphore] Device %p natively supports timelineSemaphore feature", physDev);
    }
    return native;
}

bool TimelineSemaphoreModule::is_device_native(VkDevice device) {
    if (device == VK_NULL_HANDLE) return false;
    std::lock_guard<std::mutex> lock(m_ext_mutex);
    auto it = m_device_native.find((uint64_t)(uintptr_t)device);
    if (it != m_device_native.end()) {
        return it->second;
    }
    return false;
}

void TimelineSemaphoreModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (!pCreateInfo) return;

    bool physNative = is_phys_device_native(physicalDevice);

    bool appRequestedInExts = false;
    for (const char* ext : enabledExtensions) {
        if (strcmp(ext, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
            appRequestedInExts = true;
            break;
        }
    }

    auto* v12Feat = vku::find_pnext_mut<VkPhysicalDeviceVulkan12Features>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    bool appRequestedInV12 = (v12Feat && v12Feat->timelineSemaphore);

    auto* tsFeat = vku::find_pnext_mut<VkPhysicalDeviceTimelineSemaphoreFeaturesKHR>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR);
    bool appRequestedInTsFeat = (tsFeat && tsFeat->timelineSemaphore);

    PFN_vkGetPhysicalDeviceProperties real_props =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
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
        for (auto it = enabledExtensions.begin(); it != enabledExtensions.end(); ) {
            if (strcmp(*it, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
                it = enabledExtensions.erase(it);
                LOGI("TimelineSemaphore: stripped %s from enabledExtensions", VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
            } else {
                ++it;
            }
        }
        vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR);
    }

    pUserData = reinterpret_cast<void*>(static_cast<uintptr_t>(deviceCanBeNative ? 1 : 0));
}

void TimelineSemaphoreModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool isNative = (reinterpret_cast<uintptr_t>(pUserData) == 1);
        std::lock_guard<std::mutex> lock(m_ext_mutex);
        m_device_native[(uint64_t)(uintptr_t)device] = isNative;
        LOGI("TimelineSemaphore: device %p native=%d (emulation=%s)",
             device, isNative ? 1 : 0, isNative ? "OFF" : "ON");
    }
}

void TimelineSemaphoreModule::on_destroy_device(VkDevice device) {
    ExtensionModuleBase::on_destroy_device(device);
    std::lock_guard<std::mutex> lock(m_fence_pool_mutex);
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkDestroyFence real_df = dt.DestroyFence;
    if (!real_df) {
        real_df = (PFN_vkDestroyFence) get_real_proc(get_last_instance(), device, "vkDestroyFence");
    }
    for (auto it = m_fence_pool.begin(); it != m_fence_pool.end(); ) {
        if (it->device == device) {
            if (real_df && it->fence != VK_NULL_HANDLE) {
                real_df(device, it->fence, nullptr);
            }
            it = m_fence_pool.erase(it);
        } else {
            ++it;
        }
    }
}

void TimelineSemaphoreModule::on_pre_create_semaphore(
    VkDevice device,
    VkSemaphoreCreateInfo& createInfo,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_device_native(device)) return;

    auto* typeInfo = vku::find_pnext_mut<VkSemaphoreTypeCreateInfo>(
        createInfo.pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);

    if (typeInfo && typeInfo->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
        uint64_t* initVal = new uint64_t(typeInfo->initialValue);
        pUserData = initVal;

        vku::unlink_pnext(createInfo.pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        LOG_OPT_DEBUG("TimelineSemaphore: unlinked VkSemaphoreTypeCreateInfo, emulating timeline semaphore initialValue=%" PRIu64, *initVal);
    }
}

void TimelineSemaphoreModule::on_post_create_semaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    VkResult result,
    VkSemaphore semaphore,
    void* pUserData
) {
    if (result == VK_SUCCESS && semaphore != VK_NULL_HANDLE && pUserData) {
        uint64_t initVal = *reinterpret_cast<uint64_t*>(pUserData);
        delete reinterpret_cast<uint64_t*>(pUserData);

        auto state = std::make_shared<TimelineSemaphoreState>();
        state->counter.store(initVal);

        std::lock_guard<std::mutex> lock(m_semaphore_mutex);
        m_timeline_semaphores[(uint64_t)(uintptr_t)semaphore] = state;
        m_active_timeline_count.fetch_add(1, std::memory_order_relaxed);
        LOG_OPT_DEBUG("TimelineSemaphore: registered emulated timeline semaphore %p with initial value %" PRIu64,
                      VK_HANDLE(semaphore), initVal);
    }
}

void TimelineSemaphoreModule::on_destroy_semaphore(
    VkDevice device,
    VkSemaphore semaphore
) {
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    if (m_timeline_semaphores.erase((uint64_t)(uintptr_t)semaphore) > 0) {
        m_active_timeline_count.fetch_sub(1, std::memory_order_relaxed);
    }
}

VkFence TimelineSemaphoreModule::acquire_internal_fence(VkDevice device) {
    {
        std::lock_guard<std::mutex> lock(m_fence_pool_mutex);
        for (auto it = m_fence_pool.begin(); it != m_fence_pool.end(); ++it) {
            if (it->device == device) {
                VkFence f = it->fence;
                m_fence_pool.erase(it);
                return f;
            }
        }
    }

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkCreateFence real_cf = dt.CreateFence;
    if (!real_cf) {
        real_cf = (PFN_vkCreateFence) get_real_proc(get_last_instance(), device, "vkCreateFence");
    }
    VkFence internalFence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (real_cf && real_cf(device, &fci, nullptr, &internalFence) == VK_SUCCESS) {
        return internalFence;
    }
    return VK_NULL_HANDLE;
}

void TimelineSemaphoreModule::release_internal_fence(VkDevice device, VkFence fence) {
    if (device == VK_NULL_HANDLE || fence == VK_NULL_HANDLE) return;
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.ResetFences) {
        dt.ResetFences(device, 1, &fence);
    } else {
        PFN_vkResetFences real_rf = (PFN_vkResetFences) get_real_proc(get_last_instance(), device, "vkResetFences");
        if (real_rf) real_rf(device, 1, &fence);
    }
    std::lock_guard<std::mutex> lock(m_fence_pool_mutex);
    m_fence_pool.push_back({device, fence});
}

TimelineSemaphoreModule::FenceHolder::~FenceHolder() {
    if (isInternal && fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        if (module) {
            module->release_internal_fence(device, fence);
        } else {
            const auto& dt = LayerManager::get().get_dispatch_table(device);
            PFN_vkDestroyFence real_df = dt.DestroyFence;
            if (!real_df) {
                real_df = (PFN_vkDestroyFence) get_real_proc(get_last_instance(), device, "vkDestroyFence");
            }
            if (real_df) real_df(device, fence, nullptr);
        }
    }
}

void TimelineSemaphoreModule::check_pending_signals_locked(std::shared_ptr<TimelineSemaphoreState>& state) {
    if (!state) return;
    bool anyUpdated = false;
    auto it = state->pendingSignals.begin();
    while (it != state->pendingSignals.end()) {
        auto fh = it->fenceHolder;
        if (fh && fh->fence != VK_NULL_HANDLE) {
            const auto& dt = LayerManager::get().get_dispatch_table(fh->device);
            PFN_vkGetFenceStatus real_gfs = dt.GetFenceStatus;
            if (!real_gfs) {
                real_gfs = (PFN_vkGetFenceStatus) get_real_proc(get_last_instance(), fh->device, "vkGetFenceStatus");
            }
            if (real_gfs && real_gfs(fh->device, fh->fence) == VK_SUCCESS) {
                if (it->targetValue > state->counter.load(std::memory_order_relaxed)) {
                    state->counter.store(it->targetValue, std::memory_order_relaxed);
                    anyUpdated = true;
                }
                it = state->pendingSignals.erase(it);
                continue;
            }
        }
        ++it;
    }
    if (anyUpdated) {
        m_global_cv.notify_all();
    }
}

bool TimelineSemaphoreModule::is_timeline_semaphore(VkSemaphore semaphore) {
    if (semaphore == VK_NULL_HANDLE) return false;
    if (m_active_timeline_count.load(std::memory_order_relaxed) == 0) return false;
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    return m_timeline_semaphores.find((uint64_t)(uintptr_t)semaphore) != m_timeline_semaphores.end();
}

void TimelineSemaphoreModule::on_queue_wait_idle(VkQueue queue) {
    if (m_active_timeline_count.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    for (auto& pair : m_timeline_semaphores) {
        auto& state = pair.second;
        for (auto& ps : state->pendingSignals) {
            if (ps.targetValue > state->counter.load(std::memory_order_relaxed)) {
                state->counter.store(ps.targetValue, std::memory_order_relaxed);
            }
        }
        state->pendingSignals.clear();
    }
    m_global_cv.notify_all();
}

void TimelineSemaphoreModule::on_device_wait_idle(VkDevice device) {
    if (m_active_timeline_count.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    for (auto& pair : m_timeline_semaphores) {
        auto& state = pair.second;
        for (auto& ps : state->pendingSignals) {
            if (ps.targetValue > state->counter.load(std::memory_order_relaxed)) {
                state->counter.store(ps.targetValue, std::memory_order_relaxed);
            }
        }
        state->pendingSignals.clear();
    }
    m_global_cv.notify_all();
}

bool TimelineSemaphoreModule::on_get_semaphore_counter_value(
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t* pValue,
    VkResult& outResult
) {
    if (!pValue) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    std::shared_ptr<TimelineSemaphoreState> state;
    {
        std::lock_guard<std::mutex> lock(m_semaphore_mutex);
        auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)semaphore);
        if (it != m_timeline_semaphores.end()) {
            state = it->second;
            check_pending_signals_locked(state);
        }
    }

    if (state) {
        *pValue = state->counter.load();
        outResult = VK_SUCCESS;
        return true;
    }
    return false;
}

bool TimelineSemaphoreModule::on_wait_semaphores(
    VkDevice device,
    const VkSemaphoreWaitInfo* pWaitInfo,
    uint64_t timeout,
    VkResult& outResult
) {
    if (!pWaitInfo || pWaitInfo->semaphoreCount == 0) {
        outResult = VK_SUCCESS;
        return true;
    }

    if (m_active_timeline_count.load(std::memory_order_relaxed) == 0) {
        return false;
    }

    std::vector<std::shared_ptr<TimelineSemaphoreState>> states(pWaitInfo->semaphoreCount);
    bool has_emulated = false;

    {
        std::lock_guard<std::mutex> lock(m_semaphore_mutex);
        for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
            auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)pWaitInfo->pSemaphores[i]);
            if (it != m_timeline_semaphores.end()) {
                states[i] = it->second;
                has_emulated = true;
                check_pending_signals_locked(states[i]);
            }
        }
    }

    if (!has_emulated) return false;

    bool waitAny = (pWaitInfo->flags & VK_SEMAPHORE_WAIT_ANY_BIT);
    auto startTime = std::chrono::steady_clock::now();

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkWaitForFences real_wff = dt.WaitForFences;
    if (!real_wff) {
        real_wff = (PFN_vkWaitForFences) get_real_proc(get_last_instance(), device, "vkWaitForFences");
    }

    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_semaphore_mutex);
            for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
                if (states[i]) {
                    check_pending_signals_locked(states[i]);
                }
            }
        }

        bool satisfied = waitAny ? false : true;
        for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
            uint64_t current = states[i] ? states[i]->counter.load(std::memory_order_relaxed) : pWaitInfo->pValues[i];
            bool met = (current >= pWaitInfo->pValues[i]);
            if (waitAny && met) {
                satisfied = true;
                break;
            } else if (!waitAny && !met) {
                satisfied = false;
                break;
            }
        }

        if (satisfied) {
            outResult = VK_SUCCESS;
            return true;
        }

        if (timeout == 0) {
            outResult = VK_TIMEOUT;
            return true;
        }

        uint64_t elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (timeout != UINT64_MAX && elapsedNs >= timeout) {
            outResult = VK_TIMEOUT;
            return true;
        }

        uint64_t remainingTimeout = (timeout == UINT64_MAX) ? UINT64_MAX : (timeout - elapsedNs);

        std::shared_ptr<FenceHolder> fenceToWait = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_semaphore_mutex);
            for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
                if (states[i] && states[i]->counter.load(std::memory_order_relaxed) < pWaitInfo->pValues[i]) {
                    for (auto& ps : states[i]->pendingSignals) {
                        if (ps.targetValue >= pWaitInfo->pValues[i]) {
                            fenceToWait = ps.fenceHolder;
                            break;
                        }
                    }
                    if (fenceToWait) break;
                    if (!states[i]->pendingSignals.empty()) {
                        fenceToWait = states[i]->pendingSignals.back().fenceHolder;
                        break;
                    }
                }
            }
        }

        if (fenceToWait && fenceToWait->fence != VK_NULL_HANDLE && real_wff) {
            uint64_t sliceTimeout = std::min<uint64_t>(remainingTimeout, 20000000ULL); // 20ms
            VkResult wr = real_wff(fenceToWait->device, 1, &fenceToWait->fence, VK_TRUE, sliceTimeout);
            if (wr == VK_SUCCESS || wr == VK_TIMEOUT) {
                continue;
            } else {
                outResult = wr;
                return true;
            }
        } else {
            std::unique_lock<std::mutex> lock(m_semaphore_mutex);
            uint64_t waitSliceMs = std::min<uint64_t>(remainingTimeout / 1000000ULL, 2ULL);
            if (waitSliceMs == 0 && remainingTimeout > 0) waitSliceMs = 1;
            m_global_cv.wait_for(lock, std::chrono::milliseconds(waitSliceMs));
        }
    }
}

bool TimelineSemaphoreModule::on_signal_semaphore(
    VkDevice device,
    const VkSemaphoreSignalInfo* pSignalInfo,
    VkResult& outResult
) {
    if (!pSignalInfo) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    if (m_active_timeline_count.load(std::memory_order_relaxed) == 0) {
        return false;
    }

    std::shared_ptr<TimelineSemaphoreState> state;
    {
        std::lock_guard<std::mutex> lock(m_semaphore_mutex);
        auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)pSignalInfo->semaphore);
        if (it != m_timeline_semaphores.end()) {
            state = it->second;
        }
    }

    if (state) {
        state->counter.store(pSignalInfo->value, std::memory_order_relaxed);
        m_global_cv.notify_all();
        outResult = VK_SUCCESS;
        return true;
    }
    return false;
}

bool TimelineSemaphoreModule::on_queue_submit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence,
    VkResult& outResult
) {
    VkDevice device = LayerManager::get().get_device_for_queue(queue);
    if (is_device_native(device)) return false;

    if (submitCount == 0 || !pSubmits) return false;

    uint32_t activeCount = m_active_timeline_count.load(std::memory_order_relaxed);
    bool has_timeline = false;
    for (uint32_t s = 0; s < submitCount; ++s) {
        if (vku::find_pnext<VkTimelineSemaphoreSubmitInfo>(
                pSubmits[s].pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) != nullptr) {
            has_timeline = true;
            break;
        }
        if (activeCount > 0) {
            const auto& orig = pSubmits[s];
            if (orig.pWaitSemaphores) {
                for (uint32_t i = 0; i < orig.waitSemaphoreCount; ++i) {
                    if (is_timeline_semaphore(orig.pWaitSemaphores[i])) {
                        has_timeline = true;
                        break;
                    }
                }
            }
            if (has_timeline) break;
            if (orig.pSignalSemaphores) {
                for (uint32_t i = 0; i < orig.signalSemaphoreCount; ++i) {
                    if (is_timeline_semaphore(orig.pSignalSemaphores[i])) {
                        has_timeline = true;
                        break;
                    }
                }
            }
            if (has_timeline) break;
        }
    }

    if (!has_timeline) {
        return false;
    }

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkQueueSubmit real_fn = dt.QueueSubmit;
    if (!real_fn) {
        real_fn = (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), device, "vkQueueSubmit");
    }
    if (!real_fn) {
        real_fn = (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueSubmit");
    }
    if (!real_fn) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    struct SignalTarget {
        VkSemaphore semaphore;
        uint64_t targetValue;
    };
    std::vector<SignalTarget> emulatedSignals;

    // Check if any submits need to wait on emulated timeline semaphores
    for (uint32_t s = 0; s < submitCount; ++s) {
        auto* timelineInfo = vku::find_pnext<VkTimelineSemaphoreSubmitInfo>(
            pSubmits[s].pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        if (timelineInfo && timelineInfo->pWaitSemaphoreValues) {
            for (uint32_t i = 0; i < timelineInfo->waitSemaphoreValueCount; ++i) {
                if (i < pSubmits[s].waitSemaphoreCount) {
                    VkSemaphore sem = pSubmits[s].pWaitSemaphores[i];
                    if (is_timeline_semaphore(sem)) {
                        uint64_t targetVal = timelineInfo->pWaitSemaphoreValues[i];
                        VkSemaphoreWaitInfo waitInfo{};
                        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
                        waitInfo.semaphoreCount = 1;
                        waitInfo.pSemaphores = &sem;
                        waitInfo.pValues = &targetVal;
                        VkResult wr = VK_SUCCESS;
                        on_wait_semaphores(device, &waitInfo, UINT64_MAX, wr);
                    }
                }
            }
        }

        if (timelineInfo && timelineInfo->pSignalSemaphoreValues) {
            for (uint32_t i = 0; i < timelineInfo->signalSemaphoreValueCount; ++i) {
                if (i < pSubmits[s].signalSemaphoreCount) {
                    VkSemaphore sem = pSubmits[s].pSignalSemaphores[i];
                    if (is_timeline_semaphore(sem)) {
                        emulatedSignals.push_back({sem, timelineInfo->pSignalSemaphoreValues[i]});
                    }
                }
            }
        }
    }

    std::shared_ptr<FenceHolder> fenceHolder = nullptr;
    VkFence fenceToSubmit = fence;

    if (!emulatedSignals.empty()) {
        if (fence != VK_NULL_HANDLE) {
            fenceHolder = std::make_shared<FenceHolder>();
            fenceHolder->module = this;
            fenceHolder->device = device;
            fenceHolder->fence = fence;
            fenceHolder->isInternal = false;
        } else {
            VkFence internalFence = acquire_internal_fence(device);
            if (internalFence != VK_NULL_HANDLE) {
                fenceHolder = std::make_shared<FenceHolder>();
                fenceHolder->module = this;
                fenceHolder->device = device;
                fenceHolder->fence = internalFence;
                fenceHolder->isInternal = true;
                fenceToSubmit = internalFence;
            }
        }
    }

    struct CleanData {
        std::vector<VkSemaphore> waitSemaphores;
        std::vector<VkPipelineStageFlags> waitDstStageMask;
        std::vector<VkSemaphore> signalSemaphores;
    };
    std::vector<CleanData> cleanData(submitCount);
    std::vector<VkSubmitInfo> cleanSubmits(submitCount);

    for (uint32_t s = 0; s < submitCount; ++s) {
        const auto& orig = pSubmits[s];
        auto& cd = cleanData[s];
        auto& cs = cleanSubmits[s];
        cs = orig;

        if (orig.pWaitSemaphores && orig.waitSemaphoreCount > 0) {
            for (uint32_t i = 0; i < orig.waitSemaphoreCount; ++i) {
                if (!is_timeline_semaphore(orig.pWaitSemaphores[i])) {
                    cd.waitSemaphores.push_back(orig.pWaitSemaphores[i]);
                    if (orig.pWaitDstStageMask) {
                        cd.waitDstStageMask.push_back(orig.pWaitDstStageMask[i]);
                    } else {
                        cd.waitDstStageMask.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                    }
                }
            }
            cs.waitSemaphoreCount = (uint32_t) cd.waitSemaphores.size();
            cs.pWaitSemaphores = cd.waitSemaphores.empty() ? nullptr : cd.waitSemaphores.data();
            cs.pWaitDstStageMask = cd.waitDstStageMask.empty() ? nullptr : cd.waitDstStageMask.data();
        }

        if (orig.pSignalSemaphores && orig.signalSemaphoreCount > 0) {
            for (uint32_t i = 0; i < orig.signalSemaphoreCount; ++i) {
                if (!is_timeline_semaphore(orig.pSignalSemaphores[i])) {
                    cd.signalSemaphores.push_back(orig.pSignalSemaphores[i]);
                }
            }
            cs.signalSemaphoreCount = (uint32_t) cd.signalSemaphores.size();
            cs.pSignalSemaphores = cd.signalSemaphores.empty() ? nullptr : cd.signalSemaphores.data();
        }

        vku::unlink_pnext(cs.pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
    }

    outResult = real_fn(queue, submitCount, cleanSubmits.data(), fenceToSubmit);
    if (outResult == VK_SUCCESS) {
        if (!emulatedSignals.empty()) {
            std::lock_guard<std::mutex> lock(m_semaphore_mutex);
            bool updated = false;
            for (const auto& sig : emulatedSignals) {
                auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)sig.semaphore);
                if (it != m_timeline_semaphores.end()) {
                    if (fenceHolder && fenceHolder->fence != VK_NULL_HANDLE) {
                        it->second->pendingSignals.push_back({sig.targetValue, fenceHolder});
                    } else {
                        if (sig.targetValue > it->second->counter.load(std::memory_order_relaxed)) {
                            it->second->counter.store(sig.targetValue, std::memory_order_relaxed);
                            updated = true;
                        }
                    }
                }
            }
            if (updated) {
                m_global_cv.notify_all();
            }
        }
    }
    return true;
}

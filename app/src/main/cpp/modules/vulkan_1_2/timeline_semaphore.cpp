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

void TimelineSemaphoreModule::on_pre_create_device_custom(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    if (pCreateInfo) {
        vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
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
        LOG_OPT_DEBUG("TimelineSemaphore: registered emulated timeline semaphore %p with initial value %" PRIu64,
                      VK_HANDLE(semaphore), initVal);
    }
}

void TimelineSemaphoreModule::on_destroy_semaphore(
    VkDevice device,
    VkSemaphore semaphore
) {
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    m_timeline_semaphores.erase((uint64_t)(uintptr_t)semaphore);
}

TimelineSemaphoreModule::FenceHolder::~FenceHolder() {
    if (isInternal && fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        PFN_vkDestroyFence real_df =
            (PFN_vkDestroyFence) get_real_proc(get_last_instance(), device, "vkDestroyFence");
        if (real_df) real_df(device, fence, nullptr);
    }
}

void TimelineSemaphoreModule::check_pending_signals_locked(std::shared_ptr<TimelineSemaphoreState>& state) {
    if (!state) return;
    PFN_vkGetFenceStatus real_gfs = nullptr;

    auto it = state->pendingSignals.begin();
    while (it != state->pendingSignals.end()) {
        auto fh = it->fenceHolder;
        if (fh && fh->fence != VK_NULL_HANDLE) {
            if (!real_gfs) {
                real_gfs = (PFN_vkGetFenceStatus) get_real_proc(get_last_instance(), fh->device, "vkGetFenceStatus");
            }
            if (real_gfs && real_gfs(fh->device, fh->fence) == VK_SUCCESS) {
                if (it->targetValue > state->counter.load()) {
                    state->counter.store(it->targetValue);
                    state->cv.notify_all();
                }
                it = state->pendingSignals.erase(it);
                continue;
            }
        }
        ++it;
    }
}

bool TimelineSemaphoreModule::is_timeline_semaphore(VkSemaphore semaphore) {
    if (semaphore == VK_NULL_HANDLE) return false;
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    return m_timeline_semaphores.find((uint64_t)(uintptr_t)semaphore) != m_timeline_semaphores.end();
}

void TimelineSemaphoreModule::on_queue_wait_idle(VkQueue queue) {
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    for (auto& pair : m_timeline_semaphores) {
        auto& state = pair.second;
        for (auto& ps : state->pendingSignals) {
            if (ps.targetValue > state->counter.load()) {
                state->counter.store(ps.targetValue);
            }
        }
        state->pendingSignals.clear();
        state->cv.notify_all();
    }
}

void TimelineSemaphoreModule::on_device_wait_idle(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    for (auto& pair : m_timeline_semaphores) {
        auto& state = pair.second;
        for (auto& ps : state->pendingSignals) {
            if (ps.targetValue > state->counter.load()) {
                state->counter.store(ps.targetValue);
            }
        }
        state->pendingSignals.clear();
        state->cv.notify_all();
    }
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

    PFN_vkWaitForFences real_wff =
        (PFN_vkWaitForFences) get_real_proc(get_last_instance(), device, "vkWaitForFences");

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
            uint64_t current = states[i] ? states[i]->counter.load() : pWaitInfo->pValues[i];
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
                if (states[i] && states[i]->counter.load() < pWaitInfo->pValues[i]) {
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
            uint64_t sliceTimeout = std::min<uint64_t>(remainingTimeout, 50000000ULL); // 50ms
            VkResult wr = real_wff(fenceToWait->device, 1, &fenceToWait->fence, VK_TRUE, sliceTimeout);
            if (wr == VK_SUCCESS || wr == VK_TIMEOUT) {
                continue;
            } else {
                outResult = wr;
                return true;
            }
        } else {
            std::unique_lock<std::mutex> lock(m_semaphore_mutex);
            if (states[0]) {
                states[0]->cv.wait_for(lock, std::chrono::milliseconds(5));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
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

    std::shared_ptr<TimelineSemaphoreState> state;
    {
        std::lock_guard<std::mutex> lock(m_semaphore_mutex);
        auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)pSignalInfo->semaphore);
        if (it != m_timeline_semaphores.end()) {
            state = it->second;
        }
    }

    if (state) {
        state->counter.store(pSignalInfo->value);
        state->cv.notify_all();
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

    PFN_vkQueueSubmit real_fn =
        (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueSubmit");
    if (!real_fn) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    if (submitCount == 0 || !pSubmits) {
        outResult = real_fn(queue, submitCount, pSubmits, fence);
        return true;
    }

    struct SignalTarget {
        VkSemaphore semaphore;
        uint64_t targetValue;
    };
    std::vector<SignalTarget> emulatedSignals;

    for (uint32_t s = 0; s < submitCount; ++s) {
        auto* timelineInfo = vku::find_pnext<VkTimelineSemaphoreSubmitInfo>(
            pSubmits[s].pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
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
            fenceHolder->device = device;
            fenceHolder->fence = fence;
            fenceHolder->isInternal = false;
        } else {
            VkFence internalFence = VK_NULL_HANDLE;
            PFN_vkCreateFence real_cf =
                (PFN_vkCreateFence) get_real_proc(get_last_instance(), device, "vkCreateFence");
            VkFenceCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (real_cf && real_cf(device, &fci, nullptr, &internalFence) == VK_SUCCESS) {
                fenceHolder = std::make_shared<FenceHolder>();
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
            for (const auto& sig : emulatedSignals) {
                auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)sig.semaphore);
                if (it != m_timeline_semaphores.end()) {
                    if (fenceHolder && fenceHolder->fence != VK_NULL_HANDLE) {
                        it->second->pendingSignals.push_back({sig.targetValue, fenceHolder});
                    } else {
                        if (sig.targetValue > it->second->counter.load()) {
                            it->second->counter.store(sig.targetValue);
                            it->second->cv.notify_all();
                        }
                    }
                }
            }
        }
    }
    return true;
}

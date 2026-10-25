#include "host_query_reset.h"
#include "layer_manager.h"
#include "timeline_scheduler.h"
#include "driver_loader.h"
#include "vk_pnext.h"

REGISTER_LAYER_MODULE(HostQueryResetModule);

HostQueryResetModule::HostQueryResetModule()
    : ExtensionModuleBase(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME, VK_EXT_HOST_QUERY_RESET_SPEC_VERSION, VK_API_VERSION_1_2) {
    LOGI("HostQueryResetModule initialized");
}

void HostQueryResetModule::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    pUserData = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
}

void HostQueryResetModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures || !pUserData) return;

    auto* f = reinterpret_cast<VkPhysicalDeviceHostQueryResetFeatures*>(pUserData);
    f->hostQueryReset = VK_TRUE;
    vku::relink_pnext(pFeatures->pNext, pUserData);
}

void HostQueryResetModule::on_pre_create_device_custom(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    if (pCreateInfo) {
        vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
    }
}

bool HostQueryResetModule::on_reset_query_pool(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount
) {
    if (is_device_native(device)) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.ResetQueryPool) {
        dt.ResetQueryPool(device, queryPool, firstQuery, queryCount);
        return true;
    }
    PFN_vkResetQueryPool real_fn =
        (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPool");
    if (!real_fn) {
        real_fn = (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPoolEXT");
    }
    if (real_fn) {
        real_fn(device, queryPool, firstQuery, queryCount);
        return true;
    }

    // Emulate via cached command buffer
    std::lock_guard<std::mutex> lock(m_reset_mutex);
    auto& ctx = m_device_contexts[(uint64_t)(uintptr_t)device];

    if (ctx.pool == VK_NULL_HANDLE) {
        PFN_vkCreateCommandPool real_cp = (PFN_vkCreateCommandPool) get_real_proc(get_last_instance(), device, "vkCreateCommandPool");
        PFN_vkAllocateCommandBuffers real_ac = dt.AllocateCommandBuffers;
        if (!real_ac) {
            real_ac = (PFN_vkAllocateCommandBuffers) get_real_proc(get_last_instance(), device, "vkAllocateCommandBuffers");
        }

        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queueFamilyIndex = 0;
        if (!LayerManager::get().get_device_queue_info(device, queue, queueFamilyIndex)) {
            PFN_vkGetDeviceQueue real_gdq = dt.GetDeviceQueue;
            if (!real_gdq) {
                real_gdq = (PFN_vkGetDeviceQueue) get_real_proc(get_last_instance(), device, "vkGetDeviceQueue");
            }
            if (real_gdq) real_gdq(device, 0, 0, &queue);
        }
        ctx.queue = queue;
        ctx.queueFamilyIndex = queueFamilyIndex;

        if (real_cp && real_ac && ctx.queue) {
            VkCommandPoolCreateInfo poolCI{};
            poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolCI.queueFamilyIndex = queueFamilyIndex;
            if (real_cp(device, &poolCI, nullptr, &ctx.pool) == VK_SUCCESS) {
                VkCommandBufferAllocateInfo allocInfo{};
                allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocInfo.commandPool = ctx.pool;
                allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocInfo.commandBufferCount = 1;
                if (real_ac(device, &allocInfo, &ctx.cmd) != VK_SUCCESS) {
                    ctx.cmd = VK_NULL_HANDLE;
                }
                if (ctx.cmd != VK_NULL_HANDLE) {
                    PFN_vkCreateFence real_cf = dt.CreateFence;
                    if (!real_cf) {
                        real_cf = (PFN_vkCreateFence) get_real_proc(get_last_instance(), device, "vkCreateFence");
                    }
                    if (real_cf) {
                        VkFenceCreateInfo fenceCI{};
                        fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                        if (real_cf(device, &fenceCI, nullptr, &ctx.fence) != VK_SUCCESS) {
                            ctx.fence = VK_NULL_HANDLE;
                        }
                    }
                }
            }
        }
    }

    if (ctx.pool != VK_NULL_HANDLE && ctx.cmd != VK_NULL_HANDLE && ctx.queue != VK_NULL_HANDLE) {
        PFN_vkResetCommandBuffer real_rcb = dt.ResetCommandBuffer;
        if (!real_rcb) {
            real_rcb = (PFN_vkResetCommandBuffer) get_real_proc(get_last_instance(), device, "vkResetCommandBuffer");
        }
        PFN_vkBeginCommandBuffer real_bc = (PFN_vkBeginCommandBuffer) get_real_proc(get_last_instance(), device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer real_ec = (PFN_vkEndCommandBuffer) get_real_proc(get_last_instance(), device, "vkEndCommandBuffer");
        PFN_vkCmdResetQueryPool real_cmd_reset = dt.CmdResetQueryPool;
        if (!real_cmd_reset) {
            real_cmd_reset = (PFN_vkCmdResetQueryPool) get_real_proc(get_last_instance(), device, "vkCmdResetQueryPool");
        }
        PFN_vkQueueSubmit real_qs = dt.QueueSubmit;
        if (!real_qs) {
            real_qs = (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), device, "vkQueueSubmit");
        }
        PFN_vkQueueWaitIdle real_qwi = dt.QueueWaitIdle;
        if (!real_qwi) {
            real_qwi = (PFN_vkQueueWaitIdle) get_real_proc(get_last_instance(), device, "vkQueueWaitIdle");
        }
        PFN_vkResetFences real_rf = dt.ResetFences;
        if (!real_rf) {
            real_rf = (PFN_vkResetFences) get_real_proc(get_last_instance(), device, "vkResetFences");
        }
        PFN_vkWaitForFences real_wf = dt.WaitForFences;
        if (!real_wf) {
            real_wf = (PFN_vkWaitForFences) get_real_proc(get_last_instance(), device, "vkWaitForFences");
        }

        if (real_bc && real_cmd_reset && real_ec && real_qs) {
            if (real_rcb) {
                real_rcb(ctx.cmd, 0);
            }
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

            real_bc(ctx.cmd, &beginInfo);
            real_cmd_reset(ctx.cmd, queryPool, firstQuery, queryCount);
            real_ec(ctx.cmd);

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &ctx.cmd;
            auto submit_reset = [&](VkFence fence) {
                VkResult result=VK_SUCCESS;
                if(vkfix::TimelineScheduler::get().submit_internal(ctx.queue,1,&submitInfo,fence,result))return result;
                return real_qs(ctx.queue,1,&submitInfo,fence);
            };

            if (ctx.fence != VK_NULL_HANDLE && real_rf && real_wf) {
                // Wait only for the reset submission instead of the whole queue
                // (vkQueueWaitIdle also waits for in-flight frames).
                real_rf(device, 1, &ctx.fence);
                if (submit_reset(ctx.fence) == VK_SUCCESS) {
                    real_wf(device, 1, &ctx.fence, VK_TRUE, UINT64_MAX);
                } else if (real_qwi) {
                    LayerManager::get().dispatch_queue_wait_idle(ctx.queue);
                }
            } else {
                submit_reset(VK_NULL_HANDLE);
                if (real_qwi) LayerManager::get().dispatch_queue_wait_idle(ctx.queue);
            }
        }
    }
    return true;
}

void HostQueryResetModule::on_destroy_device(VkDevice device) {
    ExtensionModuleBase::on_destroy_device(device);
    std::lock_guard<std::mutex> lock(m_reset_mutex);
    auto it = m_device_contexts.find((uint64_t)(uintptr_t)device);
    if (it != m_device_contexts.end()) {
        const auto& dt = LayerManager::get().get_dispatch_table(device);
        if (it->second.fence != VK_NULL_HANDLE) {
            PFN_vkDestroyFence real_df = dt.DestroyFence;
            if (!real_df) {
                real_df = (PFN_vkDestroyFence) get_real_proc(get_last_instance(), device, "vkDestroyFence");
            }
            if (real_df) {
                real_df(device, it->second.fence, nullptr);
            }
        }
        if (it->second.pool != VK_NULL_HANDLE) {
            PFN_vkDestroyCommandPool real_dp = dt.DestroyCommandPool;
            if (!real_dp) {
                real_dp = (PFN_vkDestroyCommandPool) get_real_proc(get_last_instance(), device, "vkDestroyCommandPool");
            }
            if (real_dp) {
                real_dp(device, it->second.pool, nullptr);
            }
        }
        m_device_contexts.erase(it);
    }
}

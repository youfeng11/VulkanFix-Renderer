#include "layer_manager.h"
#include "driver_info.h"
#include "vertex_store_emulator.h"
#include "promoted_features.h"
#include "promoted_properties.h"
#include "input_pnext_copy.h"
#include "timeline_scheduler.h"
#include "extension_dependencies.h"
#include "native_instance_state.h"
#include "driver_loader.h"
#include "pipeline_cache_manager.h"
#include "tbdr_barrier_optimizer.h"
#include "renderdoc_manager.h"
#include "shader/shader_rewriter.h"
#include "imageless_framebuffer_manager.h"
#include "layout_compat.h"
#include "vk_pnext.h"

LayerManager& LayerManager::get() {
    static LayerManager s_instance;
    return s_instance;
}

static std::vector<LayerManager::ModuleFactory>& get_registered_factories() {
    static std::vector<LayerManager::ModuleFactory> s_factories;
    return s_factories;
}

void LayerManager::register_module_factory(ModuleFactory factory) {
    get_registered_factories().push_back(std::move(factory));
}

void LayerManager::init_registered_modules() {
    for (const auto& factory : get_registered_factories()) {
        register_module(factory());
    }
    RenderDocManager::get().init();
}

void LayerManager::register_custom_proc(const char* name, PFN_vkVoidFunction proc) {
    if (!name || !proc) return;
    std::unique_lock<std::shared_mutex> lock(m_proc_rw_mutex);
    m_custom_procs[name] = proc;
    m_has_custom_procs.store(true, std::memory_order_release);
    LOGI("LayerManager: registered custom proc '%s' -> %p", name, (void*)proc);
}

PFN_vkVoidFunction LayerManager::get_custom_proc_slow(const char* name) {
    if (!name) return NULL;
    std::shared_lock<std::shared_mutex> lock(m_proc_rw_mutex);
    auto it = m_custom_procs.find(name);
    if (it != m_custom_procs.end()) {
        return it->second;
    }
    return NULL;
}

void LayerManager::register_module(std::unique_ptr<IVulkanLayerModule> module) {
    std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
    LOGI("Registering Vulkan layer module: %s", module->get_name());
    const char* name = module->get_name();
    if (strcmp(name, "VK_EXT_vertex_attribute_divisor") == 0) m_divisor_mod = module.get();
    else if (strcmp(name, "VK_KHR_dynamic_rendering") == 0) m_dyn_rendering_mod = module.get();
    else if (strcmp(name, "VK_KHR_synchronization2") == 0) m_sync2_mod = module.get();
    else if (strcmp(name, "VK_KHR_push_descriptor") == 0) m_push_desc_mod = module.get();
    else if (strcmp(name, "VK_FEATURE_fillModeNonSolid") == 0) m_fill_mode_mod = module.get();
    else if (strcmp(name, "VK_FEATURE_multiDrawIndirect") == 0) m_multi_draw_indirect_mod = module.get();
    else if (strcmp(name, "VK_FEATURE_drawIndirectFirstInstance") == 0) m_draw_indirect_first_instance_mod = module.get();
    else if (strcmp(name, "VK_FEATURE_samplerAnisotropy") == 0) m_sampler_anisotropy_mod = module.get();
    else if (strcmp(name, "VK_KHR_timeline_semaphore") == 0) m_timeline_mod = module.get();
    else if (strcmp(name, "VK_KHR_create_renderpass2") == 0) m_renderpass2_mod = module.get();
    else if (strcmp(name, "VK_KHR_draw_indirect_count") == 0) m_draw_indirect_count_mod = module.get();
    else if (strcmp(name, "VK_KHR_device_group") == 0) m_device_group_mod = module.get();
    else if (strcmp(name, "VK_KHR_swapchain") == 0) m_swapchain_mod = module.get();
    else if (strcmp(name, "VK_KHR_buffer_device_address") == 0) m_bda_mod = module.get();
    else if (strcmp(name, "VK_EXT_host_query_reset") == 0) m_host_query_reset_mod = module.get();
    else if (strcmp(name, "VK_KHR_maintenance1") == 0) m_maintenance_mod = module.get();
    else if (strcmp(name, "VK_KHR_get_memory_requirements2") == 0) m_get_mem_reqs2_mod = module.get();
    else if (strcmp(name, "VK_KHR_descriptor_update_template") == 0) m_desc_template_mod = module.get();
    else if (strcmp(name, "VK_KHR_bind_memory2") == 0) m_bind_mem2_mod = module.get();
    m_modules.push_back(std::move(module));
}

void LayerManager::init_device_dispatch_table(VkDevice device) {
    if (device == VK_NULL_HANDLE) return;
    DeviceDispatchTable dt{};
    VkInstance inst = get_last_instance();

    #define LOAD_PROC(name) \
        dt.name = (decltype(dt.name)) get_real_proc(inst, device, "vk" #name)
    #define LOAD_PROC_OPT(field, name) \
        dt.field = (decltype(dt.field)) get_real_proc(inst, device, name)

    LOAD_PROC(CmdDraw);
    LOAD_PROC(CmdDrawIndexed);
    LOAD_PROC(CmdDrawIndirect);
    LOAD_PROC(CmdDrawIndexedIndirect);
    LOAD_PROC(CmdBindPipeline);
    LOAD_PROC(CmdBindVertexBuffers);
    LOAD_PROC(CmdBindVertexBuffers2);
    if (!dt.CmdBindVertexBuffers2) {
        LOAD_PROC_OPT(CmdBindVertexBuffers2, "vkCmdBindVertexBuffers2EXT");
    }
    LOAD_PROC(BeginCommandBuffer);
    LOAD_PROC(ResetCommandBuffer);
    LOAD_PROC(CmdPipelineBarrier);
    LOAD_PROC(CmdPipelineBarrier2);
    if (!dt.CmdPipelineBarrier2) {
        LOAD_PROC_OPT(CmdPipelineBarrier2, "vkCmdPipelineBarrier2KHR");
    }
    LOAD_PROC(CmdBeginRendering);
    if (!dt.CmdBeginRendering) {
        LOAD_PROC_OPT(CmdBeginRendering, "vkCmdBeginRenderingKHR");
    }
    LOAD_PROC(CmdEndRendering);
    if (!dt.CmdEndRendering) {
        LOAD_PROC_OPT(CmdEndRendering, "vkCmdEndRenderingKHR");
    }
    LOAD_PROC(CmdPushDescriptorSetKHR);
    LOAD_PROC(QueueSubmit);
    LOAD_PROC_OPT(QueueSubmit2, "vkQueueSubmit2KHR");
    if (!dt.QueueSubmit2) {
        LOAD_PROC(QueueSubmit2);
    }
    if (dt.QueueSubmit2) {
        LOGI("Device %p: QueueSubmit2 successfully loaded (%p)", device, (void*)dt.QueueSubmit2);
    } else {
        LOGW("Device %p: QueueSubmit2 not found in native driver", device);
    }
    LOAD_PROC(CmdSetEvent);
    LOAD_PROC(CmdResetEvent);
    LOAD_PROC(CmdWaitEvents);
    LOAD_PROC(CmdWriteTimestamp);
    LOAD_PROC_OPT(CmdWriteTimestamp2, "vkCmdWriteTimestamp2KHR");
    if (!dt.CmdWriteTimestamp2) {
        LOAD_PROC(CmdWriteTimestamp2);
    }
    LOAD_PROC(QueueWaitIdle);
    LOAD_PROC(DeviceWaitIdle);
    LOAD_PROC(GetDeviceQueue);
    LOAD_PROC(GetDeviceQueue2);
    LOAD_PROC(CreateGraphicsPipelines);
    LOAD_PROC(CreateComputePipelines);
    LOAD_PROC(CreatePipelineCache);
    LOAD_PROC(DestroyPipelineCache);
    LOAD_PROC(GetPipelineCacheData);
    LOAD_PROC(MergePipelineCaches);
    LOAD_PROC(DestroyPipeline);
    LOAD_PROC(CreateDescriptorSetLayout);
    LOAD_PROC(DestroyDescriptorSetLayout);
    LOAD_PROC(CreatePipelineLayout);
    LOAD_PROC(DestroyPipelineLayout);
    LOAD_PROC(AllocateCommandBuffers);
    LOAD_PROC(FreeCommandBuffers);
    LOAD_PROC(CreateImage);
    LOAD_PROC(DestroyImage);
    LOAD_PROC(CreateImageView);
    LOAD_PROC(DestroyImageView);
    LOAD_PROC(CreateSampler);
    LOAD_PROC(DestroySampler);
    LOAD_PROC(CmdBeginRenderPass);
    LOAD_PROC(CmdNextSubpass);
    LOAD_PROC(CmdEndRenderPass);
    LOAD_PROC(CreateRenderPass);
    LOAD_PROC(CreateFramebuffer);
    LOAD_PROC(UpdateDescriptorSets);
    LOAD_PROC(CmdBindDescriptorSets);
    LOAD_PROC(AllocateDescriptorSets);
    LOAD_PROC(FreeDescriptorSets);
    LOAD_PROC(ResetDescriptorPool);
    LOAD_PROC(CreateSemaphore);
    LOAD_PROC(DestroySemaphore);
    LOAD_PROC(GetSemaphoreCounterValue);
    if (!dt.GetSemaphoreCounterValue) {
        LOAD_PROC_OPT(GetSemaphoreCounterValue, "vkGetSemaphoreCounterValueKHR");
    }
    LOAD_PROC(WaitSemaphores);
    if (!dt.WaitSemaphores) {
        LOAD_PROC_OPT(WaitSemaphores, "vkWaitSemaphoresKHR");
    }
    LOAD_PROC(SignalSemaphore);
    if (!dt.SignalSemaphore) {
        LOAD_PROC_OPT(SignalSemaphore, "vkSignalSemaphoreKHR");
    }
    LOAD_PROC(ResetQueryPool);
    if (!dt.ResetQueryPool) {
        LOAD_PROC_OPT(ResetQueryPool, "vkResetQueryPoolEXT");
    }
    LOAD_PROC(CreateRenderPass2);
    if (!dt.CreateRenderPass2) {
        LOAD_PROC_OPT(CreateRenderPass2, "vkCreateRenderPass2KHR");
    }
    LOAD_PROC(CmdBeginRenderPass2);
    if (!dt.CmdBeginRenderPass2) {
        LOAD_PROC_OPT(CmdBeginRenderPass2, "vkCmdBeginRenderPass2KHR");
    }
    LOAD_PROC(CmdNextSubpass2);
    if (!dt.CmdNextSubpass2) {
        LOAD_PROC_OPT(CmdNextSubpass2, "vkCmdNextSubpass2KHR");
    }
    LOAD_PROC(CmdEndRenderPass2);
    if (!dt.CmdEndRenderPass2) {
        LOAD_PROC_OPT(CmdEndRenderPass2, "vkCmdEndRenderPass2KHR");
    }
    LOAD_PROC(CmdDrawIndirectCount);
    if (!dt.CmdDrawIndirectCount) {
        LOAD_PROC_OPT(CmdDrawIndirectCount, "vkCmdDrawIndirectCountKHR");
    }
    if (!dt.CmdDrawIndirectCount) {
        LOAD_PROC_OPT(CmdDrawIndirectCount, "vkCmdDrawIndirectCountAMD");
    }
    LOAD_PROC(CmdDrawIndexedIndirectCount);
    if (!dt.CmdDrawIndexedIndirectCount) {
        LOAD_PROC_OPT(CmdDrawIndexedIndirectCount, "vkCmdDrawIndexedIndirectCountKHR");
    }
    if (!dt.CmdDrawIndexedIndirectCount) {
        LOAD_PROC_OPT(CmdDrawIndexedIndirectCount, "vkCmdDrawIndexedIndirectCountAMD");
    }
    LOAD_PROC(GetBufferDeviceAddress);
    if (!dt.GetBufferDeviceAddress) {
        LOAD_PROC_OPT(GetBufferDeviceAddress, "vkGetBufferDeviceAddressKHR");
    }
    if (!dt.GetBufferDeviceAddress) {
        LOAD_PROC_OPT(GetBufferDeviceAddress, "vkGetBufferDeviceAddressEXT");
    }
    LOAD_PROC(BindBufferMemory2);
    if (!dt.BindBufferMemory2) {
        LOAD_PROC_OPT(BindBufferMemory2, "vkBindBufferMemory2KHR");
    }
    LOAD_PROC(BindImageMemory2);
    if (!dt.BindImageMemory2) {
        LOAD_PROC_OPT(BindImageMemory2, "vkBindImageMemory2KHR");
    }
    LOAD_PROC(GetBufferMemoryRequirements2);
    if (!dt.GetBufferMemoryRequirements2) {
        LOAD_PROC_OPT(GetBufferMemoryRequirements2, "vkGetBufferMemoryRequirements2KHR");
    }
    LOAD_PROC(GetImageMemoryRequirements2);
    if (!dt.GetImageMemoryRequirements2) {
        LOAD_PROC_OPT(GetImageMemoryRequirements2, "vkGetImageMemoryRequirements2KHR");
    }
    LOAD_PROC(GetImageSparseMemoryRequirements2);
    if (!dt.GetImageSparseMemoryRequirements2) {
        LOAD_PROC_OPT(GetImageSparseMemoryRequirements2, "vkGetImageSparseMemoryRequirements2KHR");
    }
    LOAD_PROC(UpdateDescriptorSetWithTemplate);
    if (!dt.UpdateDescriptorSetWithTemplate) {
        LOAD_PROC_OPT(UpdateDescriptorSetWithTemplate, "vkUpdateDescriptorSetWithTemplateKHR");
    }
    LOAD_PROC(GetDescriptorSetLayoutSupport);
    if (!dt.GetDescriptorSetLayoutSupport) {
        LOAD_PROC_OPT(GetDescriptorSetLayoutSupport, "vkGetDescriptorSetLayoutSupportKHR");
    }
    LOAD_PROC(CmdDispatchBase);
    if (!dt.CmdDispatchBase) {
        LOAD_PROC_OPT(CmdDispatchBase, "vkCmdDispatchBaseKHR");
    }
    LOAD_PROC(TrimCommandPool);
    if (!dt.TrimCommandPool) {
        LOAD_PROC_OPT(TrimCommandPool, "vkTrimCommandPoolKHR");
    }
    LOAD_PROC(CmdSetDeviceMask);
    if (!dt.CmdSetDeviceMask) {
        LOAD_PROC_OPT(CmdSetDeviceMask, "vkCmdSetDeviceMaskKHR");
    }
    LOAD_PROC(GetDeviceGroupPeerMemoryFeatures);
    if (!dt.GetDeviceGroupPeerMemoryFeatures) {
        LOAD_PROC_OPT(GetDeviceGroupPeerMemoryFeatures, "vkGetDeviceGroupPeerMemoryFeaturesKHR");
    }
    LOAD_PROC_OPT(CreateSwapchain, "vkCreateSwapchainKHR");
    LOAD_PROC_OPT(DestroySwapchain, "vkDestroySwapchainKHR");
    LOAD_PROC_OPT(GetSwapchainImages, "vkGetSwapchainImagesKHR");
    LOAD_PROC_OPT(AcquireNextImage, "vkAcquireNextImageKHR");
    LOAD_PROC_OPT(AcquireNextImage2, "vkAcquireNextImage2KHR");
    LOAD_PROC_OPT(QueuePresent, "vkQueuePresentKHR");
    LOAD_PROC(CreateFence);
    LOAD_PROC(DestroyFence);
    LOAD_PROC(WaitForFences);
    LOAD_PROC(GetFenceStatus);
    LOAD_PROC(ResetFences);
    LOAD_PROC(CmdResetQueryPool);
    LOAD_PROC(DestroyCommandPool);
    LOAD_PROC_OPT(CmdBeginDebugUtilsLabelEXT, "vkCmdBeginDebugUtilsLabelEXT");
    LOAD_PROC_OPT(CmdEndDebugUtilsLabelEXT, "vkCmdEndDebugUtilsLabelEXT");
    LOAD_PROC_OPT(CmdInsertDebugUtilsLabelEXT, "vkCmdInsertDebugUtilsLabelEXT");
    LOAD_PROC_OPT(SetDebugUtilsObjectNameEXT, "vkSetDebugUtilsObjectNameEXT");
    LOAD_PROC_OPT(SetDebugUtilsObjectTagEXT, "vkSetDebugUtilsObjectTagEXT");
    LOAD_PROC_OPT(CmdDispatch, "vkCmdDispatch");
    LOAD_PROC_OPT(CmdDispatchIndirect, "vkCmdDispatchIndirect");
    LOAD_PROC_OPT(CmdCopyBuffer, "vkCmdCopyBuffer");
    LOAD_PROC_OPT(CmdCopyImage, "vkCmdCopyImage");
    LOAD_PROC_OPT(CmdBlitImage, "vkCmdBlitImage");
    LOAD_PROC_OPT(CmdCopyBufferToImage, "vkCmdCopyBufferToImage");
    LOAD_PROC_OPT(CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
    LOAD_PROC_OPT(CmdClearAttachments, "vkCmdClearAttachments");
    LOAD_PROC_OPT(CmdClearColorImage, "vkCmdClearColorImage");
    LOAD_PROC_OPT(CmdClearDepthStencilImage, "vkCmdClearDepthStencilImage");
    LOAD_PROC_OPT(CmdExecuteCommands, "vkCmdExecuteCommands");

    #undef LOAD_PROC
    #undef LOAD_PROC_OPT

    std::unique_lock<std::shared_mutex> lock(m_table_rw_mutex);
    m_device_tables[(uint64_t)(uintptr_t)device] = std::make_shared<const DeviceDispatchTable>(dt);
    if (!m_has_primary_table.load(std::memory_order_relaxed) || m_primary_device.load(std::memory_order_relaxed) == device) {
        m_primary_device.store(device, std::memory_order_release);
        m_has_primary_table.store(true, std::memory_order_release);
    }
    m_table_generation.fetch_add(1, std::memory_order_release);
}

void LayerManager::remove_device_dispatch_table(VkDevice device) {
    std::unique_lock<std::shared_mutex> lock(m_table_rw_mutex);
    m_device_tables.erase((uint64_t)(uintptr_t)device);
    if (m_primary_device.load(std::memory_order_relaxed) == device) {
        if (!m_device_tables.empty()) {
            auto it = m_device_tables.begin();
            m_primary_device.store((VkDevice)(uintptr_t)it->first, std::memory_order_release);
        } else {
            m_has_primary_table.store(false, std::memory_order_release);
            m_primary_device.store(VK_NULL_HANDLE, std::memory_order_release);
        }
    }
    m_table_generation.fetch_add(1, std::memory_order_release);
}

void LayerManager::update_device_bypass_flags(VkDevice device) {
    if (device == VK_NULL_HANDLE) return;

    DeviceBypassFlags flags{};

    flags.bypass_draw = (m_divisor_mod == nullptr || !m_divisor_mod->is_enabled() || m_divisor_mod->is_device_native(device));
    flags.bypass_multi_draw = (m_multi_draw_indirect_mod == nullptr || !m_multi_draw_indirect_mod->is_enabled() || m_multi_draw_indirect_mod->is_device_native(device));
    flags.bypass_sync2 = (m_sync2_mod == nullptr || !m_sync2_mod->is_enabled() || m_sync2_mod->is_device_native(device));
    flags.bypass_dynamic_rendering = (m_dyn_rendering_mod == nullptr || !m_dyn_rendering_mod->is_enabled() || m_dyn_rendering_mod->is_device_native(device));
    flags.bypass_push_descriptor = (m_push_desc_mod == nullptr || !m_push_desc_mod->is_enabled() || m_push_desc_mod->is_device_native(device));
    flags.bypass_timeline = (m_timeline_mod == nullptr || !m_timeline_mod->is_enabled() || m_timeline_mod->is_device_native(device));
    flags.bypass_renderpass2 = (m_renderpass2_mod == nullptr || !m_renderpass2_mod->is_enabled() || m_renderpass2_mod->is_device_native(device));
    flags.bypass_draw_indirect_count = (m_draw_indirect_count_mod == nullptr || !m_draw_indirect_count_mod->is_enabled() || m_draw_indirect_count_mod->is_device_native(device));
    flags.bypass_device_group = (m_device_group_mod == nullptr || !m_device_group_mod->is_enabled() || m_device_group_mod->is_device_native(device));
    flags.bypass_bda = (m_bda_mod == nullptr || !m_bda_mod->is_enabled() || m_bda_mod->is_device_native(device));
    flags.bypass_host_query_reset = (m_host_query_reset_mod == nullptr || !m_host_query_reset_mod->is_enabled() || m_host_query_reset_mod->is_device_native(device));

    {
        std::unique_lock<std::shared_mutex> lock(m_table_rw_mutex);
        m_device_bypass_flags[(uint64_t)(uintptr_t)device] = flags;
        if (!m_has_primary_bypass.load(std::memory_order_relaxed) || m_primary_device.load(std::memory_order_relaxed) == device) {
            m_primary_bypass_flags = flags;
            m_primary_multi_draw_bypass.store(flags.bypass_multi_draw, std::memory_order_release);
            m_has_primary_bypass.store(true, std::memory_order_release);
        }
    }

    if (flags.bypass_draw) {
        m_divisor_bypass_active.store(true, std::memory_order_release);
        LOGI("Device %p: Divisor bypass active (Trampoline enabled, 0-overhead draw/bind)", device);
    } else {
        m_divisor_bypass_active.store(false, std::memory_order_release);
        LOGI("Device %p: Divisor emulation active (sub-draw batching)", device);
    }

    LOGI("Device %p bypass flags: draw=%d, multi_draw=%d, sync2=%d, dyn_render=%d, push_desc=%d, timeline=%d, rp2=%d",
         device, flags.bypass_draw, flags.bypass_multi_draw, flags.bypass_sync2, flags.bypass_dynamic_rendering,
         flags.bypass_push_descriptor, flags.bypass_timeline, flags.bypass_renderpass2);
}

void LayerManager::remove_device_bypass_flags(VkDevice device) {
    std::unique_lock<std::shared_mutex> lock(m_table_rw_mutex);
    m_device_bypass_flags.erase((uint64_t)(uintptr_t)device);
    if (m_primary_device.load(std::memory_order_relaxed) == device) {
        if (!m_device_bypass_flags.empty()) {
            auto it = m_device_bypass_flags.begin();
            m_primary_bypass_flags = it->second;
            m_primary_multi_draw_bypass.store(m_primary_bypass_flags.bypass_multi_draw, std::memory_order_release);
            m_has_primary_bypass.store(true, std::memory_order_release);
            m_divisor_bypass_active.store(m_primary_bypass_flags.bypass_draw, std::memory_order_release);
        } else {
            m_has_primary_bypass.store(false, std::memory_order_release);
            m_primary_multi_draw_bypass.store(false, std::memory_order_release);
            m_divisor_bypass_active.store(false, std::memory_order_release);
        }
    }
}

bool LayerManager::should_intercept_device_proc(VkDevice device, const char* pName) {
    if (VertexStoreEmulator::get().enabled(device)) return true;

    if (!pName) return true;
    // Tiler scope/lifetime tracking is required even for native entry points.
    if (m_dyn_rendering_mod && m_dyn_rendering_mod->needs_tiler_tracking(device)) {
        if (!strncmp(pName, "vkCmdDraw", 9) ||
            !strncmp(pName, "vkCmdBeginRenderPass", 20) ||
            !strncmp(pName, "vkCmdEndRenderPass", 18) ||
            !strncmp(pName, "vkCmdNextSubpass", 16) ||
            !strncmp(pName, "vkCmdBeginQuery", 15) ||
            !strncmp(pName, "vkCmdEndQuery", 13) ||
            !strcmp(pName, "vkCmdBeginTransformFeedbackEXT") ||
            !strcmp(pName, "vkCmdEndTransformFeedbackEXT") ||
            !strcmp(pName, "vkCmdBeginConditionalRenderingEXT") ||
            !strcmp(pName, "vkCmdEndConditionalRenderingEXT") ||
            !strcmp(pName, "vkCreateRenderPass2") || !strcmp(pName, "vkCreateRenderPass2KHR") ||
            !strcmp(pName, "vkCreateCommandPool") || !strcmp(pName, "vkResetCommandPool") ||
            !strcmp(pName, "vkDestroyCommandPool")) return true;
    }
    if (m_push_desc_mod && m_push_desc_mod->is_enabled() &&
        !m_push_desc_mod->is_device_native(device) &&
        (!strcmp(pName, "vkCmdBindDescriptorSets") ||
         !strcmp(pName, "vkCmdBindDescriptorSets2") ||
         !strcmp(pName, "vkCmdBindDescriptorSets2KHR") ||
         !strcmp(pName, "vkResetCommandPool") || !strcmp(pName, "vkDestroyCommandPool") ||
         !strcmp(pName, "vkEndCommandBuffer")))
        return true;
    // These were direct driver calls before vertex emulation introduced state tracking.
    for (const char* name : {"vkCreateBuffer", "vkDestroyBuffer", "vkDestroyShaderModule",
                            "vkCreateCommandPool", "vkDestroyCommandPool", "vkResetCommandPool",
                            "vkEndCommandBuffer", "vkCmdBindDescriptorSets", "vkCmdBindIndexBuffer",
                            "vkCmdPushConstants", "vkCmdBeginQuery", "vkCmdEndQuery",
                            "vkCmdBindDescriptorSets2", "vkCmdBindDescriptorSets2KHR",
                            "vkCmdPushConstants2", "vkCmdPushConstants2KHR",
                            "vkCmdBindIndexBuffer2", "vkCmdBindIndexBuffer2KHR",
                            "vkCmdBeginConditionalRenderingEXT", "vkCmdEndConditionalRenderingEXT",
                            "vkCmdDrawMultiEXT", "vkCmdDrawMultiIndexedEXT", "vkCreateShadersEXT"}) {
        if (strcmp(pName, name) == 0) return false;
    }

    // Retrieve bypass flags for this device
    DeviceBypassFlags flags;
    bool found = false;
    if (__builtin_expect(device == m_primary_device.load(std::memory_order_relaxed), 1) &&
        m_has_primary_bypass.load(std::memory_order_relaxed)) {
        flags = m_primary_bypass_flags;
        found = true;
    } else {
        std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
        auto it = m_device_bypass_flags.find((uint64_t)(uintptr_t)device);
        if (it != m_device_bypass_flags.end()) {
            flags = it->second;
            found = true;
        }
    }

    if (!found) {
        // Device not yet registered or untracked: maintain layer interception for safety
        return true;
    }

    // Hot path 1: vkCmd...
    if (pName[0] == 'v' && pName[1] == 'k' && pName[2] == 'C' && pName[3] == 'm' && pName[4] == 'd') {
        const char* cmd = pName + 5; // e.g. "DrawIndexed", "BindPipeline", "PipelineBarrier2", etc.
        const char c0 = cmd[0];

        bool tbdr_active = TBDRBarrierOptimizer::get().is_enabled();

        if (!tbdr_active) {
            if (flags.bypass_draw && (c0 == 'D' || c0 == 'B')) {
                if (strcmp(cmd, "DrawIndexed") == 0 ||
                    strcmp(cmd, "Draw") == 0 ||
                    strcmp(cmd, "BindPipeline") == 0 ||
                    strcmp(cmd, "BindVertexBuffers") == 0 ||
                    strcmp(cmd, "BindVertexBuffers2") == 0 ||
                    strcmp(cmd, "BindVertexBuffers2EXT") == 0) {
                    return false; // Direct bypass! Driver native!
                }
            }
            if (flags.bypass_multi_draw && c0 == 'D') {
                if (strcmp(cmd, "DrawIndirect") == 0 ||
                    strcmp(cmd, "DrawIndexedIndirect") == 0) {
                    return false; // Direct bypass! Driver native!
                }
            }
        }
        // Pipeline barriers are intercepted for TBDR Tile flush optimization when optimizer is active
        if (c0 == 'P' && !tbdr_active) {
            if (strcmp(cmd, "PipelineBarrier") == 0) {
                return false;
            }
        }
        if (flags.bypass_sync2) {
            if (c0 == 'P' && !tbdr_active) {
                if (strcmp(cmd, "PipelineBarrier2") == 0 ||
                    strcmp(cmd, "PipelineBarrier2KHR") == 0) {
                    return false;
                }
            }
            if (c0 == 'S' || c0 == 'R' || c0 == 'W') {
                if (strcmp(cmd, "SetEvent2") == 0 ||
                    strcmp(cmd, "SetEvent2KHR") == 0 ||
                    strcmp(cmd, "ResetEvent2") == 0 ||
                    strcmp(cmd, "ResetEvent2KHR") == 0 ||
                    strcmp(cmd, "WaitEvents2") == 0 ||
                    strcmp(cmd, "WaitEvents2KHR") == 0 ||
                    strcmp(cmd, "WriteTimestamp2") == 0 ||
                    strcmp(cmd, "WriteTimestamp2KHR") == 0) {
                    return false;
                }
            }
        }
        if (!tbdr_active) {
            if (flags.bypass_dynamic_rendering && (c0 == 'B' || c0 == 'E')) {
                if (strcmp(cmd, "BeginRendering") == 0 ||
                    strcmp(cmd, "BeginRenderingKHR") == 0 ||
                    strcmp(cmd, "EndRendering") == 0 ||
                    strcmp(cmd, "EndRenderingKHR") == 0) {
                    return false;
                }
            }
            if (flags.bypass_push_descriptor && c0 == 'P') {
                if (strcmp(cmd, "PushDescriptorSetKHR") == 0 ||
                    strcmp(cmd, "PushDescriptorSetWithTemplateKHR") == 0) {
                    return false;
                }
            }
            if (flags.bypass_draw_indirect_count && c0 == 'D') {
                if (strcmp(cmd, "DrawIndirectCount") == 0 ||
                    strcmp(cmd, "DrawIndirectCountKHR") == 0 ||
                    strcmp(cmd, "DrawIndirectCountAMD") == 0 ||
                    strcmp(cmd, "DrawIndexedIndirectCount") == 0 ||
                    strcmp(cmd, "DrawIndexedIndirectCountKHR") == 0 ||
                    strcmp(cmd, "DrawIndexedIndirectCountAMD") == 0) {
                    return false;
                }
            }
            if (flags.bypass_renderpass2 && (c0 == 'B' || c0 == 'N' || c0 == 'E')) {
                if (strcmp(cmd, "BeginRenderPass2") == 0 ||
                    strcmp(cmd, "BeginRenderPass2KHR") == 0 ||
                    strcmp(cmd, "NextSubpass2") == 0 ||
                    strcmp(cmd, "NextSubpass2KHR") == 0 ||
                    strcmp(cmd, "EndRenderPass2") == 0 ||
                    strcmp(cmd, "EndRenderPass2KHR") == 0) {
                    return false;
                }
            }
            if (flags.bypass_device_group && (c0 == 'S' || c0 == 'D')) {
                if (strcmp(cmd, "SetDeviceMask") == 0 ||
                    strcmp(cmd, "SetDeviceMaskKHR") == 0 ||
                    strcmp(cmd, "DispatchBase") == 0 ||
                    strcmp(cmd, "DispatchBaseKHR") == 0) {
                    return false;
                }
            }
        }
        return true;
    }

    // Queue submissions:
    if (flags.bypass_sync2 && flags.bypass_timeline) {
        if (strcmp(pName, "vkQueueSubmit2") == 0 ||
            strcmp(pName, "vkQueueSubmit2KHR") == 0) {
            return false;
        }
    }

    // Timeline Semaphores:
    if (flags.bypass_timeline) {
        if (strcmp(pName, "vkWaitSemaphores") == 0 ||
            strcmp(pName, "vkWaitSemaphoresKHR") == 0 ||
            strcmp(pName, "vkSignalSemaphore") == 0 ||
            strcmp(pName, "vkSignalSemaphoreKHR") == 0 ||
            strcmp(pName, "vkGetSemaphoreCounterValue") == 0 ||
            strcmp(pName, "vkGetSemaphoreCounterValueKHR") == 0) {
            return false;
        }
    }

    // Buffer Device Address:
    if (flags.bypass_bda) {
        if (strcmp(pName, "vkGetBufferDeviceAddress") == 0 ||
            strcmp(pName, "vkGetBufferDeviceAddressKHR") == 0 ||
            strcmp(pName, "vkGetBufferDeviceAddressEXT") == 0) {
            return false;
        }
    }

    // Host Query Reset:
    if (flags.bypass_host_query_reset) {
        if (strcmp(pName, "vkResetQueryPool") == 0 ||
            strcmp(pName, "vkResetQueryPoolEXT") == 0) {
            return false;
        }
    }

    // RenderPass2 creation:
    if (flags.bypass_renderpass2) {
        if (strcmp(pName, "vkCreateRenderPass2") == 0 ||
            strcmp(pName, "vkCreateRenderPass2KHR") == 0) {
            return false;
        }
    }

    return true; // Keep intercepting for non-bypassed commands
}

std::shared_ptr<const DeviceDispatchTable> LayerManager::get_dispatch_table_slow(VkDevice device) {
    {
        std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
        auto it = m_device_tables.find((uint64_t)(uintptr_t)device);
        if (it != m_device_tables.end()) {
            return it->second;
        }
    }
    if (device != VK_NULL_HANDLE) {
        init_device_dispatch_table(device);
        std::shared_lock<std::shared_mutex> lock(m_table_rw_mutex);
        auto it = m_device_tables.find((uint64_t)(uintptr_t)device);
        if (it != m_device_tables.end()) {
            return it->second;
        }
    }
    static const auto s_empty = std::make_shared<const DeviceDispatchTable>();
    return s_empty;
}

VkDevice LayerManager::get_device_for_cmd_slow(VkCommandBuffer cmd) {
    std::shared_lock<std::shared_mutex> lock(m_cmd_device_rw_mutex);
    auto it = m_cmd_devices.find((uint64_t)(uintptr_t)cmd);
    if (it != m_cmd_devices.end()) {
        return it->second;
    }
    return m_last_device.load(std::memory_order_relaxed);
}

VkDevice LayerManager::get_device_for_queue_slow(VkQueue queue) {
    std::shared_lock<std::shared_mutex> lock(m_cmd_device_rw_mutex);
    auto it = m_queue_devices.find((uint64_t)(uintptr_t)queue);
    if (it != m_queue_devices.end()) {
        return it->second;
    }
    return m_last_device.load(std::memory_order_relaxed);
}

void LayerManager::add_emulated_device(VkDevice device) {
    std::unique_lock<std::shared_mutex> lock(m_state_rw_mutex);
    m_emulated_devices.insert((uint64_t)(uintptr_t)device);
    m_primary_emulated_device.store(device, std::memory_order_relaxed);
}

void LayerManager::remove_emulated_device(VkDevice device) {
    std::unique_lock<std::shared_mutex> lock(m_state_rw_mutex);
    m_emulated_devices.erase((uint64_t)(uintptr_t)device);
    if (m_primary_emulated_device.load(std::memory_order_relaxed) == device) {
        VkDevice next_dev = VK_NULL_HANDLE;
        if (!m_emulated_devices.empty()) {
            next_dev = (VkDevice)(uintptr_t)*m_emulated_devices.begin();
        }
        m_primary_emulated_device.store(next_dev, std::memory_order_relaxed);
    }
}

void LayerManager::set_device_max_spirv_version(VkDevice device, uint32_t version) {
    if (device == VK_NULL_HANDLE) return;
    std::unique_lock<std::shared_mutex> lock(m_device_info_mutex);
    m_device_spirv_versions[(uint64_t)(uintptr_t)device] = version;
    if (device == m_primary_device.load(std::memory_order_relaxed) ||
        m_primary_device.load(std::memory_order_relaxed) == VK_NULL_HANDLE) {
        m_primary_spirv_version.store(version, std::memory_order_release);
    }
}

uint32_t LayerManager::get_device_max_spirv_version(VkDevice device) {
    if (device == VK_NULL_HANDLE) return 0;
    if (__builtin_expect(device == m_primary_device.load(std::memory_order_relaxed), 1)) {
        return m_primary_spirv_version.load(std::memory_order_acquire);
    }
    std::shared_lock<std::shared_mutex> lock(m_device_info_mutex);
    auto it = m_device_spirv_versions.find((uint64_t)(uintptr_t)device);
    return it != m_device_spirv_versions.end() ? it->second : 0;
}

vkfix::SubgroupBroadcastSupport LayerManager::get_device_subgroup_broadcast(VkDevice device) {
    std::shared_lock<std::shared_mutex> lock(m_device_info_mutex);
    auto it = m_device_subgroup_broadcast.find((uint64_t)(uintptr_t)device);
    return it != m_device_subgroup_broadcast.end() ? it->second : vkfix::SubgroupBroadcastSupport{};
}

VkResult LayerManager::dispatch_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties
) {
    PFN_vkEnumerateDeviceExtensionProperties real_fn =
        (PFN_vkEnumerateDeviceExtensionProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    if (pLayerName != NULL) {
        return real_fn(physicalDevice, pLayerName, pPropertyCount, pProperties);
    }

    uint32_t real_count = 0;
    VkResult res = real_fn(physicalDevice, NULL, &real_count, NULL);
    if (res != VK_SUCCESS) return res;

    std::vector<VkExtensionProperties> extensions;
    if (real_count > 0) {
        extensions.resize(real_count);
        res = real_fn(physicalDevice, NULL, &real_count, extensions.data());
        if (res != VK_SUCCESS) return res;
    }

    {
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_enumerate_device_extensions(physicalDevice, extensions);
            }
        }
    }

    uint32_t total_count = (uint32_t) extensions.size();
    if (pProperties == NULL) {
        *pPropertyCount = total_count;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pPropertyCount < total_count) ? *pPropertyCount : total_count;
    memcpy(pProperties, extensions.data(), sizeof(VkExtensionProperties) * to_copy);
    *pPropertyCount = to_copy;

    return (to_copy < total_count) ? VK_INCOMPLETE : VK_SUCCESS;
}

void LayerManager::dispatch_get_physical_device_features(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures
) {
    if (!pFeatures) return;

    PFN_vkGetPhysicalDeviceFeatures real_fn =
        (PFN_vkGetPhysicalDeviceFeatures) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures");
    if (real_fn) {
        real_fn(physicalDevice, pFeatures);
    }

    for (auto& mod : m_modules) {
        if (mod->is_enabled()) {
            mod->on_get_features(physicalDevice, pFeatures);
        }
    }
}

void LayerManager::dispatch_get_physical_device_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures
) {
    if (!pFeatures) return;

    PromotedFeatureQuery promotions(physicalDevice, pFeatures);
    PFN_vkGetPhysicalDeviceFeatures2 real_fn = native_features2_proc(physicalDevice);

    if (!real_fn) {
        auto nativeInstance = vkfix::native_instance_for_physical(physicalDevice);
        if (!nativeInstance) nativeInstance = get_last_instance();
        PFN_vkGetPhysicalDeviceFeatures real_fn1 =
            (PFN_vkGetPhysicalDeviceFeatures) get_real_proc(nativeInstance, VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures");

        const size_t mod_count = m_modules.size();
        void* stack_user_data[32] = {nullptr};
        std::vector<void*> heap_user_data;
        void** user_data = stack_user_data;
        if (__builtin_expect(mod_count > 32, 0)) {
            heap_user_data.resize(mod_count, nullptr);
            user_data = heap_user_data.data();
        }

        // On a 1.0 driver there is no Features2 entry point. Run the module
        // pre-hooks (they unlink the 1.1/1.2 structs), fill the 1.0 features,
        // then let the modules relink and fill the emulated structs.
        for (size_t i = 0; i < mod_count; i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_pre_get_features2(physicalDevice, pFeatures, user_data[i]);
            }
        }
        if (real_fn1) {
            real_fn1(physicalDevice, &pFeatures->features);
        }
        for (size_t i = 0; i < mod_count; i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_get_features(physicalDevice, &pFeatures->features);
                m_modules[i]->on_post_get_features2(physicalDevice, pFeatures, user_data[i]);
            }
        }
        return;
    }

    const size_t mod_count = m_modules.size();
    void* stack_user_data[32] = {nullptr};
    std::vector<void*> heap_user_data;
    void** user_data = stack_user_data;
    if (__builtin_expect(mod_count > 32, 0)) {
        heap_user_data.resize(mod_count, nullptr);
        user_data = heap_user_data.data();
    }

    for (size_t i = 0; i < mod_count; i++) {
        if (m_modules[i]->is_enabled()) {
            m_modules[i]->on_pre_get_features2(physicalDevice, pFeatures, user_data[i]);
        }
    }

    real_fn(physicalDevice, pFeatures);

    for (size_t i = 0; i < mod_count; i++) {
        if (m_modules[i]->is_enabled()) {
            m_modules[i]->on_post_get_features2(physicalDevice, pFeatures, user_data[i]);
        }
    }
}

void LayerManager::dispatch_get_physical_device_properties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties
) {
    if (!pProperties) return;
    uint32_t nativeApi;
    {
        PromotedPropertyQuery promotion(physicalDevice, pProperties);
        nativeApi = promotion.native_api_version();
        std::vector<void*> user_data(m_modules.size(), nullptr);
        for (size_t i = 0; i < m_modules.size(); ++i)
            if (m_modules[i]->is_enabled())
                m_modules[i]->on_pre_get_properties2(physicalDevice, pProperties, user_data[i]);
        promotion.query_native();
        for (size_t i = 0; i < m_modules.size(); ++i)
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_get_properties(physicalDevice, &pProperties->properties);
                m_modules[i]->on_post_get_properties2(physicalDevice, pProperties, user_data[i]);
            }
    }
    vkfix::set_vulkan_fix_driver_info(*pProperties, nativeApi);
}

void LayerManager::dispatch_get_physical_device_properties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties
) {
    if (!pProperties) return;

    PFN_vkGetPhysicalDeviceProperties real_fn =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_fn) {
        real_fn(physicalDevice, pProperties);
    }

    for (auto& mod : m_modules) {
        if (mod->is_enabled()) {
            mod->on_get_properties(physicalDevice, pProperties);
        }
    }
}

VkResult LayerManager::dispatch_create_device(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice
) {
    VkInstance nativeInstance = vkfix::native_instance_for_physical(physicalDevice);
    if (!nativeInstance) nativeInstance = get_last_instance();
    PFN_vkCreateDevice real_fn =
        (PFN_vkCreateDevice) get_real_proc(nativeInstance, VK_NULL_HANDLE, "vkCreateDevice");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pCreateInfo) return real_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);

    VkDeviceCreateInfo modCreateInfo = *pCreateInfo;
    VkPhysicalDeviceFeatures modFeatures{};
    bool has_mod_features = false;
    if (pCreateInfo->pEnabledFeatures != NULL) {
        modFeatures = *pCreateInfo->pEnabledFeatures;
        modCreateInfo.pEnabledFeatures = &modFeatures;
        has_mod_features = true;
    }

    std::vector<const char*> enabledExtensions;
    if (pCreateInfo->enabledExtensionCount > 0 && pCreateInfo->ppEnabledExtensionNames != NULL) {
        enabledExtensions.assign(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount
        );
    }

    PromotedDeviceFeatures promotedFeatures(physicalDevice, modCreateInfo, enabledExtensions);
    if (promotedFeatures.result() != VK_SUCCESS) {
        if (pDevice) *pDevice = VK_NULL_HANDLE;
        return promotedFeatures.result();
    }
    if (!has_mod_features && modCreateInfo.pEnabledFeatures) {
        modFeatures = *modCreateInfo.pEnabledFeatures;
        modCreateInfo.pEnabledFeatures = &modFeatures;
        has_mod_features = true;
    }

    const size_t mod_count = m_modules.size();
    void* stack_user_data[32] = {nullptr};
    std::vector<void*> heap_user_data;
    void** user_data = stack_user_data;
    if (__builtin_expect(mod_count > 32, 0)) {
        heap_user_data.resize(mod_count, nullptr);
        user_data = heap_user_data.data();
    }

    for (size_t i = 0; i < mod_count; i++) {
        if (m_modules[i]->is_enabled()) {
            m_modules[i]->on_pre_create_device(physicalDevice, &modCreateInfo, has_mod_features ? &modFeatures : nullptr, enabledExtensions, user_data[i]);
        }
    }

    vkfix::ExtensionEnvironment environment;
    if (auto context = vkfix::native_state_for_physical(physicalDevice)) {
        environment.instance_api = context->instance_api;
        environment.enabled_instance = context->enabled_extensions;
    }
    if (auto properties = (PFN_vkGetPhysicalDeviceProperties)get_real_proc(
            nativeInstance, VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties")) {
        VkPhysicalDeviceProperties nativeProperties{};
        properties(physicalDevice, &nativeProperties);
        environment.device_api = nativeProperties.apiVersion;
        if (auto context = vkfix::native_state_for_physical(physicalDevice))
            environment.device_api = std::min(environment.device_api, context->target_api);
    }
    VkResult res = VK_SUCCESS;
    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)get_real_proc(
        nativeInstance, VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    auto gatherExtensions = [&](const char *layer) {
        if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            uint32_t count = 0;
            VkResult status = enumerate(physicalDevice, layer, &count, nullptr);
            if (status != VK_SUCCESS) return status;
            std::vector<VkExtensionProperties> available(count);
            status = count ? enumerate(physicalDevice, layer, &count, available.data()) : VK_SUCCESS;
            if (status == VK_INCOMPLETE) continue;
            if (status != VK_SUCCESS) return status;
            for (uint32_t i = 0; i < count; ++i) environment.available.insert(available[i].extensionName);
            return VK_SUCCESS;
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    // With no native extension requests no closure is needed, including in
    // deterministic tests whose mock boundary omits native enumeration.
    if (!enabledExtensions.empty()) {
        res = gatherExtensions(nullptr);
        // Device layers are superseded; enabled layers belong to the instance.
        // Never read the legacy VkDeviceCreateInfo layer-name fields.
        if (auto context = vkfix::native_state_for_physical(physicalDevice))
            for (const auto &layer : context->enabled_layers) {
                if (res != VK_SUCCESS) break;
                res = gatherExtensions(layer.c_str());
            }
    }
    auto dependencyPlan = res == VK_SUCCESS
                              ? vkfix::close_extension_dependencies(enabledExtensions, environment)
                              : vkfix::ExtensionDependencyPlan{};
    if (res == VK_SUCCESS) res = dependencyPlan.result;
    if (res == VK_SUCCESS) {
        modCreateInfo.enabledExtensionCount = uint32_t(dependencyPlan.names.size());
        modCreateInfo.ppEnabledExtensionNames = dependencyPlan.names.empty() ? nullptr : dependencyPlan.names.data();
        res = real_fn(physicalDevice, &modCreateInfo, pAllocator, pDevice);
    } else {
        LOGE("vkCreateDevice: native extension dependency failed: %s", dependencyPlan.missing.c_str());
        if (pDevice) *pDevice = VK_NULL_HANDLE;
    }

    for (size_t i = 0; i < mod_count; i++) {
        if (m_modules[i]->is_enabled()) {
            m_modules[i]->on_post_create_device(physicalDevice, (res == VK_SUCCESS && pDevice) ? *pDevice : VK_NULL_HANDLE, res, user_data[i]);
        }
    }

    if (res == VK_SUCCESS && pDevice && *pDevice != VK_NULL_HANDLE && m_timeline_mod && m_timeline_mod->is_enabled() && !m_timeline_mod->is_device_native(*pDevice)) {
        auto aggregate = vku::find_pnext<VkPhysicalDeviceVulkan12Features>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        auto feature = vku::find_pnext<VkPhysicalDeviceTimelineSemaphoreFeatures>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        auto mode = getenv("VULKAN_FIX_TIMELINE_ASYNC");
        if (((aggregate && aggregate->timelineSemaphore) || (feature && feature->timelineSemaphore)) && (!mode || strcmp(mode,"0"))) {
            auto started = vkfix::TimelineScheduler::get().start(*pDevice, modCreateInfo);
            if (started != VK_SUCCESS) {
                for (auto& module : m_modules) if (module->is_enabled()) module->on_destroy_device(*pDevice);
                if (auto destroy = (PFN_vkDestroyDevice)get_real_proc(get_last_instance(),*pDevice,"vkDestroyDevice")) destroy(*pDevice,pAllocator);
                *pDevice=VK_NULL_HANDLE;
                return started;
            }
        }
    }
    if (res == VK_SUCCESS && pDevice && *pDevice != VK_NULL_HANDLE) {
        add_emulated_device(*pDevice);
        m_last_device = *pDevice;
        m_device_count.fetch_add(1, std::memory_order_relaxed);
        m_primary_device.store(*pDevice, std::memory_order_relaxed);
        {
            std::unique_lock<std::shared_mutex> lock(m_state_rw_mutex);
            m_device_phys[(uint64_t)(uintptr_t)*pDevice] = physicalDevice;
        }
        init_device_dispatch_table(*pDevice);
        RenderDocManager::get().on_device_created(*pDevice, nativeInstance);
        update_device_bypass_flags(*pDevice);
        PipelineCacheManager::get().init_for_device(physicalDevice, *pDevice);

        // Record the SPIR-V ceiling implied by the *real* driver API version.
        PFN_vkGetPhysicalDeviceProperties real_props = (PFN_vkGetPhysicalDeviceProperties)
            get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
        uint32_t api_version = VK_API_VERSION_1_0;
        if (real_props) {
            VkPhysicalDeviceProperties props{};
            real_props(physicalDevice, &props);
            api_version = props.apiVersion;
        }
        const uint32_t spirv_version = ShaderRewriter::api_version_to_spirv(api_version);
        set_device_max_spirv_version(*pDevice, spirv_version);
        {
            auto broadcast = vkfix::subgroup_broadcast_support(physicalDevice);
            std::unique_lock<std::shared_mutex> lock(m_device_info_mutex);
            m_device_subgroup_broadcast[(uint64_t)(uintptr_t)*pDevice] = broadcast;
        }
        LOGI("Created logical device %p (driver api 0x%x, max SPIR-V %s)",
             *pDevice, api_version, ShaderRewriter::spirv_version_string(spirv_version));
    }

    return res;
}

void LayerManager::dispatch_destroy_device(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator
) {
    vkfix::TimelineScheduler::get().stop(device);
    PipelineCacheManager::get().on_destroy_device(device);
    RenderDocManager::get().on_device_destroyed(device);
    m_device_count.fetch_sub(1, std::memory_order_relaxed);
    remove_device_bypass_flags(device);
    remove_device_dispatch_table(device);
    {
        std::unique_lock<std::shared_mutex> lock(m_state_rw_mutex);
        m_device_phys.erase((uint64_t)(uintptr_t)device);
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_cmd_device_rw_mutex);
        for (auto it = m_cmd_devices.begin(); it != m_cmd_devices.end(); ) {
            if (it->second == device) {
                it = m_cmd_devices.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = m_queue_devices.begin(); it != m_queue_devices.end(); ) {
            if (it->second == device) {
                it = m_queue_devices.erase(it);
            } else {
                ++it;
            }
        }
        m_device_queues.erase((uint64_t)(uintptr_t)device);
        if (m_last_device.load() == device) {
            m_last_device = VK_NULL_HANDLE;
        }
    }

    {
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_destroy_device(device);
            }
        }
    }

    remove_emulated_device(device);

    {
        std::unique_lock<std::shared_mutex> info_lock(m_device_info_mutex);
        if (m_primary_device.load(std::memory_order_relaxed) == device) {
            m_primary_spirv_version.store(0, std::memory_order_release);
        }
        m_device_spirv_versions.erase((uint64_t)(uintptr_t)device);
        m_device_subgroup_broadcast.erase((uint64_t)(uintptr_t)device);
    }

    PFN_vkDestroyDevice real_fn =
        (PFN_vkDestroyDevice) get_real_proc(get_last_instance(), device, "vkDestroyDevice");
    if (real_fn) {
        real_fn(device, pAllocator);
    }
}

VkResult LayerManager::dispatch_create_graphics_pipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines
) {
    VkResult emulated_result = VK_SUCCESS;
    if (VertexStoreEmulator::get().create_pipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, emulated_result)) return emulated_result;

    const auto& dt = get_dispatch_table(device);
    PFN_vkCreateGraphicsPipelines real_fn = dt.CreateGraphicsPipelines;
    if (!real_fn) {
        real_fn = (PFN_vkCreateGraphicsPipelines) get_real_proc(get_last_instance(), device, "vkCreateGraphicsPipelines");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkPipelineCache effectiveCache = PipelineCacheManager::get().prepare_pipeline_cache(device, pipelineCache);

    // Hold shared access to the layer's internal cache for the entire call so the
    // background flusher can never query/flush the same VkPipelineCache at the
    // same time (Vulkan requires external synchronization on cache host access).
    auto cacheGuard = PipelineCacheManager::get().lock_cache_for_creation(device, effectiveCache);

    if (!is_emulated_device(device) || createInfoCount == 0 || !pCreateInfos) {
        VkResult res = real_fn(device, effectiveCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        if (res == VK_SUCCESS) {
            PipelineCacheManager::get().on_pipelines_created(device, effectiveCache, createInfoCount);
        }
        return res;
    }

    // Fast-path scan: Check if ANY module requires interception for this pipeline batch
    bool needs_interception = false;
    if (m_fill_mode_mod && m_fill_mode_mod->is_enabled() && m_fill_mode_mod->needs_pipeline_interception(device, createInfoCount, pCreateInfos)) {
        needs_interception = true;
    } else if (m_divisor_mod && m_divisor_mod->is_enabled() && m_divisor_mod->needs_pipeline_interception(device, createInfoCount, pCreateInfos)) {
        needs_interception = true;
    } else if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled() && m_dyn_rendering_mod->needs_pipeline_interception(device, createInfoCount, pCreateInfos)) {
        needs_interception = true;
    }

    // FAST-PATH: Zero heap allocations, zero copies!
    if (!needs_interception) {
        VkResult res = real_fn(device, effectiveCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        if (res == VK_SUCCESS) {
            PipelineCacheManager::get().on_pipelines_created(device, effectiveCache, createInfoCount);
        }
        return res;
    }

    // Small Buffer Optimization: use stack allocation for common small batches (<= 16 pipelines)
    constexpr uint32_t SBO_LIMIT = 16;
    VkGraphicsPipelineCreateInfo stackInfos[SBO_LIMIT];
    VkPipelineVertexInputStateCreateInfo stackVIStates[SBO_LIMIT];

    VkGraphicsPipelineCreateInfo* modInfos = (createInfoCount <= SBO_LIMIT) ?
        stackInfos : (VkGraphicsPipelineCreateInfo*) malloc(sizeof(VkGraphicsPipelineCreateInfo) * createInfoCount);
    if (!modInfos) return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkPipelineVertexInputStateCreateInfo* modVIStates = (createInfoCount <= SBO_LIMIT) ?
        stackVIStates : (VkPipelineVertexInputStateCreateInfo*) malloc(sizeof(VkPipelineVertexInputStateCreateInfo) * createInfoCount);
    if (!modVIStates) {
        if (modInfos != stackInfos) free(modInfos);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    memcpy(modInfos, pCreateInfos, sizeof(VkGraphicsPipelineCreateInfo) * createInfoCount);

    std::vector<void*> allocationsToFree;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        if (modInfos[i].pVertexInputState) {
            modVIStates[i] = *modInfos[i].pVertexInputState;
        }
        if (m_fill_mode_mod && m_fill_mode_mod->is_enabled()) {
            m_fill_mode_mod->on_modify_pipeline_create_info(device, i, modInfos[i], modVIStates[i], allocationsToFree);
        }
        if (m_divisor_mod && m_divisor_mod->is_enabled()) {
            m_divisor_mod->on_modify_pipeline_create_info(device, i, modInfos[i], modVIStates[i], allocationsToFree);
        }
        if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
            m_dyn_rendering_mod->on_modify_pipeline_create_info(device, i, modInfos[i], modVIStates[i], allocationsToFree);
        }
    }

    VkResult res = real_fn(device, effectiveCache, createInfoCount, modInfos, pAllocator, pPipelines);

    if (res == VK_SUCCESS && pPipelines) {
        PipelineCacheManager::get().on_pipelines_created(device, effectiveCache, createInfoCount);
        if (m_divisor_mod && m_divisor_mod->is_enabled()) {
            m_divisor_mod->on_post_create_graphics_pipelines(device, createInfoCount, modInfos, pPipelines);
        }
    } else if (res != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed with VkResult: %d", res);
    }

    for (void* ptr : allocationsToFree) {
        free(ptr);
    }
    if (modVIStates != stackVIStates) free(modVIStates);
    if (modInfos != stackInfos) free(modInfos);

    return res;
}

VkResult LayerManager::dispatch_create_compute_pipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkCreateComputePipelines real_fn = dt.CreateComputePipelines ? dt.CreateComputePipelines :
        (PFN_vkCreateComputePipelines) get_real_proc(get_last_instance(), device, "vkCreateComputePipelines");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkPipelineCache effectiveCache = PipelineCacheManager::get().prepare_pipeline_cache(device, pipelineCache);
    auto cacheGuard = PipelineCacheManager::get().lock_cache_for_creation(device, effectiveCache);
    VkResult res = real_fn(device, effectiveCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    if (res == VK_SUCCESS) {
        PipelineCacheManager::get().on_pipelines_created(device, effectiveCache, createInfoCount);
    }
    return res;
}

VkResult LayerManager::dispatch_create_pipeline_cache(
    VkDevice device,
    const VkPipelineCacheCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineCache* pPipelineCache
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkCreatePipelineCache real_fn = dt.CreatePipelineCache ? dt.CreatePipelineCache :
        (PFN_vkCreatePipelineCache) get_real_proc(get_last_instance(), device, "vkCreatePipelineCache");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(device, pCreateInfo, pAllocator, pPipelineCache);
    if (res == VK_SUCCESS && pPipelineCache && *pPipelineCache != VK_NULL_HANDLE) {
        PipelineCacheManager::get().on_post_create_app_pipeline_cache(device, *pPipelineCache);
    }
    return res;
}

void LayerManager::dispatch_destroy_pipeline_cache(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const VkAllocationCallbacks* pAllocator
) {
    if (pipelineCache == VK_NULL_HANDLE) return;

    PipelineCacheManager::get().on_pre_destroy_app_pipeline_cache(device, pipelineCache);

    const auto& dt = get_dispatch_table(device);
    PFN_vkDestroyPipelineCache real_fn = dt.DestroyPipelineCache ? dt.DestroyPipelineCache :
        (PFN_vkDestroyPipelineCache) get_real_proc(get_last_instance(), device, "vkDestroyPipelineCache");
    if (real_fn) {
        real_fn(device, pipelineCache, pAllocator);
    }
}

VkResult LayerManager::dispatch_get_pipeline_cache_data(
    VkDevice device,
    VkPipelineCache pipelineCache,
    size_t* pDataSize,
    void* pData
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkGetPipelineCacheData real_fn = dt.GetPipelineCacheData ? dt.GetPipelineCacheData :
        (PFN_vkGetPipelineCacheData) get_real_proc(get_last_instance(), device, "vkGetPipelineCacheData");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    return real_fn(device, pipelineCache, pDataSize, pData);
}

VkResult LayerManager::dispatch_merge_pipeline_caches(
    VkDevice device,
    VkPipelineCache dstCache,
    uint32_t srcCacheCount,
    const VkPipelineCache* pSrcCaches
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkMergePipelineCaches real_fn = dt.MergePipelineCaches ? dt.MergePipelineCaches :
        (PFN_vkMergePipelineCaches) get_real_proc(get_last_instance(), device, "vkMergePipelineCaches");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    return real_fn(device, dstCache, srcCacheCount, pSrcCaches);
}

VkResult LayerManager::dispatch_create_descriptor_set_layout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkCreateDescriptorSetLayout real_fn = dt.CreateDescriptorSetLayout;
    if (!real_fn) {
        real_fn = (PFN_vkCreateDescriptorSetLayout) get_real_proc(get_last_instance(), device, "vkCreateDescriptorSetLayout");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pCreateInfo) return real_fn(device, pCreateInfo, pAllocator, pSetLayout);

    VkDescriptorSetLayoutCreateInfo modInfo = *pCreateInfo;
    std::vector<VkDescriptorSetLayoutBinding> compute_bindings;
    if (VertexStoreEmulator::get().enabled(device) && modInfo.bindingCount) {
        compute_bindings.assign(modInfo.pBindings, modInfo.pBindings + modInfo.bindingCount);
        for (auto& binding : compute_bindings) if (binding.stageFlags & VK_SHADER_STAGE_VERTEX_BIT) binding.stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT;
        modInfo.pBindings = compute_bindings.data();
    }

    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        m_push_desc_mod->on_pre_create_descriptor_set_layout(device, modInfo);
    }

    VkResult res = real_fn(device, &modInfo, pAllocator, pSetLayout);
    if (res == VK_SUCCESS) VertexStoreEmulator::get().set_layout(device, *pSetLayout, &modInfo);

    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        m_push_desc_mod->on_post_create_descriptor_set_layout(device, pCreateInfo, res, (res == VK_SUCCESS && pSetLayout) ? *pSetLayout : VK_NULL_HANDLE);
    }

    return res;
}

void LayerManager::dispatch_destroy_descriptor_set_layout(
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator
) {
    VertexStoreEmulator::get().erase_set_layout(device, descriptorSetLayout);

    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        m_push_desc_mod->on_destroy_descriptor_set_layout(device, descriptorSetLayout);
    }

    const auto& dt = get_dispatch_table(device);
    if (dt.DestroyDescriptorSetLayout) {
        dt.DestroyDescriptorSetLayout(device, descriptorSetLayout, pAllocator);
    } else {
        PFN_vkDestroyDescriptorSetLayout real_fn =
            (PFN_vkDestroyDescriptorSetLayout) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorSetLayout");
        if (real_fn) {
            real_fn(device, descriptorSetLayout, pAllocator);
        }
    }
}

VkResult LayerManager::dispatch_create_pipeline_layout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineLayout* pPipelineLayout
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkCreatePipelineLayout real_fn = dt.CreatePipelineLayout;
    if (!real_fn) {
        real_fn = (PFN_vkCreatePipelineLayout) get_real_proc(get_last_instance(), device, "vkCreatePipelineLayout");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(device, pCreateInfo, pAllocator, pPipelineLayout);
    if (res == VK_SUCCESS) VertexStoreEmulator::get().layout(device, *pPipelineLayout, pCreateInfo);

    if (res == VK_SUCCESS && pCreateInfo && pPipelineLayout) {
        if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
            m_push_desc_mod->on_post_create_pipeline_layout(device, pCreateInfo, res, *pPipelineLayout);
        }
    }

    return res;
}

void LayerManager::dispatch_destroy_pipeline_layout(
    VkDevice device,
    VkPipelineLayout pipelineLayout,
    const VkAllocationCallbacks* pAllocator
) {
    VertexStoreEmulator::get().erase_layout(device, pipelineLayout);

    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        m_push_desc_mod->on_destroy_pipeline_layout(device, pipelineLayout);
    }

    const auto& dt = get_dispatch_table(device);
    if (dt.DestroyPipelineLayout) {
        dt.DestroyPipelineLayout(device, pipelineLayout, pAllocator);
    } else {
        PFN_vkDestroyPipelineLayout real_fn =
            (PFN_vkDestroyPipelineLayout) get_real_proc(get_last_instance(), device, "vkDestroyPipelineLayout");
        if (real_fn) {
            real_fn(device, pipelineLayout, pAllocator);
        }
    }
}

VkResult LayerManager::dispatch_allocate_command_buffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkAllocateCommandBuffers real_fn = dt.AllocateCommandBuffers;
    if (!real_fn) {
        real_fn = (PFN_vkAllocateCommandBuffers) get_real_proc(get_last_instance(), device, "vkAllocateCommandBuffers");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(device, pAllocateInfo, pCommandBuffers);
    if (res == VK_SUCCESS) VertexStoreEmulator::get().allocate_commands(device, pAllocateInfo, pCommandBuffers);

    if (res == VK_SUCCESS && pAllocateInfo && pCommandBuffers) {
        {
            std::unique_lock<std::shared_mutex> lock(m_cmd_device_rw_mutex);
            for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
                m_cmd_devices[(uint64_t)(uintptr_t)pCommandBuffers[i]] = device;
            }
        }
        if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
            m_push_desc_mod->on_post_allocate_command_buffers(device, pAllocateInfo, res, pCommandBuffers);
        }
        if (m_divisor_mod && m_divisor_mod->is_enabled()) {
            m_divisor_mod->on_post_allocate_command_buffers(device, pAllocateInfo, res, pCommandBuffers);
        }
        if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
            m_dyn_rendering_mod->on_post_allocate_command_buffers(device, pAllocateInfo, res, pCommandBuffers);
        }
    }

    return res;
}

void LayerManager::dispatch_free_command_buffers(
    VkDevice device,
    VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers
) {
    VertexStoreEmulator::get().free_commands(device, commandBufferCount, pCommandBuffers);

    if (pCommandBuffers) {
        std::unique_lock<std::shared_mutex> lock(m_cmd_device_rw_mutex);
        for (uint32_t i = 0; i < commandBufferCount; i++) {
            m_cmd_devices.erase((uint64_t)(uintptr_t)pCommandBuffers[i]);
            TBDRBarrierOptimizer::get().on_cmd_free(pCommandBuffers[i]);
        }
    }

    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        m_push_desc_mod->on_free_command_buffers(device, commandBufferCount, pCommandBuffers);
    }
    if (m_divisor_mod && m_divisor_mod->is_enabled()) {
        m_divisor_mod->on_free_command_buffers(device, commandBufferCount, pCommandBuffers);
    }
    if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
        m_dyn_rendering_mod->on_free_command_buffers(device, commandBufferCount, pCommandBuffers);
    }

    const auto& dt = get_dispatch_table(device);
    if (dt.FreeCommandBuffers) {
        dt.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
    } else {
        PFN_vkFreeCommandBuffers real_fn =
            (PFN_vkFreeCommandBuffers) get_real_proc(get_last_instance(), device, "vkFreeCommandBuffers");
        if (real_fn) {
            real_fn(device, commandPool, commandBufferCount, pCommandBuffers);
        }
    }
}

VkResult LayerManager::dispatch_begin_command_buffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo
) {
    VertexStoreEmulator::get().clear_commands(commandBuffer);

    TBDRBarrierOptimizer::get().on_cmd_begin(commandBuffer);

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (!dt.BeginCommandBuffer) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pBeginInfo) {
        return dt.BeginCommandBuffer(commandBuffer, pBeginInfo);
    }

    VkCommandBufferBeginInfo modBeginInfo = *pBeginInfo;
    VkCommandBufferInheritanceInfo modInheritanceInfo{};
    bool has_mod_inheritance = false;

    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        m_push_desc_mod->on_begin_command_buffer(commandBuffer, pBeginInfo);
    }
    if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
        m_dyn_rendering_mod->on_begin_command_buffer(commandBuffer, pBeginInfo);
        m_dyn_rendering_mod->on_pre_begin_command_buffer(commandBuffer, pBeginInfo, modBeginInfo, modInheritanceInfo, has_mod_inheritance);
    }

    return dt.BeginCommandBuffer(commandBuffer, has_mod_inheritance ? &modBeginInfo : pBeginInfo);
}

VkResult LayerManager::dispatch_reset_command_buffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags
) {
    VertexStoreEmulator::get().clear_commands(commandBuffer);

    TBDRBarrierOptimizer::get().on_cmd_reset(commandBuffer);

    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        m_push_desc_mod->on_reset_command_buffer(commandBuffer, flags);
    }
    if (m_divisor_mod && m_divisor_mod->is_enabled()) {
        m_divisor_mod->on_reset_command_buffer(commandBuffer, flags);
    }
    if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
        m_dyn_rendering_mod->on_reset_command_buffer(commandBuffer, flags);
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.ResetCommandBuffer) {
        return dt.ResetCommandBuffer(commandBuffer, flags);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult LayerManager::dispatch_create_descriptor_update_template(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate
) {
    if (!pCreateInfo || !pDescriptorUpdateTemplate) return VK_ERROR_INITIALIZATION_FAILED;
    auto complete = [&](VkResult result) {
        if (m_push_desc_mod && m_push_desc_mod->is_enabled())
            m_push_desc_mod->on_post_create_descriptor_update_template(device, pCreateInfo,
                result, result == VK_SUCCESS ? *pDescriptorUpdateTemplate : VK_NULL_HANDLE);
        return result;
    };

    VkDescriptorUpdateTemplateCreateInfo modInfo = *pCreateInfo;
    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        m_push_desc_mod->on_pre_create_descriptor_update_template(device, modInfo);
    }

    VkResult res = VK_SUCCESS;
    if (m_desc_template_mod && m_desc_template_mod->is_enabled()) {
        if (m_desc_template_mod->on_create_descriptor_update_template(
                device, &modInfo, pAllocator, pDescriptorUpdateTemplate, res)) {
            return complete(res);
        }
    }

    PFN_vkCreateDescriptorUpdateTemplate real_fn =
        (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplateKHR");
    }
    if (real_fn) {
        return complete(real_fn(device, &modInfo, pAllocator, pDescriptorUpdateTemplate));
    }

    return VK_ERROR_INITIALIZATION_FAILED;
}

void LayerManager::dispatch_destroy_descriptor_update_template(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks* pAllocator
) {
    if (m_push_desc_mod && m_push_desc_mod->is_enabled())
        m_push_desc_mod->on_destroy_descriptor_update_template(device, descriptorUpdateTemplate,
                                                              pAllocator);
    if (m_desc_template_mod && m_desc_template_mod->is_enabled()) {
        if (m_desc_template_mod->on_destroy_descriptor_update_template(
                device, descriptorUpdateTemplate, pAllocator)) {
            return;
        }
    }

    PFN_vkDestroyDescriptorUpdateTemplate real_fn =
        (PFN_vkDestroyDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorUpdateTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkDestroyDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorUpdateTemplateKHR");
    }
    if (real_fn) {
        real_fn(device, descriptorUpdateTemplate, pAllocator);
    }
}

VkResult LayerManager::command_recording_error(VkCommandBuffer cmd) {
    return m_push_desc_mod && m_push_desc_mod->is_enabled() ?
        m_push_desc_mod->command_recording_error(cmd) : VK_SUCCESS;
}

void LayerManager::track_descriptor_binding(VkCommandBuffer cmd, VkPipelineBindPoint point,
    VkPipelineLayout layout, uint32_t first, uint32_t count) {
    if (m_push_desc_mod && m_push_desc_mod->is_enabled())
        m_push_desc_mod->on_cmd_bind_descriptor_sets(cmd, point, layout, first, count);
}

void LayerManager::track_command_pool_reset(VkDevice device, VkCommandPool pool, bool destroyed) {
    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        if (destroyed) m_push_desc_mod->on_destroy_command_pool(device, pool);
        else m_push_desc_mod->on_reset_command_pool(device, pool);
    }
}

void LayerManager::dispatch_cmd_push_descriptor_set(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t set,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites
) {
    // Descriptor writes are command state, not barrier-relevant memory access.

    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        if (m_push_desc_mod->on_cmd_push_descriptor_set(
                commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdPushDescriptorSetKHR) {
        dt.CmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
    }
}

void LayerManager::dispatch_cmd_push_descriptor_set_with_template(
    VkCommandBuffer commandBuffer,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    VkPipelineLayout layout,
    uint32_t set,
    const void* pData
) {
    // Descriptor writes are command state, not barrier-relevant memory access.

    if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
        if (m_push_desc_mod->on_cmd_push_descriptor_set_with_template(
                commandBuffer, descriptorUpdateTemplate, layout, set, pData)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    PFN_vkCmdPushDescriptorSetWithTemplateKHR real_fn =
        (PFN_vkCmdPushDescriptorSetWithTemplateKHR) get_real_proc(get_last_instance(), device, "vkCmdPushDescriptorSetWithTemplateKHR");
    if (real_fn) {
        real_fn(commandBuffer, descriptorUpdateTemplate, layout, set, pData);
    }
}

VkResult LayerManager::dispatch_create_image(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkCreateImage real_fn = dt.CreateImage;
    if (!real_fn) {
        real_fn = (PFN_vkCreateImage) get_real_proc(get_last_instance(), device, "vkCreateImage");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(device, pCreateInfo, pAllocator, pImage);
    if (res == VK_SUCCESS) VertexStoreEmulator::get().image(device, *pImage, pCreateInfo);
    if (res == VK_SUCCESS && pCreateInfo && pImage && *pImage != VK_NULL_HANDLE) {
        if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
            m_dyn_rendering_mod->on_post_create_image(device, pCreateInfo, res, *pImage);
        }
    }
    return res;
}

void LayerManager::dispatch_destroy_image(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator
) {
    VertexStoreEmulator::get().erase_image(device, image);

    if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
        m_dyn_rendering_mod->on_destroy_image(device, image);
    }

    const auto& dt = get_dispatch_table(device);
    if (dt.DestroyImage) {
        dt.DestroyImage(device, image, pAllocator);
    } else {
        PFN_vkDestroyImage real_fn =
            (PFN_vkDestroyImage) get_real_proc(get_last_instance(), device, "vkDestroyImage");
        if (real_fn) {
            real_fn(device, image, pAllocator);
        }
    }
}

VkResult LayerManager::dispatch_create_image_view(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkCreateImageView real_fn = dt.CreateImageView;
    if (!real_fn) {
        real_fn = (PFN_vkCreateImageView) get_real_proc(get_last_instance(), device, "vkCreateImageView");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(device, pCreateInfo, pAllocator, pView);
    if (res == VK_SUCCESS) VertexStoreEmulator::get().view(device, *pView, pCreateInfo);
    if (res == VK_SUCCESS && pCreateInfo && pView && *pView != VK_NULL_HANDLE) {
        if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
            m_dyn_rendering_mod->on_post_create_image_view(device, pCreateInfo, res, *pView);
        }
    }
    return res;
}

void LayerManager::dispatch_destroy_image_view(
    VkDevice device,
    VkImageView imageView,
    const VkAllocationCallbacks* pAllocator
) {
    VertexStoreEmulator::get().erase_view(device, imageView);

    if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
        m_dyn_rendering_mod->on_destroy_image_view(device, imageView);
    }

    // Cached imageless framebuffers reference the view; drop them before the
    // driver destroys it.
    ImagelessFramebufferManager::get().on_destroy_image_view(device, imageView);

    const auto& dt = get_dispatch_table(device);
    if (dt.DestroyImageView) {
        dt.DestroyImageView(device, imageView, pAllocator);
    } else {
        PFN_vkDestroyImageView real_fn =
            (PFN_vkDestroyImageView) get_real_proc(get_last_instance(), device, "vkDestroyImageView");
        if (real_fn) {
            real_fn(device, imageView, pAllocator);
        }
    }
}

VkResult LayerManager::dispatch_create_sampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkCreateSampler real_fn = dt.CreateSampler;
    if (!real_fn) {
        real_fn = (PFN_vkCreateSampler) get_real_proc(get_last_instance(), device, "vkCreateSampler");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pCreateInfo) return real_fn(device, pCreateInfo, pAllocator, pSampler);

    VkSamplerCreateInfo modInfo = *pCreateInfo;
    if (m_sampler_anisotropy_mod && m_sampler_anisotropy_mod->is_enabled()) {
        m_sampler_anisotropy_mod->on_pre_create_sampler(device, modInfo);
    }

    return real_fn(device, &modInfo, pAllocator, pSampler);
}

void LayerManager::dispatch_destroy_sampler(
    VkDevice device,
    VkSampler sampler,
    const VkAllocationCallbacks* pAllocator
) {
    if (sampler == VK_NULL_HANDLE) return;

    const auto& dt = get_dispatch_table(device);
    if (dt.DestroySampler) {
        dt.DestroySampler(device, sampler, pAllocator);
    } else {
        PFN_vkDestroySampler real_fn =
            (PFN_vkDestroySampler) get_real_proc(get_last_instance(), device, "vkDestroySampler");
        if (real_fn) {
            real_fn(device, sampler, pAllocator);
        }
    }
}

void LayerManager::dispatch_cmd_begin_rendering(
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo* pRenderingInfo
) {
    pRenderingInfo = VertexStoreEmulator::get().begin_rendering(commandBuffer, pRenderingInfo);

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
        if (m_dyn_rendering_mod->on_cmd_begin_rendering(commandBuffer, pRenderingInfo)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdBeginRendering) {
        dt.CmdBeginRendering(commandBuffer, pRenderingInfo);
    }
}

void LayerManager::dispatch_cmd_end_rendering(
    VkCommandBuffer commandBuffer
) {
    VertexStoreEmulator::get().end_rendering(commandBuffer);

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (m_dyn_rendering_mod && m_dyn_rendering_mod->is_enabled()) {
        if (m_dyn_rendering_mod->on_cmd_end_rendering(commandBuffer)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdEndRendering) {
        dt.CmdEndRendering(commandBuffer);
    }
}

void LayerManager::dispatch_destroy_pipeline(
    VkDevice device,
    VkPipeline pipeline,
    const VkAllocationCallbacks* pAllocator
) {
    VertexStoreEmulator::get().erase_pipeline(device, pipeline);

    if (m_divisor_mod && m_divisor_mod->is_enabled()) {
        m_divisor_mod->on_destroy_pipeline(device, pipeline);
    }

    const auto& dt = get_dispatch_table(device);
    if (dt.DestroyPipeline) {
        dt.DestroyPipeline(device, pipeline, pAllocator);
    }
}

void LayerManager::dispatch_cmd_bind_pipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline
) {
    VertexStoreEmulator::get().bind_pipeline(commandBuffer, pipelineBindPoint, pipeline);

    // Binding state does not access memory. Do not mark it as a barrier action:
    // identical barriers separated only by binds can still be deduplicated.

    if (__builtin_expect(m_device_count.load(std::memory_order_relaxed) == 1 &&
                         m_divisor_bypass_active.load(std::memory_order_relaxed), 1)) {
        if (__builtin_expect(m_has_primary_table.load(std::memory_order_relaxed), 1)) {
            const auto& primary_dt = get_dispatch_table(get_device_for_cmd(commandBuffer));
            primary_dt.CmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
            return;
        }
    }

    if (m_divisor_mod && m_divisor_mod->is_enabled()) {
        m_divisor_mod->on_cmd_bind_pipeline(commandBuffer, pipelineBindPoint, pipeline);
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdBindPipeline) {
        dt.CmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
    }
}

void LayerManager::dispatch_cmd_bind_vertex_buffers(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets
) {
    VertexStoreEmulator::get().bind_vertices(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);

    // Vertex buffer binding is not a memory access; keep the TBDR dedup window open.

    if (__builtin_expect(m_device_count.load(std::memory_order_relaxed) == 1 &&
                         m_divisor_bypass_active.load(std::memory_order_relaxed), 1)) {
        if (__builtin_expect(m_has_primary_table.load(std::memory_order_relaxed), 1)) {
            const auto& primary_dt = get_dispatch_table(get_device_for_cmd(commandBuffer));
            primary_dt.CmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
            return;
        }
    }

    if (m_divisor_mod && m_divisor_mod->is_enabled()) {
        m_divisor_mod->on_cmd_bind_vertex_buffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdBindVertexBuffers) {
        dt.CmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
    }
}

void LayerManager::dispatch_cmd_bind_vertex_buffers2(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets,
    const VkDeviceSize* pSizes,
    const VkDeviceSize* pStrides
) {
    VertexStoreEmulator::get().bind_vertices(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pStrides);

    // Vertex buffer binding is not a memory access; keep the TBDR dedup window open.

    if (__builtin_expect(m_device_count.load(std::memory_order_relaxed) == 1 &&
                         m_divisor_bypass_active.load(std::memory_order_relaxed), 1)) {
        if (__builtin_expect(m_has_primary_table.load(std::memory_order_relaxed), 1)) {
            const auto& primary_dt = get_dispatch_table(get_device_for_cmd(commandBuffer));
            if (primary_dt.CmdBindVertexBuffers2) {
                primary_dt.CmdBindVertexBuffers2(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
            } else if (primary_dt.CmdBindVertexBuffers) {
                primary_dt.CmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
            }
            return;
        }
    }

    if (m_divisor_mod && m_divisor_mod->is_enabled()) {
        if (pStrides != nullptr && bindingCount > 0) {
            // The divisor emulation rewrites divisor bindings to stride 0 and
            // applies the element offset per sub-draw. A dynamic stride from
            // vkCmdBindVertexBuffers2 would override that, so let the module
            // zero the strides for divisor bindings before forwarding.
            std::vector<VkDeviceSize> adjusted(pStrides, pStrides + bindingCount);
            m_divisor_mod->on_cmd_bind_vertex_buffers2(
                commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, adjusted.data());
            VkDevice device = get_device_for_cmd(commandBuffer);
            const auto& dt = get_dispatch_table(device);
            if (dt.CmdBindVertexBuffers2) {
                dt.CmdBindVertexBuffers2(commandBuffer, firstBinding, bindingCount,
                                         pBuffers, pOffsets, pSizes, adjusted.data());
                return;
            }
            // No vkCmdBindVertexBuffers2 on the driver: the dynamic strides are
            // dropped together with vkCmdBindVertexBuffers, matching the 1.0
            // fallback semantics used below.
        } else {
            m_divisor_mod->on_cmd_bind_vertex_buffers(
                commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdBindVertexBuffers2) {
        dt.CmdBindVertexBuffers2(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
    } else if (dt.CmdBindVertexBuffers) {
        dt.CmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
    }
}

void LayerManager::block_tiler_split(VkCommandBuffer commandBuffer, bool begin) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->block_tiler_split(commandBuffer, begin);
}

void LayerManager::track_render_pass(VkDevice device, const VkRenderPassCreateInfo* info,
                                   VkRenderPass pass) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->track_render_pass(device, info, pass);
}

void LayerManager::forget_render_pass(VkDevice device, VkRenderPass pass) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->forget_render_pass(device, pass);
}

void LayerManager::dispatch_cmd_draw(
    VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->before_draw(commandBuffer,
        uint64_t(vertexCount) * instanceCount);
    if (VertexStoreEmulator::get().draw(commandBuffer, vertexCount, instanceCount, firstVertex, 0, firstInstance, false)) return;

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (__builtin_expect(m_device_count.load(std::memory_order_relaxed) == 1 &&
                         m_divisor_bypass_active.load(std::memory_order_relaxed), 1)) {
        if (__builtin_expect(m_has_primary_table.load(std::memory_order_relaxed), 1)) {
            const auto& primary_dt = get_dispatch_table(get_device_for_cmd(commandBuffer));
            primary_dt.CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
            return;
        }
    }

    if (m_divisor_mod && m_divisor_mod->is_enabled()) {
        if (m_divisor_mod->on_cmd_draw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdDraw) {
        dt.CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }
}

void LayerManager::dispatch_cmd_draw_indexed(
    VkCommandBuffer commandBuffer,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->before_draw(commandBuffer,
        uint64_t(indexCount) * instanceCount);
    if (VertexStoreEmulator::get().draw(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance, true)) return;

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (__builtin_expect(m_device_count.load(std::memory_order_relaxed) == 1 &&
                         m_divisor_bypass_active.load(std::memory_order_relaxed), 1)) {
        if (__builtin_expect(m_has_primary_table.load(std::memory_order_relaxed), 1)) {
            const auto& primary_dt = get_dispatch_table(get_device_for_cmd(commandBuffer));
            primary_dt.CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
            return;
        }
    }

    if (m_divisor_mod && m_divisor_mod->is_enabled()) {
        if (m_divisor_mod->on_cmd_draw_indexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdDrawIndexed) {
        dt.CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }
}

void LayerManager::dispatch_cmd_draw_indirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->before_draw(commandBuffer, 0, drawCount);
    VertexStoreEmulator::get().indirect(commandBuffer);
    if (VertexStoreEmulator::get().recording_error(commandBuffer) != VK_SUCCESS) return;

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (m_divisor_mod && m_divisor_mod->is_enabled() &&
        m_divisor_mod->has_active_divisor_pipeline(commandBuffer)) {
        static std::atomic<uint64_t> warned{0};
        if (warned.fetch_add(1, std::memory_order_relaxed) == 0) {
            LOGW("VertexAttributeDivisor: indirect draw uses a divisor>1 pipeline; indirect draws are not expanded, instanced attributes may be incorrect");
        }
    }

    if (__builtin_expect(m_device_count.load(std::memory_order_relaxed) == 1 &&
                         m_has_primary_table.load(std::memory_order_relaxed) &&
                         m_primary_multi_draw_bypass.load(std::memory_order_relaxed), 1)) {
        const auto& primary_dt = get_dispatch_table(get_device_for_cmd(commandBuffer));
        if (primary_dt.CmdDrawIndirect) {
            primary_dt.CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
            return;
        }
    }

    if (m_multi_draw_indirect_mod && m_multi_draw_indirect_mod->is_enabled()) {
        if (m_multi_draw_indirect_mod->on_cmd_draw_indirect(commandBuffer, buffer, offset, drawCount, stride)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdDrawIndirect) {
        dt.CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }
}

void LayerManager::dispatch_cmd_draw_indexed_indirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->before_draw(commandBuffer, 0, drawCount);
    VertexStoreEmulator::get().indirect(commandBuffer);
    if (VertexStoreEmulator::get().recording_error(commandBuffer) != VK_SUCCESS) return;

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (m_divisor_mod && m_divisor_mod->is_enabled() &&
        m_divisor_mod->has_active_divisor_pipeline(commandBuffer)) {
        static std::atomic<uint64_t> warned{0};
        if (warned.fetch_add(1, std::memory_order_relaxed) == 0) {
            LOGW("VertexAttributeDivisor: indirect draw uses a divisor>1 pipeline; indirect draws are not expanded, instanced attributes may be incorrect");
        }
    }

    if (__builtin_expect(m_device_count.load(std::memory_order_relaxed) == 1 &&
                         m_has_primary_table.load(std::memory_order_relaxed) &&
                         m_primary_multi_draw_bypass.load(std::memory_order_relaxed), 1)) {
        const auto& primary_dt = get_dispatch_table(get_device_for_cmd(commandBuffer));
        if (primary_dt.CmdDrawIndexedIndirect) {
            primary_dt.CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
            return;
        }
    }

    if (m_multi_draw_indirect_mod && m_multi_draw_indirect_mod->is_enabled()) {
        if (m_multi_draw_indirect_mod->on_cmd_draw_indexed_indirect(commandBuffer, buffer, offset, drawCount, stride)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdDrawIndexedIndirect) {
        dt.CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }
}

void LayerManager::dispatch_cmd_begin_render_pass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    VkSubpassContents contents
) {
    VertexStoreEmulator::get().legacy_rendering(commandBuffer, true);

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);

    VkRenderPassBeginInfo resolved{};
    const VkRenderPassBeginInfo* begin = pRenderPassBegin;
    const auto imageless = ImagelessFramebufferManager::get().resolve(pRenderPassBegin, resolved);
    if (imageless == ImagelessFramebufferManager::ResolveResult::Failed) {
        LOGE("vkCmdBeginRenderPass: imageless framebuffer could not be resolved; skipping submission");
        return;
    }
    if (imageless == ImagelessFramebufferManager::ResolveResult::Resolved) {
        begin = &resolved;
    }

    if (dt.CmdBeginRenderPass) {
        if (m_dyn_rendering_mod)
            m_dyn_rendering_mod->begin_legacy_rendering(commandBuffer, begin, contents);
        dt.CmdBeginRenderPass(commandBuffer, begin, contents);
    }
}

void LayerManager::dispatch_cmd_next_subpass(
    VkCommandBuffer commandBuffer,
    VkSubpassContents contents
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->end_legacy_rendering(commandBuffer);
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdNextSubpass) {
        dt.CmdNextSubpass(commandBuffer, contents);
    }
}

void LayerManager::dispatch_cmd_end_render_pass(
    VkCommandBuffer commandBuffer
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->end_legacy_rendering(commandBuffer);
    VertexStoreEmulator::get().legacy_rendering(commandBuffer, false);

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdEndRenderPass) {
        dt.CmdEndRenderPass(commandBuffer);
    }
}

void LayerManager::dispatch_cmd_dispatch(
    VkCommandBuffer commandBuffer,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdDispatch) {
        dt.CmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
    }
}

void LayerManager::dispatch_cmd_dispatch_indirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdDispatchIndirect) {
        dt.CmdDispatchIndirect(commandBuffer, buffer, offset);
    }
}

void LayerManager::dispatch_cmd_copy_buffer(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkBuffer dstBuffer,
    uint32_t regionCount,
    const VkBufferCopy* pRegions
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdCopyBuffer) {
        dt.CmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
    }
}

void LayerManager::dispatch_cmd_copy_image(
    VkCommandBuffer commandBuffer,
    VkImage srcImage,
    VkImageLayout srcImageLayout,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkImageCopy* pRegions
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdCopyImage) {
        dt.CmdCopyImage(commandBuffer, srcImage, LayoutCompat::sanitize(srcImageLayout),
                        dstImage, LayoutCompat::sanitize(dstImageLayout), regionCount, pRegions);
    }
}

void LayerManager::dispatch_cmd_blit_image(
    VkCommandBuffer commandBuffer,
    VkImage srcImage,
    VkImageLayout srcImageLayout,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkImageBlit* pRegions,
    VkFilter filter
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdBlitImage) {
        dt.CmdBlitImage(commandBuffer, srcImage, LayoutCompat::sanitize(srcImageLayout),
                        dstImage, LayoutCompat::sanitize(dstImageLayout), regionCount, pRegions, filter);
    }
}

void LayerManager::dispatch_cmd_clear_attachments(
    VkCommandBuffer commandBuffer,
    uint32_t attachmentCount,
    const VkClearAttachment* pAttachments,
    uint32_t rectCount,
    const VkClearRect* pRects
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdClearAttachments) {
        dt.CmdClearAttachments(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);
    }
}

void LayerManager::dispatch_cmd_clear_color_image(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout imageLayout,
    const VkClearColorValue* pColor,
    uint32_t rangeCount,
    const VkImageSubresourceRange* pRanges
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdClearColorImage) {
        dt.CmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
    }
}

void LayerManager::dispatch_cmd_clear_depth_stencil_image(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout imageLayout,
    const VkClearDepthStencilValue* pDepthStencil,
    uint32_t rangeCount,
    const VkImageSubresourceRange* pRanges
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdClearDepthStencilImage) {
        dt.CmdClearDepthStencilImage(commandBuffer, image, LayoutCompat::sanitize(imageLayout),
                                     pDepthStencil, rangeCount, pRanges);
    }
}

void LayerManager::dispatch_cmd_execute_commands(
    VkCommandBuffer commandBuffer,
    uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdExecuteCommands) {
        dt.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
    }
}




void LayerManager::dispatch_get_device_queue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkGetDeviceQueue real_fn = dt.GetDeviceQueue;
    if (!real_fn) {
        real_fn = (PFN_vkGetDeviceQueue) get_real_proc(get_last_instance(), device, "vkGetDeviceQueue");
    }
    if (real_fn) {
        real_fn(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue != VK_NULL_HANDLE) {
            std::unique_lock<std::shared_mutex> lock(m_cmd_device_rw_mutex);
            m_queue_devices[(uint64_t)(uintptr_t)*pQueue] = device;
            m_device_queues[(uint64_t)(uintptr_t)device] = {*pQueue, queueFamilyIndex};
        }
    }
}

void LayerManager::dispatch_get_device_queue2(
    VkDevice device,
    const VkDeviceQueueInfo2* pQueueInfo,
    VkQueue* pQueue
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkGetDeviceQueue2 real_fn = dt.GetDeviceQueue2;
    if (!real_fn) {
        real_fn = (PFN_vkGetDeviceQueue2) get_real_proc(get_last_instance(), device, "vkGetDeviceQueue2");
    }
    if (real_fn) {
        real_fn(device, pQueueInfo, pQueue);
    } else if (pQueueInfo && pQueue && pQueueInfo->flags == 0) {
        // Vulkan 1.0 fallback: vkGetDeviceQueue cannot express queue flags, but
        // flags == 0 is the common case for every queue.
        PFN_vkGetDeviceQueue real_get =
            (PFN_vkGetDeviceQueue) get_real_proc(get_last_instance(), device, "vkGetDeviceQueue");
        if (real_get) {
            real_get(device, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex, pQueue);
        }
    }

    if (pQueue && *pQueue != VK_NULL_HANDLE && pQueueInfo) {
        std::unique_lock<std::shared_mutex> lock(m_cmd_device_rw_mutex);
        m_queue_devices[(uint64_t)(uintptr_t)*pQueue] = device;
        m_device_queues[(uint64_t)(uintptr_t)device] = {*pQueue, pQueueInfo->queueFamilyIndex};
    }
}

bool LayerManager::get_device_queue_info(VkDevice device, VkQueue& outQueue, uint32_t& outQueueFamily) {
    std::shared_lock<std::shared_mutex> lock(m_cmd_device_rw_mutex);
    auto it = m_device_queues.find((uint64_t)(uintptr_t)device);
    if (it != m_device_queues.end()) {
        outQueue = it->second.first;
        outQueueFamily = it->second.second;
        return true;
    }
    return false;
}

void LayerManager::dispatch_cmd_set_event2(
    VkCommandBuffer commandBuffer,
    VkEvent event,
    const VkDependencyInfo* pDependencyInfo
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    if (m_sync2_mod && m_sync2_mod->is_enabled()) {
        if (m_sync2_mod->on_cmd_set_event2(commandBuffer, event, pDependencyInfo)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    PFN_vkCmdSetEvent2KHR real_fn =
        (PFN_vkCmdSetEvent2KHR) get_real_proc(get_last_instance(), device, "vkCmdSetEvent2KHR");
    if (!real_fn) {
        real_fn = (PFN_vkCmdSetEvent2KHR) get_real_proc(get_last_instance(), device, "vkCmdSetEvent2");
    }
    if (real_fn) {
        real_fn(commandBuffer, event, pDependencyInfo);
    }
}

void LayerManager::dispatch_cmd_reset_event2(
    VkCommandBuffer commandBuffer,
    VkEvent event,
    VkPipelineStageFlags2 stageMask
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    if (m_sync2_mod && m_sync2_mod->is_enabled()) {
        if (m_sync2_mod->on_cmd_reset_event2(commandBuffer, event, stageMask)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    PFN_vkCmdResetEvent2KHR real_fn =
        (PFN_vkCmdResetEvent2KHR) get_real_proc(get_last_instance(), device, "vkCmdResetEvent2KHR");
    if (!real_fn) {
        real_fn = (PFN_vkCmdResetEvent2KHR) get_real_proc(get_last_instance(), device, "vkCmdResetEvent2");
    }
    if (real_fn) {
        real_fn(commandBuffer, event, stageMask);
    }
}

void LayerManager::dispatch_cmd_wait_events2(
    VkCommandBuffer commandBuffer,
    uint32_t eventCount,
    const VkEvent* pEvents,
    const VkDependencyInfo* pDependencyInfos
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    if (m_sync2_mod && m_sync2_mod->is_enabled()) {
        if (m_sync2_mod->on_cmd_wait_events2(commandBuffer, eventCount, pEvents, pDependencyInfos)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    PFN_vkCmdWaitEvents2KHR real_fn =
        (PFN_vkCmdWaitEvents2KHR) get_real_proc(get_last_instance(), device, "vkCmdWaitEvents2KHR");
    if (!real_fn) {
        real_fn = (PFN_vkCmdWaitEvents2KHR) get_real_proc(get_last_instance(), device, "vkCmdWaitEvents2");
    }
    if (real_fn) {
        real_fn(commandBuffer, eventCount, pEvents, pDependencyInfos);
    }
}

void LayerManager::dispatch_cmd_pipeline_barrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount,
    const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers
) {
    if (__builtin_expect(TBDRBarrierOptimizer::get().is_enabled(), 0)) {
        static thread_local TBDRBarrierOptimizer::ScratchStorage1 storage;
        if (!TBDRBarrierOptimizer::get().optimize_pipeline_barrier1(
                commandBuffer, srcStageMask, dstStageMask, dependencyFlags,
                memoryBarrierCount, pMemoryBarriers,
                bufferMemoryBarrierCount, pBufferMemoryBarriers,
                imageMemoryBarrierCount, pImageMemoryBarriers,
                storage)) {
            return; // Dropped by TBDR optimizer!
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);

    const VkImageMemoryBarrier* image_barriers = pImageMemoryBarriers;
    static thread_local std::vector<VkImageMemoryBarrier> sanitized_barriers;
    sanitized_barriers.clear();
    if (LayoutCompat::get().any_emulated() && imageMemoryBarrierCount > 0 && pImageMemoryBarriers != nullptr) {
        sanitized_barriers.assign(pImageMemoryBarriers, pImageMemoryBarriers + imageMemoryBarrierCount);
        for (auto& barrier : sanitized_barriers) {
            barrier.oldLayout = LayoutCompat::sanitize(barrier.oldLayout);
            barrier.newLayout = LayoutCompat::sanitize(barrier.newLayout);
        }
        image_barriers = sanitized_barriers.data();
    }

    if (dt.CmdPipelineBarrier) {
        dt.CmdPipelineBarrier(
            commandBuffer,
            srcStageMask,
            dstStageMask,
            dependencyFlags,
            memoryBarrierCount,
            pMemoryBarriers,
            bufferMemoryBarrierCount,
            pBufferMemoryBarriers,
            imageMemoryBarrierCount,
            image_barriers);
    }
}

void LayerManager::dispatch_cmd_pipeline_barrier2(
    VkCommandBuffer commandBuffer,
    const VkDependencyInfo* pDependencyInfo
) {
    const VkDependencyInfo* pInfoToUse = pDependencyInfo;
    static thread_local TBDRBarrierOptimizer::ScratchStorage2 storage;
    VkDependencyInfo optInfo{};
    if (__builtin_expect(TBDRBarrierOptimizer::get().is_enabled(), 0)) {
        if (!TBDRBarrierOptimizer::get().optimize_dependency_info(commandBuffer, pDependencyInfo, optInfo, storage)) {
            return; // Dropped by TBDR optimizer!
        }
        pInfoToUse = &optInfo;
    }

    const VkDependencyInfo* info = pInfoToUse;
    VkDependencyInfo sanitized_info{};
    static thread_local std::vector<VkImageMemoryBarrier2> sanitized_barriers;
    sanitized_barriers.clear();
    if (LayoutCompat::get().any_emulated() && info != nullptr &&
        info->imageMemoryBarrierCount > 0 && info->pImageMemoryBarriers != nullptr) {
        sanitized_barriers.assign(
            info->pImageMemoryBarriers, info->pImageMemoryBarriers + info->imageMemoryBarrierCount);
        for (auto& barrier : sanitized_barriers) {
            barrier.oldLayout = LayoutCompat::sanitize(barrier.oldLayout);
            barrier.newLayout = LayoutCompat::sanitize(barrier.newLayout);
        }
        sanitized_info = *info;
        sanitized_info.pImageMemoryBarriers = sanitized_barriers.data();
        info = &sanitized_info;
    }

    if (m_sync2_mod && m_sync2_mod->is_enabled()) {
        if (m_sync2_mod->on_cmd_pipeline_barrier2(commandBuffer, info)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdPipelineBarrier2) {
        dt.CmdPipelineBarrier2(commandBuffer, info);
    }
}

void LayerManager::dispatch_cmd_write_timestamp2(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags2 stage,
    VkQueryPool queryPool,
    uint32_t query
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    if (m_sync2_mod && m_sync2_mod->is_enabled()) {
        if (m_sync2_mod->on_cmd_write_timestamp2(commandBuffer, stage, queryPool, query)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdWriteTimestamp2) {
        dt.CmdWriteTimestamp2(commandBuffer, stage, queryPool, query);
    } else if (dt.CmdWriteTimestamp) {
        dt.CmdWriteTimestamp(commandBuffer, (VkPipelineStageFlagBits)(stage & 0x0001FFFFULL), queryPool, query);
    }
}

VkResult LayerManager::dispatch_queue_submit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence
) {
    VkResult res = VK_SUCCESS;
    if (m_sync2_mod && m_sync2_mod->is_enabled()) {
        if (m_sync2_mod->on_queue_submit2(queue, submitCount, pSubmits, fence, res)) {
            return res;
        }
    }

    VkDevice device = get_device_for_queue(queue);
    const auto& dt = get_dispatch_table(device);
    if (dt.QueueSubmit2) {
        return dt.QueueSubmit2(queue, submitCount, pSubmits, fence);
    }

    // Safety fallback: if dt.QueueSubmit2 is NULL, force module emulation
    if (m_sync2_mod) {
        LOGW("vkQueueSubmit2: dt.QueueSubmit2 is NULL, forcing emulation fallback");
        if (m_sync2_mod->on_queue_submit2(queue, submitCount, pSubmits, fence, res)) {
            return res;
        }
    }

    LOGE("vkQueueSubmit2: neither handled by module nor found in native driver!");
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult LayerManager::dispatch_queue_submit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence
) {
    VkResult res = VK_SUCCESS;
    if (vkfix::TimelineScheduler::get().submit(queue, submitCount, pSubmits, fence, res)) return res;
    if (m_timeline_mod && m_timeline_mod->is_enabled()) {
        if (m_timeline_mod->on_queue_submit(queue, submitCount, pSubmits, fence, res)) {
            return res;
        }
    }

    VkDevice device = get_device_for_queue(queue);
    const auto& dt = get_dispatch_table(device);
    if (dt.QueueSubmit) {
        return dt.QueueSubmit(queue, submitCount, pSubmits, fence);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult LayerManager::dispatch_queue_wait_idle(VkQueue queue) {
    VkResult scheduled = VK_SUCCESS;
    if (vkfix::TimelineScheduler::get().queue_idle(queue, scheduled)) return scheduled;
    VkDevice device = get_device_for_queue(queue);
    const auto& dt = get_dispatch_table(device);
    PFN_vkQueueWaitIdle real_fn = dt.QueueWaitIdle ? dt.QueueWaitIdle :
        (PFN_vkQueueWaitIdle) get_real_proc(get_last_instance(), device, "vkQueueWaitIdle");
    VkResult res = real_fn ? real_fn(queue) : VK_SUCCESS;
    if (res == VK_SUCCESS) {
        if (m_timeline_mod && m_timeline_mod->is_enabled()) {
            m_timeline_mod->on_queue_wait_idle(queue);
        }
    }
    return res;
}

VkResult LayerManager::dispatch_device_wait_idle(VkDevice device) {
    VkResult scheduled = VK_SUCCESS;
    if (vkfix::TimelineScheduler::get().device_idle(device, scheduled)) return scheduled;
    const auto& dt = get_dispatch_table(device);
    PFN_vkDeviceWaitIdle real_fn = dt.DeviceWaitIdle ? dt.DeviceWaitIdle :
        (PFN_vkDeviceWaitIdle) get_real_proc(get_last_instance(), device, "vkDeviceWaitIdle");
    VkResult res = real_fn ? real_fn(device) : VK_SUCCESS;
    if (res == VK_SUCCESS) {
        if (m_timeline_mod && m_timeline_mod->is_enabled()) {
            m_timeline_mod->on_device_wait_idle(device);
        }
    }
    return res;
}

bool LayerManager::is_timeline_semaphore(VkDevice device, VkSemaphore semaphore) {
    if (semaphore == VK_NULL_HANDLE) return false;
    if (m_timeline_mod && m_timeline_mod->is_enabled()) {
        return m_timeline_mod->is_timeline_semaphore(device, semaphore);
    }
    return false;
}

VkResult LayerManager::dispatch_create_semaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSemaphore* pSemaphore
) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkCreateSemaphore real_fn = dt.CreateSemaphore;
    if (!real_fn) {
        real_fn = (PFN_vkCreateSemaphore) get_real_proc(get_last_instance(), device, "vkCreateSemaphore");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pCreateInfo) return real_fn(device, pCreateInfo, pAllocator, pSemaphore);

    VkSemaphoreCreateInfo modInfo = *pCreateInfo;
    vkfix::InputPNextCopy input;
    if (m_timeline_mod && m_timeline_mod->is_enabled() && !m_timeline_mod->is_device_native(device)) {
        auto copied = input.copy(modInfo.pNext, vkfix::InputChainKind::Semaphore);
        if (copied != VK_SUCCESS) return copied;
        modInfo.pNext = input.head();
    }
    void* user_data = nullptr;
    if (m_timeline_mod && m_timeline_mod->is_enabled()) {
        m_timeline_mod->on_pre_create_semaphore(device, modInfo, user_data);
    }

    VkResult res = real_fn(device, &modInfo, pAllocator, pSemaphore);

    if (m_timeline_mod && m_timeline_mod->is_enabled()) {
        try {
            m_timeline_mod->on_post_create_semaphore(device, pCreateInfo, res,
                res == VK_SUCCESS && pSemaphore ? *pSemaphore : VK_NULL_HANDLE, user_data);
        } catch (const std::bad_alloc&) {
            if (res==VK_SUCCESS && pSemaphore && *pSemaphore) {
                auto destroy=(PFN_vkDestroySemaphore)get_real_proc(get_last_instance(),device,"vkDestroySemaphore");
                if(destroy)destroy(device,*pSemaphore,pAllocator);
                *pSemaphore=VK_NULL_HANDLE;
            }
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    return res;
}

void LayerManager::dispatch_destroy_semaphore(
    VkDevice device,
    VkSemaphore semaphore,
    const VkAllocationCallbacks* pAllocator
) {
    if (m_timeline_mod && m_timeline_mod->is_enabled()) {
        m_timeline_mod->on_destroy_semaphore(device, semaphore);
    }

    const auto& dt = get_dispatch_table(device);
    if (dt.DestroySemaphore) {
        dt.DestroySemaphore(device, semaphore, pAllocator);
    } else {
        PFN_vkDestroySemaphore real_fn =
            (PFN_vkDestroySemaphore) get_real_proc(get_last_instance(), device, "vkDestroySemaphore");
        if (real_fn) {
            real_fn(device, semaphore, pAllocator);
        }
    }
}

VkResult LayerManager::dispatch_get_semaphore_counter_value(
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t* pValue
) {
    VkResult res = VK_SUCCESS;
    if (m_timeline_mod && m_timeline_mod->is_enabled()) {
        if (m_timeline_mod->on_get_semaphore_counter_value(device, semaphore, pValue, res)) {
            return res;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.GetSemaphoreCounterValue) {
        return dt.GetSemaphoreCounterValue(device, semaphore, pValue);
    }
    PFN_vkGetSemaphoreCounterValueKHR real_fn =
        (PFN_vkGetSemaphoreCounterValueKHR) get_real_proc(get_last_instance(), device, "vkGetSemaphoreCounterValueKHR");
    if (!real_fn) {
        real_fn = (PFN_vkGetSemaphoreCounterValueKHR) get_real_proc(get_last_instance(), device, "vkGetSemaphoreCounterValue");
    }
    if (real_fn) {
        return real_fn(device, semaphore, pValue);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult LayerManager::dispatch_wait_semaphores(
    VkDevice device,
    const VkSemaphoreWaitInfo* pWaitInfo,
    uint64_t timeout
) {
    VkResult res = VK_SUCCESS;
    if (m_timeline_mod && m_timeline_mod->is_enabled()) {
        if (m_timeline_mod->on_wait_semaphores(device, pWaitInfo, timeout, res)) {
            return res;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.WaitSemaphores) {
        return dt.WaitSemaphores(device, pWaitInfo, timeout);
    }
    PFN_vkWaitSemaphoresKHR real_fn =
        (PFN_vkWaitSemaphoresKHR) get_real_proc(get_last_instance(), device, "vkWaitSemaphoresKHR");
    if (!real_fn) {
        real_fn = (PFN_vkWaitSemaphoresKHR) get_real_proc(get_last_instance(), device, "vkWaitSemaphores");
    }
    if (real_fn) {
        return real_fn(device, pWaitInfo, timeout);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult LayerManager::dispatch_signal_semaphore(
    VkDevice device,
    const VkSemaphoreSignalInfo* pSignalInfo
) {
    VkResult res = VK_SUCCESS;
    if (m_timeline_mod && m_timeline_mod->is_enabled()) {
        if (m_timeline_mod->on_signal_semaphore(device, pSignalInfo, res)) {
            return res;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.SignalSemaphore) {
        return dt.SignalSemaphore(device, pSignalInfo);
    }
    PFN_vkSignalSemaphoreKHR real_fn =
        (PFN_vkSignalSemaphoreKHR) get_real_proc(get_last_instance(), device, "vkSignalSemaphoreKHR");
    if (!real_fn) {
        real_fn = (PFN_vkSignalSemaphoreKHR) get_real_proc(get_last_instance(), device, "vkSignalSemaphore");
    }
    if (real_fn) {
        return real_fn(device, pSignalInfo);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

void LayerManager::dispatch_reset_query_pool(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount
) {
    if (m_host_query_reset_mod && m_host_query_reset_mod->is_enabled()) {
        if (m_host_query_reset_mod->on_reset_query_pool(device, queryPool, firstQuery, queryCount)) {
            return;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.ResetQueryPool) {
        dt.ResetQueryPool(device, queryPool, firstQuery, queryCount);
        return;
    }
    PFN_vkResetQueryPool real_fn =
        (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPool");
    if (!real_fn) {
        real_fn = (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPoolEXT");
    }
    if (real_fn) {
        real_fn(device, queryPool, firstQuery, queryCount);
    }
}

VkResult LayerManager::dispatch_create_render_pass2(
    VkDevice device,
    const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass
) {
    auto tracked = [&](VkResult result) {
        if (result == VK_SUCCESS && pRenderPass && m_dyn_rendering_mod)
            m_dyn_rendering_mod->track_render_pass2(device, pCreateInfo, *pRenderPass);
        return result;
    };
    VkResult res = VK_SUCCESS;
    if (m_renderpass2_mod && m_renderpass2_mod->is_enabled()) {
        if (m_renderpass2_mod->on_create_render_pass2(device, pCreateInfo, pAllocator, pRenderPass, res)) {
            return tracked(res);
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.CreateRenderPass2) {
        return tracked(dt.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass));
    }
    PFN_vkCreateRenderPass2KHR real_fn =
        (PFN_vkCreateRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCreateRenderPass2KHR");
    if (!real_fn) {
        real_fn = (PFN_vkCreateRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCreateRenderPass2");
    }
    if (real_fn) {
        return tracked(real_fn(device, pCreateInfo, pAllocator, pRenderPass));
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

void LayerManager::dispatch_cmd_begin_render_pass2(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo
) {
    VertexStoreEmulator::get().legacy_rendering(commandBuffer, true);

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    VkRenderPassBeginInfo resolved{};
    const VkRenderPassBeginInfo* begin = pRenderPassBegin;
    const auto imageless = ImagelessFramebufferManager::get().resolve(pRenderPassBegin, resolved);
    if (imageless == ImagelessFramebufferManager::ResolveResult::Failed) {
        LOGE("vkCmdBeginRenderPass2: imageless framebuffer could not be resolved; skipping submission");
        return;
    }
    if (imageless == ImagelessFramebufferManager::ResolveResult::Resolved) {
        begin = &resolved;
    }

    if (m_dyn_rendering_mod) {
        m_dyn_rendering_mod->begin_legacy_rendering(commandBuffer, begin,
            pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE);
        if (pSubpassBeginInfo && pSubpassBeginInfo->pNext)
            m_dyn_rendering_mod->end_legacy_rendering(commandBuffer);
    }
    if (m_renderpass2_mod && m_renderpass2_mod->is_enabled()) {
        if (m_renderpass2_mod->on_cmd_begin_render_pass2(commandBuffer, begin, pSubpassBeginInfo)) {
            return;
        }
    }
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdBeginRenderPass2) {
        dt.CmdBeginRenderPass2(commandBuffer, begin, pSubpassBeginInfo);
    } else if (dt.CmdBeginRenderPass) {
        VkSubpassContents contents = pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE;
        dt.CmdBeginRenderPass(commandBuffer, begin, contents);
    }
}

void LayerManager::dispatch_cmd_next_subpass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassBeginInfo* pSubpassBeginInfo,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->end_legacy_rendering(commandBuffer);
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (m_renderpass2_mod && m_renderpass2_mod->is_enabled()) {
        if (m_renderpass2_mod->on_cmd_next_subpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo)) {
            return;
        }
    }
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdNextSubpass2) {
        dt.CmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
    } else if (dt.CmdNextSubpass) {
        VkSubpassContents contents = pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE;
        dt.CmdNextSubpass(commandBuffer, contents);
    }
}

void LayerManager::dispatch_cmd_end_render_pass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->end_legacy_rendering(commandBuffer);
    VertexStoreEmulator::get().legacy_rendering(commandBuffer, false);

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (m_renderpass2_mod && m_renderpass2_mod->is_enabled()) {
        if (m_renderpass2_mod->on_cmd_end_render_pass2(commandBuffer, pSubpassEndInfo)) {
            return;
        }
    }
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdEndRenderPass2) {
        dt.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
    } else if (dt.CmdEndRenderPass) {
        dt.CmdEndRenderPass(commandBuffer);
    }
}

void LayerManager::dispatch_cmd_draw_indirect_count(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->before_draw(commandBuffer, 0, maxDrawCount);
    VertexStoreEmulator::get().indirect(commandBuffer);
    if (VertexStoreEmulator::get().recording_error(commandBuffer) != VK_SUCCESS) return;

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (m_draw_indirect_count_mod && m_draw_indirect_count_mod->is_enabled()) {
        if (m_draw_indirect_count_mod->on_cmd_draw_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride)) {
            return;
        }
    }
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdDrawIndirectCount) {
        dt.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }
}

void LayerManager::dispatch_cmd_draw_indexed_indirect_count(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    if (m_dyn_rendering_mod) m_dyn_rendering_mod->before_draw(commandBuffer, 0, maxDrawCount);
    VertexStoreEmulator::get().indirect(commandBuffer);
    if (VertexStoreEmulator::get().recording_error(commandBuffer) != VK_SUCCESS) return;

    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (m_draw_indirect_count_mod && m_draw_indirect_count_mod->is_enabled()) {
        if (m_draw_indirect_count_mod->on_cmd_draw_indexed_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride)) {
            return;
        }
    }
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdDrawIndexedIndirectCount) {
        dt.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }
}

VkDeviceAddress LayerManager::dispatch_get_buffer_device_address(
    VkDevice device,
    const VkBufferDeviceAddressInfo* pInfo
) {
    VkDeviceAddress addr = 0;
    if (m_bda_mod && m_bda_mod->is_enabled()) {
        if (m_bda_mod->on_get_buffer_device_address(device, pInfo, addr)) {
            return addr;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.GetBufferDeviceAddress) {
        return dt.GetBufferDeviceAddress(device, pInfo);
    }
    PFN_vkGetBufferDeviceAddressKHR real_fn =
        (PFN_vkGetBufferDeviceAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferDeviceAddressKHR");
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferDeviceAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferDeviceAddress");
    }
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferDeviceAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferDeviceAddressEXT");
    }
    if (real_fn) {
        addr = real_fn(device, pInfo);
    }
    return addr;
}

uint64_t LayerManager::dispatch_get_buffer_opaque_capture_address(
    VkDevice device,
    const VkBufferDeviceAddressInfo* pInfo
) {
    PFN_vkGetBufferOpaqueCaptureAddressKHR real_fn =
        (PFN_vkGetBufferOpaqueCaptureAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferOpaqueCaptureAddressKHR");
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferOpaqueCaptureAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferOpaqueCaptureAddress");
    }
    if (real_fn) {
        return real_fn(device, pInfo);
    }
    return pInfo ? (uint64_t)(uintptr_t)pInfo->buffer : 0;
}

uint64_t LayerManager::dispatch_get_device_memory_opaque_capture_address(
    VkDevice device,
    const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo
) {
    PFN_vkGetDeviceMemoryOpaqueCaptureAddressKHR real_fn =
        (PFN_vkGetDeviceMemoryOpaqueCaptureAddressKHR) get_real_proc(get_last_instance(), device, "vkGetDeviceMemoryOpaqueCaptureAddressKHR");
    if (!real_fn) {
        real_fn = (PFN_vkGetDeviceMemoryOpaqueCaptureAddressKHR) get_real_proc(get_last_instance(), device, "vkGetDeviceMemoryOpaqueCaptureAddress");
    }
    if (real_fn) {
        return real_fn(device, pInfo);
    }
    return pInfo ? (uint64_t)(uintptr_t)pInfo->memory : 0;
}

// ============================================================================
// Vulkan 1.1 Core / Promoted Features Dispatchers
// ============================================================================

VkResult LayerManager::dispatch_bind_buffer_memory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindBufferMemoryInfo* pBindInfos
) {
    VkResult res = VK_SUCCESS;
    if (m_bind_mem2_mod && m_bind_mem2_mod->is_enabled()) {
        if (m_bind_mem2_mod->on_bind_buffer_memory2(device, bindInfoCount, pBindInfos, res)) {
            return res;
        }
    }

    PFN_vkBindBufferMemory2 real_fn =
        (PFN_vkBindBufferMemory2) get_real_proc(get_last_instance(), device, "vkBindBufferMemory2");
    if (!real_fn) {
        real_fn = (PFN_vkBindBufferMemory2) get_real_proc(get_last_instance(), device, "vkBindBufferMemory2KHR");
    }
    if (real_fn) {
        return real_fn(device, bindInfoCount, pBindInfos);
    }

    if (bindInfoCount == 0 || !pBindInfos) return VK_SUCCESS;
    PFN_vkBindBufferMemory real_bind =
        (PFN_vkBindBufferMemory) get_real_proc(get_last_instance(), device, "vkBindBufferMemory");
    if (!real_bind) return VK_ERROR_INITIALIZATION_FAILED;

    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        const auto* group = vku::find_pnext<VkBindBufferMemoryDeviceGroupInfo>(
            pBindInfos[i].pNext, VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO);
        if (group != nullptr &&
            (group->deviceIndexCount > 1 ||
             (group->deviceIndexCount == 1 && group->pDeviceIndices != nullptr &&
              group->pDeviceIndices[0] != 0))) {
            LOGW("vkBindBufferMemory2: multi-device binding is not supported on this driver");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        VkResult r = real_bind(device, pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

VkResult LayerManager::dispatch_bind_image_memory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindImageMemoryInfo* pBindInfos
) {
    VkResult res = VK_SUCCESS;
    if (m_bind_mem2_mod && m_bind_mem2_mod->is_enabled()) {
        if (m_bind_mem2_mod->on_bind_image_memory2(device, bindInfoCount, pBindInfos, res)) {
            return res;
        }
    }

    PFN_vkBindImageMemory2 real_fn =
        (PFN_vkBindImageMemory2) get_real_proc(get_last_instance(), device, "vkBindImageMemory2");
    if (!real_fn) {
        real_fn = (PFN_vkBindImageMemory2) get_real_proc(get_last_instance(), device, "vkBindImageMemory2KHR");
    }
    if (real_fn) {
        return real_fn(device, bindInfoCount, pBindInfos);
    }

    if (bindInfoCount == 0 || !pBindInfos) return VK_SUCCESS;
    PFN_vkBindImageMemory real_bind =
        (PFN_vkBindImageMemory) get_real_proc(get_last_instance(), device, "vkBindImageMemory");
    if (!real_bind) return VK_ERROR_INITIALIZATION_FAILED;

    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        const auto* group = vku::find_pnext<VkBindImageMemoryDeviceGroupInfo>(
            pBindInfos[i].pNext, VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO);
        if (group != nullptr &&
            (group->deviceIndexCount > 1 || group->splitInstanceBindRegionCount > 0 ||
             (group->deviceIndexCount == 1 && group->pDeviceIndices != nullptr &&
              group->pDeviceIndices[0] != 0))) {
            LOGW("vkBindImageMemory2: multi-device binding is not supported on this driver");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        VkResult r = real_bind(device, pBindInfos[i].image, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

void LayerManager::dispatch_get_buffer_memory_requirements2(
    VkDevice device,
    const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    if (m_get_mem_reqs2_mod && m_get_mem_reqs2_mod->is_enabled()) {
        if (m_get_mem_reqs2_mod->on_get_buffer_memory_requirements2(device, pInfo, pMemoryRequirements)) {
            return;
        }
    }

    PFN_vkGetBufferMemoryRequirements2 real_fn =
        (PFN_vkGetBufferMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements2");
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements2KHR");
    }
    if (real_fn) {
        real_fn(device, pInfo, pMemoryRequirements);
        return;
    }

    if (pInfo && pMemoryRequirements) {
        PFN_vkGetBufferMemoryRequirements real_gmr =
            (PFN_vkGetBufferMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements");
        if (real_gmr) {
            real_gmr(device, pInfo->buffer, &pMemoryRequirements->memoryRequirements);
        }
        // A 1.0 driver has no dedicated-allocation support; never leave the
        // application reading uninitialised pNext memory.
        if (auto* dedicated = vku::find_pnext_mut<VkMemoryDedicatedRequirements>(
                pMemoryRequirements->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS)) {
            dedicated->prefersDedicatedAllocation = VK_FALSE;
            dedicated->requiresDedicatedAllocation = VK_FALSE;
        }
    }
}

void LayerManager::dispatch_get_image_memory_requirements2(
    VkDevice device,
    const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    if (m_get_mem_reqs2_mod && m_get_mem_reqs2_mod->is_enabled()) {
        if (m_get_mem_reqs2_mod->on_get_image_memory_requirements2(device, pInfo, pMemoryRequirements)) {
            return;
        }
    }

    PFN_vkGetImageMemoryRequirements2 real_fn =
        (PFN_vkGetImageMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements2");
    if (!real_fn) {
        real_fn = (PFN_vkGetImageMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements2KHR");
    }
    if (real_fn) {
        real_fn(device, pInfo, pMemoryRequirements);
        return;
    }

    if (pInfo && pMemoryRequirements) {
        PFN_vkGetImageMemoryRequirements real_gmr =
            (PFN_vkGetImageMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements");
        if (real_gmr) {
            real_gmr(device, pInfo->image, &pMemoryRequirements->memoryRequirements);
        }
        if (auto* dedicated = vku::find_pnext_mut<VkMemoryDedicatedRequirements>(
                pMemoryRequirements->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS)) {
            dedicated->prefersDedicatedAllocation = VK_FALSE;
            dedicated->requiresDedicatedAllocation = VK_FALSE;
        }
    }
}

void LayerManager::dispatch_get_image_sparse_memory_requirements2(
    VkDevice device,
    const VkImageSparseMemoryRequirementsInfo2* pInfo,
    uint32_t* pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2* pSparseMemoryRequirements
) {
    if (m_get_mem_reqs2_mod && m_get_mem_reqs2_mod->is_enabled()) {
        if (m_get_mem_reqs2_mod->on_get_image_sparse_memory_requirements2(
                device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements)) {
            return;
        }
    }

    PFN_vkGetImageSparseMemoryRequirements2 real_fn =
        (PFN_vkGetImageSparseMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageSparseMemoryRequirements2");
    if (!real_fn) {
        real_fn = (PFN_vkGetImageSparseMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageSparseMemoryRequirements2KHR");
    }
    if (real_fn) {
        real_fn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
        return;
    }

    if (pSparseMemoryRequirementCount) {
        *pSparseMemoryRequirementCount = 0;
    }
}

void LayerManager::dispatch_update_descriptor_set_with_template(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const void* pData
) {
    if (m_desc_template_mod && m_desc_template_mod->is_enabled()) {
        if (m_desc_template_mod->on_update_descriptor_set_with_template(
                device, descriptorSet, descriptorUpdateTemplate, pData)) {
            return;
        }
    }

    PFN_vkUpdateDescriptorSetWithTemplate real_fn =
        (PFN_vkUpdateDescriptorSetWithTemplate) get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSetWithTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkUpdateDescriptorSetWithTemplate) get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSetWithTemplateKHR");
    }
    if (real_fn) {
        real_fn(device, descriptorSet, descriptorUpdateTemplate, pData);
    }
}

VkPhysicalDevice LayerManager::get_physical_device_for(VkDevice device) {
    std::shared_lock<std::shared_mutex> lock(m_state_rw_mutex);
    auto it = m_device_phys.find((uint64_t)(uintptr_t)device);
    return it != m_device_phys.end() ? it->second : VK_NULL_HANDLE;
}

namespace {

struct DescriptorCounts {
    uint64_t sampler = 0;
    uint64_t sampled = 0;
    uint64_t ubo = 0;
    uint64_t sbo = 0;
    uint64_t storage_image = 0;
    uint64_t input = 0;
};

bool accumulate_descriptor(DescriptorCounts& counts, VkDescriptorType type, uint64_t count) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            counts.sampler += count;
            return true;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            counts.sampler += count;
            counts.sampled += count;
            return true;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            counts.sampled += count;
            return true;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            counts.storage_image += count;
            return true;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            counts.ubo += count;
            return true;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            counts.sbo += count;
            return true;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            counts.input += count;
            return true;
        default:
            // Types introduced after Vulkan 1.0 (acceleration structures,
            // inline uniform blocks, mutable) are not available on a 1.0 driver.
            return false;
    }
}

bool descriptor_layout_within_limits(const VkPhysicalDeviceLimits& limits,
                                     const VkDescriptorSetLayoutCreateInfo* info) {
    if (!info) return true;
    const auto* binding_flags = vku::find_pnext<VkDescriptorSetLayoutBindingFlagsCreateInfo>(
        info->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    if (binding_flags != nullptr && binding_flags->pBindingFlags != nullptr) {
        for (uint32_t i = 0; i < binding_flags->bindingCount; ++i) {
            if ((binding_flags->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0) {
                // Variable descriptor count layouts are validated by the driver
                // against the descriptor-indexing limits.
                return true;
            }
        }
    }
    if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) != 0) {
        // Native descriptor indexing supports update-after-bind; the driver
        // validates the update-after-bind limits itself.
        return true;
    }
#ifdef VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_EXT
    if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_EXT) != 0) {
        return false;
    }
#endif
    if (info->bindingCount > 0 && info->pBindings == nullptr) return false;

    DescriptorCounts total{};
    DescriptorCounts per_stage[32]{};
    uint64_t per_stage_resources[32] = {};

    for (uint32_t i = 0; i < info->bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding& binding = info->pBindings[i];
        if (binding.descriptorCount == 0) continue;
        if (!accumulate_descriptor(total, binding.descriptorType, binding.descriptorCount)) {
            return false;
        }
        for (uint32_t bit = 0; bit < 32; ++bit) {
            if ((binding.stageFlags & (1u << bit)) == 0) continue;
            if (!accumulate_descriptor(per_stage[bit], binding.descriptorType, binding.descriptorCount)) {
                return false;
            }
            per_stage_resources[bit] += binding.descriptorCount;
        }
    }

    if (total.sampler > limits.maxDescriptorSetSamplers ||
        total.sampled > limits.maxDescriptorSetSampledImages ||
        total.ubo > limits.maxDescriptorSetUniformBuffers ||
        total.sbo > limits.maxDescriptorSetStorageBuffers ||
        total.storage_image > limits.maxDescriptorSetStorageImages ||
        total.input > limits.maxDescriptorSetInputAttachments) {
        return false;
    }

    for (uint32_t bit = 0; bit < 32; ++bit) {
        if (per_stage[bit].sampler > limits.maxPerStageDescriptorSamplers ||
            per_stage[bit].sampled > limits.maxPerStageDescriptorSampledImages ||
            per_stage[bit].ubo > limits.maxPerStageDescriptorUniformBuffers ||
            per_stage[bit].sbo > limits.maxPerStageDescriptorStorageBuffers ||
            per_stage[bit].storage_image > limits.maxPerStageDescriptorStorageImages ||
            per_stage[bit].input > limits.maxPerStageDescriptorInputAttachments ||
            per_stage_resources[bit] > limits.maxPerStageResources) {
            return false;
        }
    }
    return true;
}

}  // namespace

void LayerManager::dispatch_get_descriptor_set_layout_support(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    VkDescriptorSetLayoutSupport* pSupport
) {
    if (m_maintenance_mod && m_maintenance_mod->is_enabled()) {
        if (m_maintenance_mod->on_get_descriptor_set_layout_support(device, pCreateInfo, pSupport)) {
            return;
        }
    }

    PFN_vkGetDescriptorSetLayoutSupport real_fn =
        (PFN_vkGetDescriptorSetLayoutSupport) get_real_proc(get_last_instance(), device, "vkGetDescriptorSetLayoutSupport");
    if (!real_fn) {
        real_fn = (PFN_vkGetDescriptorSetLayoutSupport) get_real_proc(get_last_instance(), device, "vkGetDescriptorSetLayoutSupportKHR");
    }
    if (real_fn) {
        real_fn(device, pCreateInfo, pSupport);
        return;
    }
    if (pSupport) {
        bool supported = true;
        if (pCreateInfo) {
            VkPhysicalDevice physicalDevice = get_physical_device_for(device);
            PFN_vkGetPhysicalDeviceProperties get_props = (PFN_vkGetPhysicalDeviceProperties)
                get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
            if (physicalDevice != VK_NULL_HANDLE && get_props != nullptr) {
                VkPhysicalDeviceProperties props{};
                get_props(physicalDevice, &props);
                supported = descriptor_layout_within_limits(props.limits, pCreateInfo);
            }
        }
        pSupport->supported = supported ? VK_TRUE : VK_FALSE;
    }
}

void LayerManager::dispatch_cmd_dispatch_base(
    VkCommandBuffer commandBuffer,
    uint32_t baseGroupX,
    uint32_t baseGroupY,
    uint32_t baseGroupZ,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);

    if (m_device_group_mod && m_device_group_mod->is_enabled()) {
        if (m_device_group_mod->on_cmd_dispatch_base(
                commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdDispatchBase) {
        dt.CmdDispatchBase(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
        return;
    }

    if (baseGroupX == 0 && baseGroupY == 0 && baseGroupZ == 0) {
        PFN_vkCmdDispatch real_disp =
            (PFN_vkCmdDispatch) get_real_proc(get_last_instance(), device, "vkCmdDispatch");
        if (real_disp) {
            real_disp(commandBuffer, groupCountX, groupCountY, groupCountZ);
        }
    } else {
        LOGW("vkCmdDispatchBase: non-zero base group (%u,%u,%u) needs shader-side emulation; dispatch skipped",
             baseGroupX, baseGroupY, baseGroupZ);
    }
}

VkResult LayerManager::dispatch_enumerate_physical_device_groups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties
) {
    VkResult res = VK_SUCCESS;
    if (m_device_group_mod && m_device_group_mod->is_enabled()) {
        if (m_device_group_mod->on_enumerate_physical_device_groups(
                instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties, res)) {
            return res;
        }
    }

    VkInstance inst = (instance != VK_NULL_HANDLE) ? instance : get_last_instance();
    PFN_vkEnumeratePhysicalDeviceGroups real_fn =
        (PFN_vkEnumeratePhysicalDeviceGroups) get_real_proc(inst, VK_NULL_HANDLE, "vkEnumeratePhysicalDeviceGroups");
    if (!real_fn) {
        real_fn = (PFN_vkEnumeratePhysicalDeviceGroups) get_real_proc(inst, VK_NULL_HANDLE, "vkEnumeratePhysicalDeviceGroupsKHR");
    }
    if (real_fn) {
        return real_fn(inst, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    }

    // Vulkan 1.0 fallback: expose every physical device as a single-device
    // group. This is the only valid device-group topology on a 1.0 driver.
    if (!pPhysicalDeviceGroupCount) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumeratePhysicalDevices real_enum =
        (PFN_vkEnumeratePhysicalDevices) get_real_proc(inst, VK_NULL_HANDLE, "vkEnumeratePhysicalDevices");
    if (!real_enum) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t deviceCount = 0;
    if (real_enum(inst, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) {
        *pPhysicalDeviceGroupCount = 0;
        return VK_SUCCESS;
    }
    if (pPhysicalDeviceGroupProperties == nullptr) {
        *pPhysicalDeviceGroupCount = 1;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceGroupCount < 1) {
        *pPhysicalDeviceGroupCount = 1;
        return VK_INCOMPLETE;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (real_enum(inst, &deviceCount, devices.data()) != VK_SUCCESS || deviceCount == 0) {
        *pPhysicalDeviceGroupCount = 0;
        return VK_SUCCESS;
    }

    VkPhysicalDeviceGroupProperties& group = pPhysicalDeviceGroupProperties[0];
    group.physicalDeviceCount = 1;
    group.physicalDevices[0] = devices[0];
    for (uint32_t i = 1; i < VK_MAX_DEVICE_GROUP_SIZE; ++i) {
        group.physicalDevices[i] = VK_NULL_HANDLE;
    }
    group.subsetAllocation = VK_FALSE;
    *pPhysicalDeviceGroupCount = 1;
    return VK_SUCCESS;
}

void LayerManager::dispatch_trim_command_pool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolTrimFlags flags
) {
    if (m_maintenance_mod && m_maintenance_mod->is_enabled()) {
        m_maintenance_mod->on_trim_command_pool(device, commandPool, flags);
    }

    PFN_vkTrimCommandPool real_fn =
        (PFN_vkTrimCommandPool) get_real_proc(get_last_instance(), device, "vkTrimCommandPool");
    if (!real_fn) {
        real_fn = (PFN_vkTrimCommandPool) get_real_proc(get_last_instance(), device, "vkTrimCommandPoolKHR");
    }
    if (real_fn) {
        real_fn(device, commandPool, flags);
    }
}

void LayerManager::dispatch_cmd_set_device_mask(
    VkCommandBuffer commandBuffer,
    uint32_t deviceMask
) {
    if (m_device_group_mod && m_device_group_mod->is_enabled()) {
        m_device_group_mod->on_cmd_set_device_mask(commandBuffer, deviceMask);
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdSetDeviceMask) {
        dt.CmdSetDeviceMask(commandBuffer, deviceMask);
    }
}

void LayerManager::dispatch_get_device_group_peer_memory_features(
    VkDevice device,
    uint32_t heapIndex,
    uint32_t localDeviceIndex,
    uint32_t remoteDeviceIndex,
    VkPeerMemoryFeatureFlags* pPeerMemoryFeatures
) {
    if (m_device_group_mod && m_device_group_mod->is_enabled()) {
        if (m_device_group_mod->on_get_device_group_peer_memory_features(
                device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures)) {
            return;
        }
    }

    PFN_vkGetDeviceGroupPeerMemoryFeatures real_fn =
        (PFN_vkGetDeviceGroupPeerMemoryFeatures) get_real_proc(get_last_instance(), device, "vkGetDeviceGroupPeerMemoryFeatures");
    if (!real_fn) {
        real_fn = (PFN_vkGetDeviceGroupPeerMemoryFeatures) get_real_proc(get_last_instance(), device, "vkGetDeviceGroupPeerMemoryFeaturesKHR");
    }
    if (real_fn) {
        real_fn(device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
        return;
    }
    if (pPeerMemoryFeatures) {
        *pPeerMemoryFeatures = 0;
    }
}

// ============================================================================
// Swapchain & Surface Dispatches
// ============================================================================

VkResult LayerManager::dispatch_create_swapchain(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain
) {
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_create_swapchain(device, pCreateInfo, pAllocator, pSwapchain, res)) {
            if (res == VK_SUCCESS && pSwapchain && *pSwapchain != VK_NULL_HANDLE && pCreateInfo) {
                std::lock_guard<std::mutex> lk(m_swapchain_meta_mutex);
                m_swapchain_meta[(uint64_t)(uintptr_t)*pSwapchain] = {
                    pCreateInfo->imageFormat,
                    pCreateInfo->imageExtent,
                    pCreateInfo->imageUsage
                };
            }
            return res;
        }
    }
    const auto& dt = get_dispatch_table(device);
    VkSwapchainCreateInfoKHR modifiedInfo = *pCreateInfo;
    uint32_t target_min_images = 3;
    const char* env_min = getenv("VULKAN_FIX_SWAPCHAIN_MIN_IMAGES");
    if (env_min && env_min[0] != '\0') {
        int val = atoi(env_min);
        if (val >= 2 && val <= 8) target_min_images = (uint32_t)val;
    }
    if (modifiedInfo.minImageCount < target_min_images) {
        modifiedInfo.minImageCount = target_min_images;
    }
    if (dt.CreateSwapchain) {
        res = dt.CreateSwapchain(device, &modifiedInfo, pAllocator, pSwapchain);
        if (res != VK_SUCCESS) {
            res = dt.CreateSwapchain(device, pCreateInfo, pAllocator, pSwapchain);
        }
    } else {
        PFN_vkCreateSwapchainKHR real_fn =
            (PFN_vkCreateSwapchainKHR) get_real_proc(get_last_instance(), device, "vkCreateSwapchainKHR");
        if (real_fn) {
            res = real_fn(device, &modifiedInfo, pAllocator, pSwapchain);
            if (res != VK_SUCCESS) {
                res = real_fn(device, pCreateInfo, pAllocator, pSwapchain);
            }
        } else {
            res = VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    if (res == VK_SUCCESS && pSwapchain && *pSwapchain != VK_NULL_HANDLE && pCreateInfo) {
        std::lock_guard<std::mutex> lk(m_swapchain_meta_mutex);
        m_swapchain_meta[(uint64_t)(uintptr_t)*pSwapchain] = {
            pCreateInfo->imageFormat,
            pCreateInfo->imageExtent,
            pCreateInfo->imageUsage
        };
    }
    return res;
}

void LayerManager::dispatch_destroy_swapchain(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::mutex> lk(m_swapchain_meta_mutex);
        m_swapchain_meta.erase((uint64_t)(uintptr_t)swapchain);
    }
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_destroy_swapchain(device, swapchain, pAllocator)) {
            return;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.DestroySwapchain) {
        dt.DestroySwapchain(device, swapchain, pAllocator);
        return;
    }
    PFN_vkDestroySwapchainKHR real_fn =
        (PFN_vkDestroySwapchainKHR) get_real_proc(get_last_instance(), device, "vkDestroySwapchainKHR");
    if (real_fn) {
        real_fn(device, swapchain, pAllocator);
    }
}

VkResult LayerManager::dispatch_get_swapchain_images(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages
) {
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_get_swapchain_images(device, swapchain, pSwapchainImageCount, pSwapchainImages, res)) {
            // Handled below if successful
        } else {
            const auto& dt = get_dispatch_table(device);
            if (dt.GetSwapchainImages) {
                res = dt.GetSwapchainImages(device, swapchain, pSwapchainImageCount, pSwapchainImages);
            } else {
                PFN_vkGetSwapchainImagesKHR real_fn =
                    (PFN_vkGetSwapchainImagesKHR) get_real_proc(get_last_instance(), device, "vkGetSwapchainImagesKHR");
                if (real_fn) {
                    res = real_fn(device, swapchain, pSwapchainImageCount, pSwapchainImages);
                } else {
                    res = VK_ERROR_EXTENSION_NOT_PRESENT;
                }
            }
        }
    } else {
        const auto& dt = get_dispatch_table(device);
        if (dt.GetSwapchainImages) {
            res = dt.GetSwapchainImages(device, swapchain, pSwapchainImageCount, pSwapchainImages);
        } else {
            PFN_vkGetSwapchainImagesKHR real_fn =
                (PFN_vkGetSwapchainImagesKHR) get_real_proc(get_last_instance(), device, "vkGetSwapchainImagesKHR");
            if (real_fn) {
                res = real_fn(device, swapchain, pSwapchainImageCount, pSwapchainImages);
            } else {
                res = VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }
    }

    if (res == VK_SUCCESS && pSwapchainImages && pSwapchainImageCount && *pSwapchainImageCount > 0) {
        SwapchainMetadata meta{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(m_swapchain_meta_mutex);
            auto it = m_swapchain_meta.find((uint64_t)(uintptr_t)swapchain);
            if (it != m_swapchain_meta.end()) {
                meta = it->second;
                found = true;
            }
        }
        if (found) {
            std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
            for (auto& mod : m_modules) {
                mod->on_post_get_swapchain_images(
                    device,
                    swapchain,
                    meta.format,
                    meta.extent,
                    meta.usage,
                    *pSwapchainImageCount,
                    pSwapchainImages
                );
            }
        }
    }
    return res;
}

VkResult LayerManager::dispatch_acquire_next_image(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex
) {
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_acquire_next_image(device, swapchain, timeout, semaphore, fence, pImageIndex, res)) {
            return res;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.AcquireNextImage) {
        res = dt.AcquireNextImage(device, swapchain, timeout, semaphore, fence, pImageIndex);
        if (res == VK_TIMEOUT && timeout > 0 && timeout != UINT64_MAX) {
            LOGW("LayerManager: vkAcquireNextImageKHR timed out after %llu ns, retrying with 5s timeout to prevent crash...",
                 (unsigned long long)timeout);
            res = dt.AcquireNextImage(device, swapchain, 5000000000ULL, semaphore, fence, pImageIndex);
        }
        return res;
    }
    PFN_vkAcquireNextImageKHR real_fn =
        (PFN_vkAcquireNextImageKHR) get_real_proc(get_last_instance(), device, "vkAcquireNextImageKHR");
    if (real_fn) {
        res = real_fn(device, swapchain, timeout, semaphore, fence, pImageIndex);
        if (res == VK_TIMEOUT && timeout > 0 && timeout != UINT64_MAX) {
            LOGW("LayerManager: vkAcquireNextImageKHR timed out after %llu ns, retrying with 5s timeout to prevent crash...",
                 (unsigned long long)timeout);
            res = real_fn(device, swapchain, 5000000000ULL, semaphore, fence, pImageIndex);
        }
        return res;
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VkResult LayerManager::dispatch_acquire_next_image2(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* pAcquireInfo,
    uint32_t* pImageIndex
) {
    if (!pAcquireInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_acquire_next_image(device, pAcquireInfo->swapchain, pAcquireInfo->timeout, pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex, res)) {
            return res;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.AcquireNextImage2) {
        res = dt.AcquireNextImage2(device, pAcquireInfo, pImageIndex);
        if (res == VK_TIMEOUT && pAcquireInfo->timeout > 0 && pAcquireInfo->timeout != UINT64_MAX) {
            LOGW("LayerManager: vkAcquireNextImage2KHR timed out after %llu ns, retrying with 5s timeout to prevent crash...",
                 (unsigned long long)pAcquireInfo->timeout);
            VkAcquireNextImageInfoKHR retry_info = *pAcquireInfo;
            retry_info.timeout = 5000000000ULL;
            res = dt.AcquireNextImage2(device, &retry_info, pImageIndex);
        }
        return res;
    }
    PFN_vkAcquireNextImage2KHR real_fn =
        (PFN_vkAcquireNextImage2KHR) get_real_proc(get_last_instance(), device, "vkAcquireNextImage2KHR");
    if (real_fn) {
        res = real_fn(device, pAcquireInfo, pImageIndex);
        if (res == VK_TIMEOUT && pAcquireInfo->timeout > 0 && pAcquireInfo->timeout != UINT64_MAX) {
            LOGW("LayerManager: vkAcquireNextImage2KHR timed out after %llu ns, retrying with 5s timeout to prevent crash...",
                 (unsigned long long)pAcquireInfo->timeout);
            VkAcquireNextImageInfoKHR retry_info = *pAcquireInfo;
            retry_info.timeout = 5000000000ULL;
            res = real_fn(device, &retry_info, pImageIndex);
        }
        return res;
    }
    return dispatch_acquire_next_image(device, pAcquireInfo->swapchain, pAcquireInfo->timeout, pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex);
}

VkResult LayerManager::dispatch_queue_present(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    auto scheduled = vkfix::TimelineScheduler::get().before_present(queue, pPresentInfo);
    if (scheduled != VK_SUCCESS)
        return scheduled;

    auto result = vkfix::TimelineScheduler::get().native_queue_call(queue, [&]() -> VkResult {
        VkResult res = VK_SUCCESS;
        if (TBDRBarrierOptimizer::get().is_enabled()) {
            TBDRBarrierOptimizer::get().maybe_log_stats();
        }
        if (m_push_desc_mod && m_push_desc_mod->is_enabled()) {
            VkResult stats_result = VK_SUCCESS;
            m_push_desc_mod->on_queue_present(queue, pPresentInfo, stats_result);
        }
        if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
            if (m_swapchain_mod->on_queue_present(queue, pPresentInfo, res)) {
                RenderDocManager::get().on_post_present(get_device_for_queue(queue), res);
                return res;
            }
        }
        VkDevice device = get_device_for_queue(queue);
        const auto &dt = get_dispatch_table(device);
        if (dt.QueuePresent) {
            res = dt.QueuePresent(queue, pPresentInfo);
        } else {
            PFN_vkQueuePresentKHR real_fn = (PFN_vkQueuePresentKHR)get_real_proc(
                get_last_instance(), device, "vkQueuePresentKHR");
            if (real_fn) {
                res = real_fn(queue, pPresentInfo);
            } else {
                res = VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }
        RenderDocManager::get().on_post_present(device, res);
        return res;
    });
    vkfix::TimelineScheduler::get().after_present(queue, pPresentInfo, result);
    VertexStoreEmulator::get().on_present(get_device_for_queue(queue));
    return result;
}

VkResult LayerManager::dispatch_get_physical_device_surface_support(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported
) {
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_get_physical_device_surface_support(physicalDevice, queueFamilyIndex, surface, pSupported, res)) {
            return res;
        }
    }
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (real_fn) {
        return real_fn(physicalDevice, queueFamilyIndex, surface, pSupported);
    }
    if (pSupported) *pSupported = VK_TRUE;
    return VK_SUCCESS;
}

VkResult LayerManager::dispatch_get_physical_device_surface_capabilities(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities
) {
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_get_physical_device_surface_capabilities(physicalDevice, surface, pSurfaceCapabilities, res)) {
            return res;
        }
    }
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (real_fn) {
        res = real_fn(physicalDevice, surface, pSurfaceCapabilities);
        if (res == VK_SUCCESS && pSurfaceCapabilities) {
            uint32_t target_min_images = 3;
            const char* env_min = getenv("VULKAN_FIX_SWAPCHAIN_MIN_IMAGES");
            if (env_min && env_min[0] != '\0') {
                int val = atoi(env_min);
                if (val >= 2 && val <= 8) target_min_images = (uint32_t)val;
            }
            if (pSurfaceCapabilities->minImageCount < target_min_images) {
                pSurfaceCapabilities->minImageCount = target_min_images;
            }
            if (pSurfaceCapabilities->maxImageCount > 0 && pSurfaceCapabilities->minImageCount > pSurfaceCapabilities->maxImageCount) {
                pSurfaceCapabilities->minImageCount = pSurfaceCapabilities->maxImageCount;
            }
        }
        return res;
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VkResult LayerManager::dispatch_get_physical_device_surface_capabilities2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities
) {
    if (!pSurfaceInfo || !pSurfaceCapabilities) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    VkResult res = VK_SUCCESS;
    if (real_fn) {
        res = real_fn(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
    } else {
        res = dispatch_get_physical_device_surface_capabilities(physicalDevice, pSurfaceInfo->surface, &pSurfaceCapabilities->surfaceCapabilities);
    }
    if (res == VK_SUCCESS) {
        uint32_t target_min_images = 3;
        const char* env_min = getenv("VULKAN_FIX_SWAPCHAIN_MIN_IMAGES");
        if (env_min && env_min[0] != '\0') {
            int val = atoi(env_min);
            if (val >= 2 && val <= 8) target_min_images = (uint32_t)val;
        }
        if (pSurfaceCapabilities->surfaceCapabilities.minImageCount < target_min_images) {
            pSurfaceCapabilities->surfaceCapabilities.minImageCount = target_min_images;
        }
        if (pSurfaceCapabilities->surfaceCapabilities.maxImageCount > 0 &&
            pSurfaceCapabilities->surfaceCapabilities.minImageCount > pSurfaceCapabilities->surfaceCapabilities.maxImageCount) {
            pSurfaceCapabilities->surfaceCapabilities.minImageCount = pSurfaceCapabilities->surfaceCapabilities.maxImageCount;
        }
    }
    return res;
}

VkResult LayerManager::dispatch_get_physical_device_surface_formats(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats
) {
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_get_physical_device_surface_formats(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats, res)) {
            return res;
        }
    }
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (real_fn) {
        return real_fn(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VkResult LayerManager::dispatch_get_physical_device_surface_formats2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormat2KHR* pSurfaceFormats
) {
    if (!pSurfaceInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceSurfaceFormats2KHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfaceFormats2KHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceFormats2KHR");
    if (real_fn) {
        return real_fn(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
    }
    if (!pSurfaceFormats) {
        return dispatch_get_physical_device_surface_formats(physicalDevice, pSurfaceInfo->surface, pSurfaceFormatCount, nullptr);
    }
    uint32_t count = *pSurfaceFormatCount;
    std::vector<VkSurfaceFormatKHR> formats(count);
    VkResult r = dispatch_get_physical_device_surface_formats(physicalDevice, pSurfaceInfo->surface, &count, formats.data());
    if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
        for (uint32_t i = 0; i < count; ++i) {
            pSurfaceFormats[i].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
            pSurfaceFormats[i].pNext = nullptr;
            pSurfaceFormats[i].surfaceFormat = formats[i];
        }
        *pSurfaceFormatCount = count;
    }
    return r;
}

VkResult LayerManager::dispatch_get_physical_device_surface_present_modes(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes
) {
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_get_physical_device_surface_present_modes(physicalDevice, surface, pPresentModeCount, pPresentModes, res)) {
            return res;
        }
    }
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (real_fn) {
        return real_fn(physicalDevice, surface, pPresentModeCount, pPresentModes);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VkResult LayerManager::dispatch_create_android_surface(
    VkInstance instance,
    const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface
) {
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_create_android_surface(instance, pCreateInfo, pAllocator, pSurface, res)) {
            return res;
        }
    }
    PFN_vkCreateAndroidSurfaceKHR real_fn =
        (PFN_vkCreateAndroidSurfaceKHR) get_real_proc(instance, VK_NULL_HANDLE, "vkCreateAndroidSurfaceKHR");
    if (real_fn) {
        return real_fn(instance, pCreateInfo, pAllocator, pSurface);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void LayerManager::dispatch_destroy_surface(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator
) {
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_destroy_surface(instance, surface, pAllocator)) {
            return;
        }
    }
    PFN_vkDestroySurfaceKHR real_fn =
        (PFN_vkDestroySurfaceKHR) get_real_proc(instance, VK_NULL_HANDLE, "vkDestroySurfaceKHR");
    if (real_fn) {
        real_fn(instance, surface, pAllocator);
    }
}

void LayerManager::dispatch_cmd_begin_debug_utils_label(
    VkCommandBuffer commandBuffer,
    const VkDebugUtilsLabelEXT* pLabelInfo
) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdBeginDebugUtilsLabelEXT) {
        dt.CmdBeginDebugUtilsLabelEXT(commandBuffer, pLabelInfo);
    }
}

void LayerManager::dispatch_cmd_end_debug_utils_label(
    VkCommandBuffer commandBuffer
) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdEndDebugUtilsLabelEXT) {
        dt.CmdEndDebugUtilsLabelEXT(commandBuffer);
    }
}

void LayerManager::dispatch_cmd_insert_debug_utils_label(
    VkCommandBuffer commandBuffer,
    const VkDebugUtilsLabelEXT* pLabelInfo
) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdInsertDebugUtilsLabelEXT) {
        dt.CmdInsertDebugUtilsLabelEXT(commandBuffer, pLabelInfo);
    }
}

VkResult LayerManager::dispatch_set_debug_utils_object_name(
    VkDevice device,
    const VkDebugUtilsObjectNameInfoEXT* pNameInfo
) {
    const auto& dt = get_dispatch_table(device);
    if (dt.SetDebugUtilsObjectNameEXT) {
        return dt.SetDebugUtilsObjectNameEXT(device, pNameInfo);
    }
    return VK_SUCCESS;
}

VkResult LayerManager::dispatch_set_debug_utils_object_tag(
    VkDevice device,
    const VkDebugUtilsObjectTagInfoEXT* pTagInfo
) {
    const auto& dt = get_dispatch_table(device);
    if (dt.SetDebugUtilsObjectTagEXT) {
        return dt.SetDebugUtilsObjectTagEXT(device, pTagInfo);
    }
    return VK_SUCCESS;
}





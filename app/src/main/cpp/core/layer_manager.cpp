#include "layer_manager.h"
#include "driver_loader.h"

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
}

void LayerManager::register_custom_proc(const char* name, PFN_vkVoidFunction proc) {
    if (!name || !proc) return;
    std::lock_guard<std::mutex> lock(m_proc_mutex);
    m_custom_procs[name] = proc;
    LOGI("LayerManager: registered custom proc '%s' -> %p", name, (void*)proc);
}

PFN_vkVoidFunction LayerManager::get_custom_proc(const char* name) {
    if (!name) return NULL;
    std::lock_guard<std::mutex> lock(m_proc_mutex);
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
    else if (strcmp(name, "VK_KHR_timeline_semaphore") == 0) m_timeline_mod = module.get();
    else if (strcmp(name, "VK_KHR_create_renderpass2") == 0) m_renderpass2_mod = module.get();
    else if (strcmp(name, "VK_KHR_draw_indirect_count") == 0) m_draw_indirect_count_mod = module.get();
    else if (strcmp(name, "VK_KHR_device_group") == 0) m_device_group_mod = module.get();
    else if (strcmp(name, "VK_KHR_swapchain") == 0) m_swapchain_mod = module.get();
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
    LOAD_PROC(QueueWaitIdle);
    LOAD_PROC(DeviceWaitIdle);
    LOAD_PROC(GetDeviceQueue);
    LOAD_PROC(GetDeviceQueue2);
    LOAD_PROC(CreateGraphicsPipelines);
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
    LOAD_PROC(CmdBeginRenderPass);
    LOAD_PROC(CmdEndRenderPass);
    LOAD_PROC(CreateRenderPass);
    LOAD_PROC(CreateFramebuffer);
    LOAD_PROC(UpdateDescriptorSets);
    LOAD_PROC(CmdBindDescriptorSets);
    LOAD_PROC(AllocateDescriptorSets);
    LOAD_PROC(FreeDescriptorSets);
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

    #undef LOAD_PROC
    #undef LOAD_PROC_OPT

    std::lock_guard<std::mutex> lock(m_table_mutex);
    m_device_tables[(uint64_t)(uintptr_t)device] = dt;
    if (!m_has_primary_table.load(std::memory_order_relaxed) || m_primary_device.load(std::memory_order_relaxed) == device) {
        m_primary_table = dt;
        m_primary_device.store(device, std::memory_order_release);
        m_has_primary_table.store(true, std::memory_order_release);
    }
}

void LayerManager::remove_device_dispatch_table(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_table_mutex);
    m_device_tables.erase((uint64_t)(uintptr_t)device);
    if (m_primary_device.load(std::memory_order_relaxed) == device) {
        if (!m_device_tables.empty()) {
            auto it = m_device_tables.begin();
            m_primary_table = it->second;
            m_primary_device.store((VkDevice)(uintptr_t)it->first, std::memory_order_release);
        } else {
            m_has_primary_table.store(false, std::memory_order_release);
            m_primary_device.store(VK_NULL_HANDLE, std::memory_order_release);
        }
    }
}

const DeviceDispatchTable& LayerManager::get_dispatch_table_slow(VkDevice device) {
    {
        std::lock_guard<std::mutex> lock(m_table_mutex);
        auto it = m_device_tables.find((uint64_t)(uintptr_t)device);
        if (it != m_device_tables.end()) {
            return it->second;
        }
    }
    if (device != VK_NULL_HANDLE) {
        init_device_dispatch_table(device);
        std::lock_guard<std::mutex> lock(m_table_mutex);
        auto it = m_device_tables.find((uint64_t)(uintptr_t)device);
        if (it != m_device_tables.end()) {
            return it->second;
        }
    }
    static DeviceDispatchTable s_empty{};
    return s_empty;
}

VkDevice LayerManager::get_device_for_cmd_slow(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
    auto it = m_cmd_devices.find((uint64_t)(uintptr_t)cmd);
    if (it != m_cmd_devices.end()) {
        return it->second;
    }
    return m_last_device.load(std::memory_order_relaxed);
}

VkDevice LayerManager::get_device_for_queue_slow(VkQueue queue) {
    std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
    auto it = m_queue_devices.find((uint64_t)(uintptr_t)queue);
    if (it != m_queue_devices.end()) {
        return it->second;
    }
    return m_last_device.load(std::memory_order_relaxed);
}

void LayerManager::add_emulated_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_emulated_devices.insert((uint64_t)(uintptr_t)device);
    m_primary_emulated_device.store(device, std::memory_order_relaxed);
}

void LayerManager::remove_emulated_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_emulated_devices.erase((uint64_t)(uintptr_t)device);
    if (m_primary_emulated_device.load(std::memory_order_relaxed) == device) {
        VkDevice next_dev = VK_NULL_HANDLE;
        if (!m_emulated_devices.empty()) {
            next_dev = (VkDevice)(uintptr_t)*m_emulated_devices.begin();
        }
        m_primary_emulated_device.store(next_dev, std::memory_order_relaxed);
    }
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
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
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

    std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
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

    PFN_vkGetPhysicalDeviceFeatures2 real_fn =
        (PFN_vkGetPhysicalDeviceFeatures2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceFeatures2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2KHR");
    }

    if (!real_fn) {
        PFN_vkGetPhysicalDeviceFeatures real_fn1 =
            (PFN_vkGetPhysicalDeviceFeatures) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures");
        if (real_fn1) {
            real_fn1(physicalDevice, &pFeatures->features);
        }
        return;
    }

    std::vector<void*> user_data(m_modules.size(), nullptr);
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_pre_get_features2(physicalDevice, pFeatures, user_data[i]);
            }
        }
    }

    real_fn(physicalDevice, pFeatures);

    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_post_get_features2(physicalDevice, pFeatures, user_data[i]);
            }
        }
    }
}

void LayerManager::dispatch_get_physical_device_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties
) {
    if (!pProperties) return;

    PFN_vkGetPhysicalDeviceProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2KHR");
    }

    if (!real_fn) {
        PFN_vkGetPhysicalDeviceProperties real_fn1 =
            (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
        if (real_fn1) {
            real_fn1(physicalDevice, &pProperties->properties);
        }
    } else {
        std::vector<void*> user_data(m_modules.size(), nullptr);
        {
            std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
            for (size_t i = 0; i < m_modules.size(); i++) {
                if (m_modules[i]->is_enabled()) {
                    m_modules[i]->on_pre_get_properties2(physicalDevice, pProperties, user_data[i]);
                }
            }
        }

        real_fn(physicalDevice, pProperties);

        {
            std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
            for (size_t i = 0; i < m_modules.size(); i++) {
                if (m_modules[i]->is_enabled()) {
                    m_modules[i]->on_post_get_properties2(physicalDevice, pProperties, user_data[i]);
                }
            }
        }
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
    for (size_t i = 0; i < m_modules.size(); i++) {
        if (m_modules[i]->is_enabled()) {
            m_modules[i]->on_post_get_properties2(physicalDevice, pProperties, nullptr);
        }
    }
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

    std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
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
    PFN_vkCreateDevice real_fn =
        (PFN_vkCreateDevice) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkCreateDevice");
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

    std::vector<void*> user_data(m_modules.size(), nullptr);
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_pre_create_device(physicalDevice, &modCreateInfo, has_mod_features ? &modFeatures : nullptr, enabledExtensions, user_data[i]);
            }
        }
    }

    modCreateInfo.enabledExtensionCount = (uint32_t) enabledExtensions.size();
    modCreateInfo.ppEnabledExtensionNames = enabledExtensions.empty() ? NULL : enabledExtensions.data();

    VkResult res = real_fn(physicalDevice, &modCreateInfo, pAllocator, pDevice);

    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_post_create_device(physicalDevice, (res == VK_SUCCESS && pDevice) ? *pDevice : VK_NULL_HANDLE, res, user_data[i]);
            }
        }
    }

    if (res == VK_SUCCESS && pDevice && *pDevice != VK_NULL_HANDLE) {
        add_emulated_device(*pDevice);
        m_last_device = *pDevice;
        m_device_count.fetch_add(1, std::memory_order_relaxed);
        m_primary_device.store(*pDevice, std::memory_order_relaxed);
        init_device_dispatch_table(*pDevice);
        LOGI("Created logical device %p with layer module emulation enabled", *pDevice);
    }

    return res;
}

void LayerManager::dispatch_destroy_device(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator
) {
    m_device_count.fetch_sub(1, std::memory_order_relaxed);
    remove_device_dispatch_table(device);

    {
        std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
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
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_destroy_device(device);
            }
        }
    }

    remove_emulated_device(device);

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
    PFN_vkCreateGraphicsPipelines real_fn =
        (PFN_vkCreateGraphicsPipelines) get_real_proc(get_last_instance(), device, "vkCreateGraphicsPipelines");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    if (!is_emulated_device(device) || createInfoCount == 0 || !pCreateInfos) {
        return real_fn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    }

    // Fast-path scan: Check if ANY module requires interception for this pipeline batch
    bool needs_interception = false;
    for (auto& mod : m_modules) {
        if (mod->is_enabled() && mod->needs_pipeline_interception(device, createInfoCount, pCreateInfos)) {
            needs_interception = true;
            break;
        }
    }

    // FAST-PATH: Zero heap allocations, zero copies!
    if (!needs_interception) {
        return real_fn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
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
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_modify_pipeline_create_info(device, i, modInfos[i], modVIStates[i], allocationsToFree);
            }
        }
    }

    VkResult res = real_fn(device, pipelineCache, createInfoCount, modInfos, pAllocator, pPipelines);

    if (res == VK_SUCCESS && pPipelines) {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_post_create_graphics_pipelines(device, createInfoCount, modInfos, pPipelines);
            }
        }
    }

    for (void* ptr : allocationsToFree) {
        free(ptr);
    }
    if (modVIStates != stackVIStates) free(modVIStates);
    if (modInfos != stackInfos) free(modInfos);

    return res;
}

VkResult LayerManager::dispatch_create_descriptor_set_layout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout
) {
    PFN_vkCreateDescriptorSetLayout real_fn =
        (PFN_vkCreateDescriptorSetLayout) get_real_proc(get_last_instance(), device, "vkCreateDescriptorSetLayout");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pCreateInfo) return real_fn(device, pCreateInfo, pAllocator, pSetLayout);

    VkDescriptorSetLayoutCreateInfo modInfo = *pCreateInfo;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_pre_create_descriptor_set_layout(device, modInfo);
            }
        }
    }

    VkResult res = real_fn(device, &modInfo, pAllocator, pSetLayout);

    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_post_create_descriptor_set_layout(device, pCreateInfo, res, (res == VK_SUCCESS && pSetLayout) ? *pSetLayout : VK_NULL_HANDLE);
            }
        }
    }

    return res;
}

void LayerManager::dispatch_destroy_descriptor_set_layout(
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_destroy_descriptor_set_layout(device, descriptorSetLayout);
            }
        }
    }

    PFN_vkDestroyDescriptorSetLayout real_fn =
        (PFN_vkDestroyDescriptorSetLayout) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorSetLayout");
    if (real_fn) {
        real_fn(device, descriptorSetLayout, pAllocator);
    }
}

VkResult LayerManager::dispatch_create_pipeline_layout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineLayout* pPipelineLayout
) {
    PFN_vkCreatePipelineLayout real_fn =
        (PFN_vkCreatePipelineLayout) get_real_proc(get_last_instance(), device, "vkCreatePipelineLayout");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(device, pCreateInfo, pAllocator, pPipelineLayout);

    if (res == VK_SUCCESS && pCreateInfo && pPipelineLayout) {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_post_create_pipeline_layout(device, pCreateInfo, res, *pPipelineLayout);
            }
        }
    }

    return res;
}

void LayerManager::dispatch_destroy_pipeline_layout(
    VkDevice device,
    VkPipelineLayout pipelineLayout,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_destroy_pipeline_layout(device, pipelineLayout);
            }
        }
    }

    PFN_vkDestroyPipelineLayout real_fn =
        (PFN_vkDestroyPipelineLayout) get_real_proc(get_last_instance(), device, "vkDestroyPipelineLayout");
    if (real_fn) {
        real_fn(device, pipelineLayout, pAllocator);
    }
}

VkResult LayerManager::dispatch_allocate_command_buffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers
) {
    PFN_vkAllocateCommandBuffers real_fn =
        (PFN_vkAllocateCommandBuffers) get_real_proc(get_last_instance(), device, "vkAllocateCommandBuffers");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(device, pAllocateInfo, pCommandBuffers);

    if (res == VK_SUCCESS && pAllocateInfo && pCommandBuffers) {
        {
            std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
            for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
                m_cmd_devices[(uint64_t)(uintptr_t)pCommandBuffers[i]] = device;
            }
        }
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_post_allocate_command_buffers(device, pAllocateInfo, res, pCommandBuffers);
            }
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
    if (pCommandBuffers) {
        std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
        for (uint32_t i = 0; i < commandBufferCount; i++) {
            m_cmd_devices.erase((uint64_t)(uintptr_t)pCommandBuffers[i]);
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_free_command_buffers(device, commandBufferCount, pCommandBuffers);
            }
        }
    }

    PFN_vkFreeCommandBuffers real_fn =
        (PFN_vkFreeCommandBuffers) get_real_proc(get_last_instance(), device, "vkFreeCommandBuffers");
    if (real_fn) {
        real_fn(device, commandPool, commandBufferCount, pCommandBuffers);
    }
}

VkResult LayerManager::dispatch_begin_command_buffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo
) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (!dt.BeginCommandBuffer) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pBeginInfo) {
        return dt.BeginCommandBuffer(commandBuffer, pBeginInfo);
    }

    VkCommandBufferBeginInfo modBeginInfo = *pBeginInfo;
    VkCommandBufferInheritanceInfo modInheritanceInfo{};
    bool has_mod_inheritance = false;

    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_begin_command_buffer(commandBuffer, pBeginInfo);
                mod->on_pre_begin_command_buffer(commandBuffer, pBeginInfo, modBeginInfo, modInheritanceInfo, has_mod_inheritance);
            }
        }
    }

    return dt.BeginCommandBuffer(commandBuffer, has_mod_inheritance ? &modBeginInfo : pBeginInfo);
}

VkResult LayerManager::dispatch_reset_command_buffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_reset_command_buffer(commandBuffer, flags);
            }
        }
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

    VkDescriptorUpdateTemplateCreateInfo modInfo = *pCreateInfo;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_pre_create_descriptor_update_template(device, modInfo);
            }
        }
    }

    VkResult res = VK_SUCCESS;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_create_descriptor_update_template(
                    device, &modInfo, pAllocator, pDescriptorUpdateTemplate, res)) {
                return res;
            }
        }
    }

    PFN_vkCreateDescriptorUpdateTemplate real_fn =
        (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplateKHR");
    }
    if (real_fn) {
        return real_fn(device, &modInfo, pAllocator, pDescriptorUpdateTemplate);
    }

    return VK_ERROR_INITIALIZATION_FAILED;
}

void LayerManager::dispatch_destroy_descriptor_update_template(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks* pAllocator
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_destroy_descriptor_update_template(
                    device, descriptorUpdateTemplate, pAllocator)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        PFN_vkDestroyDescriptorUpdateTemplate real_fn =
            (PFN_vkDestroyDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorUpdateTemplate");
        if (!real_fn) {
            real_fn = (PFN_vkDestroyDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorUpdateTemplateKHR");
        }
        if (real_fn) {
            real_fn(device, descriptorUpdateTemplate, pAllocator);
        }
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_push_descriptor_set_with_template(
                    commandBuffer, descriptorUpdateTemplate, layout, set, pData)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdPushDescriptorSetWithTemplateKHR real_fn =
            (PFN_vkCmdPushDescriptorSetWithTemplateKHR) get_real_proc(get_last_instance(), device, "vkCmdPushDescriptorSetWithTemplateKHR");
        if (real_fn) {
            real_fn(commandBuffer, descriptorUpdateTemplate, layout, set, pData);
        }
    }
}

VkResult LayerManager::dispatch_create_image(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage
) {
    PFN_vkCreateImage real_fn =
        (PFN_vkCreateImage) get_real_proc(get_last_instance(), device, "vkCreateImage");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(device, pCreateInfo, pAllocator, pImage);
    if (res == VK_SUCCESS && pCreateInfo && pImage && *pImage != VK_NULL_HANDLE) {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_post_create_image(device, pCreateInfo, res, *pImage);
            }
        }
    }
    return res;
}

void LayerManager::dispatch_destroy_image(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_destroy_image(device, image);
            }
        }
    }

    PFN_vkDestroyImage real_fn =
        (PFN_vkDestroyImage) get_real_proc(get_last_instance(), device, "vkDestroyImage");
    if (real_fn) {
        real_fn(device, image, pAllocator);
    }
}

VkResult LayerManager::dispatch_create_image_view(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView
) {
    PFN_vkCreateImageView real_fn =
        (PFN_vkCreateImageView) get_real_proc(get_last_instance(), device, "vkCreateImageView");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real_fn(device, pCreateInfo, pAllocator, pView);
    if (res == VK_SUCCESS && pCreateInfo && pView && *pView != VK_NULL_HANDLE) {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_post_create_image_view(device, pCreateInfo, res, *pView);
            }
        }
    }
    return res;
}

void LayerManager::dispatch_destroy_image_view(
    VkDevice device,
    VkImageView imageView,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_destroy_image_view(device, imageView);
            }
        }
    }

    PFN_vkDestroyImageView real_fn =
        (PFN_vkDestroyImageView) get_real_proc(get_last_instance(), device, "vkDestroyImageView");
    if (real_fn) {
        real_fn(device, imageView, pAllocator);
    }
}

void LayerManager::dispatch_cmd_begin_rendering(
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo* pRenderingInfo
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_begin_rendering(commandBuffer, pRenderingInfo)) {
                return;
            }
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
    if (m_divisor_mod && m_divisor_mod->is_enabled()) {
        m_divisor_mod->on_cmd_bind_vertex_buffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdBindVertexBuffers2) {
        dt.CmdBindVertexBuffers2(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
    } else if (dt.CmdBindVertexBuffers) {
        dt.CmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
    }
}

void LayerManager::dispatch_cmd_draw(
    VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance
) {
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



void LayerManager::dispatch_get_device_queue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue
) {
    PFN_vkGetDeviceQueue real_fn =
        (PFN_vkGetDeviceQueue) get_real_proc(get_last_instance(), device, "vkGetDeviceQueue");
    if (real_fn) {
        real_fn(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue != VK_NULL_HANDLE) {
            std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
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
    PFN_vkGetDeviceQueue2 real_fn =
        (PFN_vkGetDeviceQueue2) get_real_proc(get_last_instance(), device, "vkGetDeviceQueue2");
    if (real_fn) {
        real_fn(device, pQueueInfo, pQueue);
        if (pQueue && *pQueue != VK_NULL_HANDLE && pQueueInfo) {
            std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
            m_queue_devices[(uint64_t)(uintptr_t)*pQueue] = device;
            m_device_queues[(uint64_t)(uintptr_t)device] = {*pQueue, pQueueInfo->queueFamilyIndex};
        }
    }
}

bool LayerManager::get_device_queue_info(VkDevice device, VkQueue& outQueue, uint32_t& outQueueFamily) {
    std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_set_event2(commandBuffer, event, pDependencyInfo)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
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
}

void LayerManager::dispatch_cmd_reset_event2(
    VkCommandBuffer commandBuffer,
    VkEvent event,
    VkPipelineStageFlags2 stageMask
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_reset_event2(commandBuffer, event, stageMask)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
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
}

void LayerManager::dispatch_cmd_wait_events2(
    VkCommandBuffer commandBuffer,
    uint32_t eventCount,
    const VkEvent* pEvents,
    const VkDependencyInfo* pDependencyInfos
) {
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

void LayerManager::dispatch_cmd_pipeline_barrier2(
    VkCommandBuffer commandBuffer,
    const VkDependencyInfo* pDependencyInfo
) {
    if (m_sync2_mod && m_sync2_mod->is_enabled()) {
        if (m_sync2_mod->on_cmd_pipeline_barrier2(commandBuffer, pDependencyInfo)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdPipelineBarrier2) {
        dt.CmdPipelineBarrier2(commandBuffer, pDependencyInfo);
    }
}

void LayerManager::dispatch_cmd_write_timestamp2(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags2 stage,
    VkQueryPool queryPool,
    uint32_t query
) {
    if (m_sync2_mod && m_sync2_mod->is_enabled()) {
        if (m_sync2_mod->on_cmd_write_timestamp2(commandBuffer, stage, queryPool, query)) {
            return;
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    PFN_vkCmdWriteTimestamp2KHR real_fn = dt.CmdPipelineBarrier2 ? (PFN_vkCmdWriteTimestamp2KHR) get_real_proc(get_last_instance(), device, "vkCmdWriteTimestamp2KHR") : nullptr;
    if (real_fn) {
        real_fn(commandBuffer, stage, queryPool, query);
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
    VkDevice device = get_device_for_queue(queue);
    const auto& dt = get_dispatch_table(device);
    PFN_vkQueueWaitIdle real_fn = dt.QueueWaitIdle ? dt.QueueWaitIdle :
        (PFN_vkQueueWaitIdle) get_real_proc(get_last_instance(), device, "vkQueueWaitIdle");
    VkResult res = real_fn ? real_fn(queue) : VK_SUCCESS;
    if (res == VK_SUCCESS) {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_queue_wait_idle(queue);
            }
        }
    }
    return res;
}

VkResult LayerManager::dispatch_device_wait_idle(VkDevice device) {
    const auto& dt = get_dispatch_table(device);
    PFN_vkDeviceWaitIdle real_fn = dt.DeviceWaitIdle ? dt.DeviceWaitIdle :
        (PFN_vkDeviceWaitIdle) get_real_proc(get_last_instance(), device, "vkDeviceWaitIdle");
    VkResult res = real_fn ? real_fn(device) : VK_SUCCESS;
    if (res == VK_SUCCESS) {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_device_wait_idle(device);
            }
        }
    }
    return res;
}

bool LayerManager::is_timeline_semaphore(VkSemaphore semaphore) {
    if (semaphore == VK_NULL_HANDLE) return false;
    std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
    for (auto& mod : m_modules) {
        if (mod->is_enabled() && mod->is_timeline_semaphore(semaphore)) {
            return true;
        }
    }
    return false;
}

VkResult LayerManager::dispatch_create_semaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSemaphore* pSemaphore
) {
    PFN_vkCreateSemaphore real_fn =
        (PFN_vkCreateSemaphore) get_real_proc(get_last_instance(), device, "vkCreateSemaphore");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pCreateInfo) return real_fn(device, pCreateInfo, pAllocator, pSemaphore);

    VkSemaphoreCreateInfo modInfo = *pCreateInfo;
    std::vector<void*> user_data(m_modules.size(), nullptr);
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_pre_create_semaphore(device, modInfo, user_data[i]);
            }
        }
    }

    VkResult res = real_fn(device, &modInfo, pAllocator, pSemaphore);

    if (res == VK_SUCCESS && pSemaphore && *pSemaphore != VK_NULL_HANDLE) {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_post_create_semaphore(device, pCreateInfo, res, *pSemaphore, user_data[i]);
            }
        }
    }
    return res;
}

void LayerManager::dispatch_destroy_semaphore(
    VkDevice device,
    VkSemaphore semaphore,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_destroy_semaphore(device, semaphore);
            }
        }
    }

    PFN_vkDestroySemaphore real_fn =
        (PFN_vkDestroySemaphore) get_real_proc(get_last_instance(), device, "vkDestroySemaphore");
    if (real_fn) {
        real_fn(device, semaphore, pAllocator);
    }
}

VkResult LayerManager::dispatch_get_semaphore_counter_value(
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t* pValue
) {
    VkResult res = VK_SUCCESS;
    bool handled = false;
    if (m_timeline_mod && m_timeline_mod->is_enabled()) {
        if (m_timeline_mod->on_get_semaphore_counter_value(device, semaphore, pValue, res)) {
            return res;
        }
    }
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_get_semaphore_counter_value(device, semaphore, pValue, res)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        PFN_vkGetSemaphoreCounterValueKHR real_fn =
            (PFN_vkGetSemaphoreCounterValueKHR) get_real_proc(get_last_instance(), device, "vkGetSemaphoreCounterValueKHR");
        if (!real_fn) {
            real_fn = (PFN_vkGetSemaphoreCounterValueKHR) get_real_proc(get_last_instance(), device, "vkGetSemaphoreCounterValue");
        }
        if (real_fn) {
            res = real_fn(device, semaphore, pValue);
        } else {
            res = VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return res;
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_wait_semaphores(device, pWaitInfo, timeout, res)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        PFN_vkWaitSemaphoresKHR real_fn =
            (PFN_vkWaitSemaphoresKHR) get_real_proc(get_last_instance(), device, "vkWaitSemaphoresKHR");
        if (!real_fn) {
            real_fn = (PFN_vkWaitSemaphoresKHR) get_real_proc(get_last_instance(), device, "vkWaitSemaphores");
        }
        if (real_fn) {
            res = real_fn(device, pWaitInfo, timeout);
        } else {
            res = VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return res;
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_signal_semaphore(device, pSignalInfo, res)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        PFN_vkSignalSemaphoreKHR real_fn =
            (PFN_vkSignalSemaphoreKHR) get_real_proc(get_last_instance(), device, "vkSignalSemaphoreKHR");
        if (!real_fn) {
            real_fn = (PFN_vkSignalSemaphoreKHR) get_real_proc(get_last_instance(), device, "vkSignalSemaphore");
        }
        if (real_fn) {
            res = real_fn(device, pSignalInfo);
        } else {
            res = VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return res;
}

void LayerManager::dispatch_reset_query_pool(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_reset_query_pool(device, queryPool, firstQuery, queryCount)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        PFN_vkResetQueryPool real_fn =
            (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPool");
        if (!real_fn) {
            real_fn = (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPoolEXT");
        }
        if (real_fn) {
            real_fn(device, queryPool, firstQuery, queryCount);
        }
    }
}

VkResult LayerManager::dispatch_create_render_pass2(
    VkDevice device,
    const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass
) {
    VkResult res = VK_SUCCESS;
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_create_render_pass2(device, pCreateInfo, pAllocator, pRenderPass, res)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        PFN_vkCreateRenderPass2KHR real_fn =
            (PFN_vkCreateRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCreateRenderPass2KHR");
        if (!real_fn) {
            real_fn = (PFN_vkCreateRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCreateRenderPass2");
        }
        if (real_fn) {
            res = real_fn(device, pCreateInfo, pAllocator, pRenderPass);
        } else {
            res = VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return res;
}

void LayerManager::dispatch_cmd_begin_render_pass2(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo
) {
    if (m_renderpass2_mod && m_renderpass2_mod->is_enabled()) {
        if (m_renderpass2_mod->on_cmd_begin_render_pass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo)) {
            return;
        }
    }
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdBeginRenderPass2) {
        dt.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    } else if (dt.CmdBeginRenderPass) {
        VkSubpassContents contents = pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE;
        dt.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }
}

void LayerManager::dispatch_cmd_next_subpass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassBeginInfo* pSubpassBeginInfo,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    if (m_renderpass2_mod && m_renderpass2_mod->is_enabled()) {
        if (m_renderpass2_mod->on_cmd_next_subpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo)) {
            return;
        }
    }
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = get_dispatch_table(device);
    if (dt.CmdNextSubpass2) {
        dt.CmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
    }
}

void LayerManager::dispatch_cmd_end_render_pass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_get_buffer_device_address(device, pInfo, addr)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
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
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_bind_buffer_memory2(device, bindInfoCount, pBindInfos, res)) {
                return res;
            }
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
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_bind_image_memory2(device, bindInfoCount, pBindInfos, res)) {
                return res;
            }
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
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_get_buffer_memory_requirements2(device, pInfo, pMemoryRequirements)) {
                return;
            }
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
    }
}

void LayerManager::dispatch_get_image_memory_requirements2(
    VkDevice device,
    const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_get_image_memory_requirements2(device, pInfo, pMemoryRequirements)) {
                return;
            }
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
    }
}

void LayerManager::dispatch_get_image_sparse_memory_requirements2(
    VkDevice device,
    const VkImageSparseMemoryRequirementsInfo2* pInfo,
    uint32_t* pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2* pSparseMemoryRequirements
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_get_image_sparse_memory_requirements2(
                    device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements)) {
                return;
            }
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
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_update_descriptor_set_with_template(
                    device, descriptorSet, descriptorUpdateTemplate, pData)) {
                return;
            }
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

void LayerManager::dispatch_get_descriptor_set_layout_support(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    VkDescriptorSetLayoutSupport* pSupport
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_get_descriptor_set_layout_support(device, pCreateInfo, pSupport)) {
                return;
            }
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
        pSupport->supported = VK_TRUE;
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
    }
}

VkResult LayerManager::dispatch_enumerate_physical_device_groups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties
) {
    VkResult res = VK_SUCCESS;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_enumerate_physical_device_groups(
                    instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties, res)) {
                return res;
            }
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

    return VK_ERROR_INITIALIZATION_FAILED;
}

void LayerManager::dispatch_trim_command_pool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolTrimFlags flags
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_trim_command_pool(device, commandPool, flags);
            }
        }
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
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_get_device_group_peer_memory_features(
                    device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures)) {
                return;
            }
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
            return res;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.CreateSwapchain) {
        return dt.CreateSwapchain(device, pCreateInfo, pAllocator, pSwapchain);
    }
    PFN_vkCreateSwapchainKHR real_fn =
        (PFN_vkCreateSwapchainKHR) get_real_proc(get_last_instance(), device, "vkCreateSwapchainKHR");
    if (real_fn) {
        return real_fn(device, pCreateInfo, pAllocator, pSwapchain);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void LayerManager::dispatch_destroy_swapchain(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator
) {
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
            return res;
        }
    }
    const auto& dt = get_dispatch_table(device);
    if (dt.GetSwapchainImages) {
        return dt.GetSwapchainImages(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    }
    PFN_vkGetSwapchainImagesKHR real_fn =
        (PFN_vkGetSwapchainImagesKHR) get_real_proc(get_last_instance(), device, "vkGetSwapchainImagesKHR");
    if (real_fn) {
        return real_fn(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
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
        return dt.AcquireNextImage(device, swapchain, timeout, semaphore, fence, pImageIndex);
    }
    PFN_vkAcquireNextImageKHR real_fn =
        (PFN_vkAcquireNextImageKHR) get_real_proc(get_last_instance(), device, "vkAcquireNextImageKHR");
    if (real_fn) {
        return real_fn(device, swapchain, timeout, semaphore, fence, pImageIndex);
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
        return dt.AcquireNextImage2(device, pAcquireInfo, pImageIndex);
    }
    PFN_vkAcquireNextImage2KHR real_fn =
        (PFN_vkAcquireNextImage2KHR) get_real_proc(get_last_instance(), device, "vkAcquireNextImage2KHR");
    if (real_fn) {
        return real_fn(device, pAcquireInfo, pImageIndex);
    }
    return dispatch_acquire_next_image(device, pAcquireInfo->swapchain, pAcquireInfo->timeout, pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex);
}

VkResult LayerManager::dispatch_queue_present(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo
) {
    VkResult res = VK_SUCCESS;
    if (m_swapchain_mod && m_swapchain_mod->is_enabled()) {
        if (m_swapchain_mod->on_queue_present(queue, pPresentInfo, res)) {
            return res;
        }
    }
    VkDevice device = get_device_for_queue(queue);
    const auto& dt = get_dispatch_table(device);
    if (dt.QueuePresent) {
        return dt.QueuePresent(queue, pPresentInfo);
    }
    PFN_vkQueuePresentKHR real_fn =
        (PFN_vkQueuePresentKHR) get_real_proc(get_last_instance(), device, "vkQueuePresentKHR");
    if (real_fn) {
        return real_fn(queue, pPresentInfo);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
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
        return real_fn(physicalDevice, surface, pSurfaceCapabilities);
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
    if (real_fn) {
        return real_fn(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
    }
    return dispatch_get_physical_device_surface_capabilities(physicalDevice, pSurfaceInfo->surface, &pSurfaceCapabilities->surfaceCapabilities);
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





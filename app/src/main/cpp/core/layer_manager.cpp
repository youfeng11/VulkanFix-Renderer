#include "layer_manager.h"
#include "driver_loader.h"

LayerManager& LayerManager::get() {
    static LayerManager s_instance;
    return s_instance;
}

void LayerManager::register_module(std::unique_ptr<IVulkanLayerModule> module) {
    std::lock_guard<std::mutex> lock(m_modules_mutex);
    LOGI("Registering Vulkan layer module: %s", module->get_name());
    m_modules.push_back(std::move(module));
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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

    std::lock_guard<std::mutex> lock(m_modules_mutex);
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_pre_get_features2(physicalDevice, pFeatures, user_data[i]);
            }
        }
    }

    real_fn(physicalDevice, pFeatures);

    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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
        return;
    }

    std::vector<void*> user_data(m_modules.size(), nullptr);
    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_pre_get_properties2(physicalDevice, pProperties, user_data[i]);
            }
        }
    }

    real_fn(physicalDevice, pProperties);

    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_post_get_properties2(physicalDevice, pProperties, user_data[i]);
            }
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (size_t i = 0; i < m_modules.size(); i++) {
            if (m_modules[i]->is_enabled()) {
                m_modules[i]->on_post_create_device(physicalDevice, (res == VK_SUCCESS && pDevice) ? *pDevice : VK_NULL_HANDLE, res, user_data[i]);
            }
        }
    }

    if (res == VK_SUCCESS && pDevice && *pDevice != VK_NULL_HANDLE) {
        add_emulated_device(*pDevice);
        LOGI("Created logical device %p with layer module emulation enabled", *pDevice);
    }

    return res;
}

void LayerManager::dispatch_destroy_device(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_pre_create_descriptor_set_layout(device, modInfo);
            }
        }
    }

    VkResult res = real_fn(device, &modInfo, pAllocator, pSetLayout);

    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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
    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
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
    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_begin_command_buffer(commandBuffer, pBeginInfo);
            }
        }
    }

    PFN_vkBeginCommandBuffer real_fn =
        (PFN_vkBeginCommandBuffer) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkBeginCommandBuffer");
    if (real_fn) {
        return real_fn(commandBuffer, pBeginInfo);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult LayerManager::dispatch_reset_command_buffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags
) {
    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_reset_command_buffer(commandBuffer, flags);
            }
        }
    }

    PFN_vkResetCommandBuffer real_fn =
        (PFN_vkResetCommandBuffer) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkResetCommandBuffer");
    if (real_fn) {
        return real_fn(commandBuffer, flags);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult LayerManager::dispatch_create_descriptor_update_template(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate
) {
    PFN_vkCreateDescriptorUpdateTemplate real_fn =
        (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplateKHR");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pCreateInfo) return real_fn(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);

    VkDescriptorUpdateTemplateCreateInfo modInfo = *pCreateInfo;
    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_pre_create_descriptor_update_template(device, modInfo);
            }
        }
    }

    return real_fn(device, &modInfo, pAllocator, pDescriptorUpdateTemplate);
}

void LayerManager::dispatch_destroy_descriptor_update_template(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_destroy_descriptor_update_template(device, descriptorUpdateTemplate);
            }
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

void LayerManager::dispatch_cmd_push_descriptor_set(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t set,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites
) {
    bool handled = false;
    {
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_push_descriptor_set(
                    commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        PFN_vkCmdPushDescriptorSetKHR real_fn =
            (PFN_vkCmdPushDescriptorSetKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkCmdPushDescriptorSetKHR");
        if (real_fn) {
            real_fn(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
        }
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
        std::lock_guard<std::mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_push_descriptor_set_with_template(
                    commandBuffer, descriptorUpdateTemplate, layout, set, pData)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        PFN_vkCmdPushDescriptorSetWithTemplateKHR real_fn =
            (PFN_vkCmdPushDescriptorSetWithTemplateKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkCmdPushDescriptorSetWithTemplateKHR");
        if (real_fn) {
            real_fn(commandBuffer, descriptorUpdateTemplate, layout, set, pData);
        }
    }
}


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
        return;
    }

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
        LOGI("Created logical device %p with layer module emulation enabled", *pDevice);
    }

    return res;
}

void LayerManager::dispatch_destroy_device(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator
) {
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
    if (!pBeginInfo) {
        PFN_vkBeginCommandBuffer real_fn =
            (PFN_vkBeginCommandBuffer) get_real_proc(get_last_instance(), device, "vkBeginCommandBuffer");
        return real_fn ? real_fn(commandBuffer, pBeginInfo) : VK_ERROR_INITIALIZATION_FAILED;
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

    PFN_vkBeginCommandBuffer real_fn =
        (PFN_vkBeginCommandBuffer) get_real_proc(get_last_instance(), device, "vkBeginCommandBuffer");
    if (real_fn) {
        return real_fn(commandBuffer, has_mod_inheritance ? &modBeginInfo : pBeginInfo);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
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
    PFN_vkResetCommandBuffer real_fn =
        (PFN_vkResetCommandBuffer) get_real_proc(get_last_instance(), device, "vkResetCommandBuffer");
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_push_descriptor_set(
                    commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdPushDescriptorSetKHR real_fn =
            (PFN_vkCmdPushDescriptorSetKHR) get_real_proc(get_last_instance(), device, "vkCmdPushDescriptorSetKHR");
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
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdBeginRenderingKHR real_fn =
            (PFN_vkCmdBeginRenderingKHR) get_real_proc(get_last_instance(), device, "vkCmdBeginRenderingKHR");
        if (!real_fn) {
            real_fn = (PFN_vkCmdBeginRenderingKHR) get_real_proc(get_last_instance(), device, "vkCmdBeginRendering");
        }
        if (real_fn) {
            real_fn(commandBuffer, pRenderingInfo);
        }
    }
}

void LayerManager::dispatch_cmd_end_rendering(
    VkCommandBuffer commandBuffer
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_end_rendering(commandBuffer)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdEndRenderingKHR real_fn =
            (PFN_vkCmdEndRenderingKHR) get_real_proc(get_last_instance(), device, "vkCmdEndRenderingKHR");
        if (!real_fn) {
            real_fn = (PFN_vkCmdEndRenderingKHR) get_real_proc(get_last_instance(), device, "vkCmdEndRendering");
        }
        if (real_fn) {
            real_fn(commandBuffer);
        }
    }
}

VkDevice LayerManager::get_device_for_cmd(VkCommandBuffer cmd) {
    {
        std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
        auto it = m_cmd_devices.find((uint64_t)(uintptr_t)cmd);
        if (it != m_cmd_devices.end()) {
            return it->second;
        }
    }
    return m_last_device.load();
}

void LayerManager::dispatch_destroy_pipeline(
    VkDevice device,
    VkPipeline pipeline,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_destroy_pipeline(device, pipeline);
            }
        }
    }

    PFN_vkDestroyPipeline real_fn = (PFN_vkDestroyPipeline)
        get_real_proc(get_last_instance(), device, "vkDestroyPipeline");
    if (real_fn) {
        real_fn(device, pipeline, pAllocator);
    }
}

void LayerManager::dispatch_cmd_bind_pipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_cmd_bind_pipeline(commandBuffer, pipelineBindPoint, pipeline);
            }
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    PFN_vkCmdBindPipeline real_fn = (PFN_vkCmdBindPipeline)
        get_real_proc(get_last_instance(), device, "vkCmdBindPipeline");
    if (real_fn) {
        real_fn(commandBuffer, pipelineBindPoint, pipeline);
    }
}

void LayerManager::dispatch_cmd_bind_vertex_buffers(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets
) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_cmd_bind_vertex_buffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
            }
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    PFN_vkCmdBindVertexBuffers real_fn = (PFN_vkCmdBindVertexBuffers)
        get_real_proc(get_last_instance(), device, "vkCmdBindVertexBuffers");
    if (real_fn) {
        real_fn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
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
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_cmd_bind_vertex_buffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
            }
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    PFN_vkCmdBindVertexBuffers2 real_fn = (PFN_vkCmdBindVertexBuffers2)
        get_real_proc(get_last_instance(), device, "vkCmdBindVertexBuffers2");
    if (!real_fn) {
        real_fn = (PFN_vkCmdBindVertexBuffers2)
            get_real_proc(get_last_instance(), device, "vkCmdBindVertexBuffers2EXT");
    }
    if (real_fn) {
        real_fn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
    } else {
        PFN_vkCmdBindVertexBuffers real_fn1 = (PFN_vkCmdBindVertexBuffers)
            get_real_proc(get_last_instance(), device, "vkCmdBindVertexBuffers");
        if (real_fn1) {
            real_fn1(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
        }
    }
}

void LayerManager::dispatch_cmd_draw(
    VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_draw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdDraw real_fn = (PFN_vkCmdDraw)
            get_real_proc(get_last_instance(), device, "vkCmdDraw");
        if (real_fn) {
            real_fn(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
        }
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_draw_indexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdDrawIndexed real_fn = (PFN_vkCmdDrawIndexed)
            get_real_proc(get_last_instance(), device, "vkCmdDrawIndexed");
        if (real_fn) {
            real_fn(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        }
    }
}

VkDevice LayerManager::get_device_for_queue(VkQueue queue) {
    std::lock_guard<std::mutex> lock(m_cmd_device_mutex);
    auto it = m_queue_devices.find((uint64_t)(uintptr_t)queue);
    if (it != m_queue_devices.end()) {
        return it->second;
    }
    return m_last_device.load();
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_wait_events2(commandBuffer, eventCount, pEvents, pDependencyInfos)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
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
}

void LayerManager::dispatch_cmd_pipeline_barrier2(
    VkCommandBuffer commandBuffer,
    const VkDependencyInfo* pDependencyInfo
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_pipeline_barrier2(commandBuffer, pDependencyInfo)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdPipelineBarrier2KHR real_fn =
            (PFN_vkCmdPipelineBarrier2KHR) get_real_proc(get_last_instance(), device, "vkCmdPipelineBarrier2KHR");
        if (!real_fn) {
            real_fn = (PFN_vkCmdPipelineBarrier2KHR) get_real_proc(get_last_instance(), device, "vkCmdPipelineBarrier2");
        }
        if (real_fn) {
            real_fn(commandBuffer, pDependencyInfo);
        }
    }
}

void LayerManager::dispatch_cmd_write_timestamp2(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags2 stage,
    VkQueryPool queryPool,
    uint32_t query
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_write_timestamp2(commandBuffer, stage, queryPool, query)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdWriteTimestamp2KHR real_fn =
            (PFN_vkCmdWriteTimestamp2KHR) get_real_proc(get_last_instance(), device, "vkCmdWriteTimestamp2KHR");
        if (!real_fn) {
            real_fn = (PFN_vkCmdWriteTimestamp2KHR) get_real_proc(get_last_instance(), device, "vkCmdWriteTimestamp2");
        }
        if (real_fn) {
            real_fn(commandBuffer, stage, queryPool, query);
        }
    }
}

VkResult LayerManager::dispatch_queue_submit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence
) {
    VkResult res = VK_SUCCESS;
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_queue_submit2(queue, submitCount, pSubmits, fence, res)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_queue(queue);
        PFN_vkQueueSubmit2KHR real_fn =
            (PFN_vkQueueSubmit2KHR) get_real_proc(get_last_instance(), device, "vkQueueSubmit2KHR");
        if (!real_fn) {
            real_fn = (PFN_vkQueueSubmit2KHR) get_real_proc(get_last_instance(), device, "vkQueueSubmit2");
        }
        if (real_fn) {
            res = real_fn(queue, submitCount, pSubmits, fence);
        } else {
            LOGE("vkQueueSubmit2: neither handled by module nor found in native driver!");
            res = VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return res;
}

VkResult LayerManager::dispatch_queue_submit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence
) {
    VkResult res = VK_SUCCESS;
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_queue_submit(queue, submitCount, pSubmits, fence, res)) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        VkDevice device = get_device_for_queue(queue);
        PFN_vkQueueSubmit real_fn =
            (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), device, "vkQueueSubmit");
        if (real_fn) {
            res = real_fn(queue, submitCount, pSubmits, fence);
        } else {
            res = VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return res;
}

VkResult LayerManager::dispatch_queue_wait_idle(VkQueue queue) {
    PFN_vkQueueWaitIdle real_fn =
        (PFN_vkQueueWaitIdle) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueWaitIdle");
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
    PFN_vkDeviceWaitIdle real_fn =
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_begin_render_pass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdBeginRenderPass2KHR real_fn =
            (PFN_vkCmdBeginRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass2KHR");
        if (!real_fn) {
            real_fn = (PFN_vkCmdBeginRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass2");
        }
        if (real_fn) {
            real_fn(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
        }
    }
}

void LayerManager::dispatch_cmd_next_subpass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassBeginInfo* pSubpassBeginInfo,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_next_subpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdNextSubpass2KHR real_fn =
            (PFN_vkCmdNextSubpass2KHR) get_real_proc(get_last_instance(), device, "vkCmdNextSubpass2KHR");
        if (!real_fn) {
            real_fn = (PFN_vkCmdNextSubpass2KHR) get_real_proc(get_last_instance(), device, "vkCmdNextSubpass2");
        }
        if (real_fn) {
            real_fn(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
        }
    }
}

void LayerManager::dispatch_cmd_end_render_pass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_end_render_pass2(commandBuffer, pSubpassEndInfo)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdEndRenderPass2KHR real_fn =
            (PFN_vkCmdEndRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCmdEndRenderPass2KHR");
        if (!real_fn) {
            real_fn = (PFN_vkCmdEndRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCmdEndRenderPass2");
        }
        if (real_fn) {
            real_fn(commandBuffer, pSubpassEndInfo);
        }
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_draw_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdDrawIndirectCountKHR real_fn =
            (PFN_vkCmdDrawIndirectCountKHR) get_real_proc(get_last_instance(), device, "vkCmdDrawIndirectCountKHR");
        if (!real_fn) {
            real_fn = (PFN_vkCmdDrawIndirectCountKHR) get_real_proc(get_last_instance(), device, "vkCmdDrawIndirectCount");
        }
        if (!real_fn) {
            real_fn = (PFN_vkCmdDrawIndirectCountKHR) get_real_proc(get_last_instance(), device, "vkCmdDrawIndirectCountAMD");
        }
        if (real_fn) {
            real_fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
        }
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
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_draw_indexed_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride)) {
                handled = true;
                break;
            }
        }
    }
    if (!handled) {
        VkDevice device = get_device_for_cmd(commandBuffer);
        PFN_vkCmdDrawIndexedIndirectCountKHR real_fn =
            (PFN_vkCmdDrawIndexedIndirectCountKHR) get_real_proc(get_last_instance(), device, "vkCmdDrawIndexedIndirectCountKHR");
        if (!real_fn) {
            real_fn = (PFN_vkCmdDrawIndexedIndirectCountKHR) get_real_proc(get_last_instance(), device, "vkCmdDrawIndexedIndirectCount");
        }
        if (!real_fn) {
            real_fn = (PFN_vkCmdDrawIndexedIndirectCountKHR) get_real_proc(get_last_instance(), device, "vkCmdDrawIndexedIndirectCountAMD");
        }
        if (real_fn) {
            real_fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
        }
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
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled() && mod->on_cmd_dispatch_base(
                    commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ)) {
                return;
            }
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    PFN_vkCmdDispatchBase real_fn =
        (PFN_vkCmdDispatchBase) get_real_proc(get_last_instance(), device, "vkCmdDispatchBase");
    if (!real_fn) {
        real_fn = (PFN_vkCmdDispatchBase) get_real_proc(get_last_instance(), device, "vkCmdDispatchBaseKHR");
    }
    if (real_fn) {
        real_fn(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
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
    {
        std::lock_guard<std::recursive_mutex> lock(m_modules_mutex);
        for (auto& mod : m_modules) {
            if (mod->is_enabled()) {
                mod->on_cmd_set_device_mask(commandBuffer, deviceMask);
            }
        }
    }

    VkDevice device = get_device_for_cmd(commandBuffer);
    PFN_vkCmdSetDeviceMask real_fn =
        (PFN_vkCmdSetDeviceMask) get_real_proc(get_last_instance(), device, "vkCmdSetDeviceMask");
    if (!real_fn) {
        real_fn = (PFN_vkCmdSetDeviceMask) get_real_proc(get_last_instance(), device, "vkCmdSetDeviceMaskKHR");
    }
    if (real_fn) {
        real_fn(commandBuffer, deviceMask);
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




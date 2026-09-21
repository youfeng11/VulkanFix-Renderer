#include "push_descriptor.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include <string.h>
#include <stdlib.h>

PushDescriptorModule::PushDescriptorModule() {
    LOGI("Initialized Vulkan VK_KHR_push_descriptor module");
}

bool PushDescriptorModule::is_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_native_support.end()) {
        return it->second;
    }

    PFN_vkEnumerateDeviceExtensionProperties real_fn =
        (PFN_vkEnumerateDeviceExtensionProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    bool native = false;
    if (real_fn) {
        uint32_t count = 0;
        if (real_fn(physDev, NULL, &count, NULL) == VK_SUCCESS && count > 0) {
            std::vector<VkExtensionProperties> exts(count);
            if (real_fn(physDev, NULL, &count, exts.data()) == VK_SUCCESS) {
                for (const auto& e : exts) {
                    if (strcmp(e.extensionName, "VK_KHR_push_descriptor") == 0) {
                        native = true;
                        break;
                    }
                }
            }
        }
    }

    m_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native VK_KHR_push_descriptor, enabling emulation layer!", physDev);
    } else {
        LOGI("Physical device %p natively supports VK_KHR_push_descriptor", physDev);
    }
    return native;
}

void PushDescriptorModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    if (is_device_native(physicalDevice)) return;

    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, "VK_KHR_push_descriptor") == 0) {
            return;
        }
    }

    VkExtensionProperties prop{};
    strncpy(prop.extensionName, "VK_KHR_push_descriptor", VK_MAX_EXTENSION_NAME_SIZE - 1);
    prop.specVersion = VK_KHR_PUSH_DESCRIPTOR_SPEC_VERSION;
    extensions.push_back(prop);
    LOGI("Emulated extension: VK_KHR_push_descriptor (spec version %u)", prop.specVersion);
}

void PushDescriptorModule::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties) return;
    void* curr = pProperties->pNext;
    while (curr != NULL) {
        VkBaseOutStructure* header = (VkBaseOutStructure*) curr;
        if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR) {
            VkPhysicalDevicePushDescriptorPropertiesKHR* props =
                (VkPhysicalDevicePushDescriptorPropertiesKHR*) header;
            props->maxPushDescriptors = 32;
            LOG_OPT_DEBUG("PushDescriptor: set maxPushDescriptors = 32 in VkPhysicalDevicePushDescriptorPropertiesKHR");
        }
        curr = header->pNext;
    }
}

void PushDescriptorModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    if (is_device_native(physicalDevice)) return;

    for (auto it = enabledExtensions.begin(); it != enabledExtensions.end();) {
        if (strcmp(*it, "VK_KHR_push_descriptor") == 0) {
            it = enabledExtensions.erase(it);
            LOGI("vkCreateDevice: stripped VK_KHR_push_descriptor from enabledExtensions for physical device %p", physicalDevice);
        } else {
            ++it;
        }
    }
}

void PushDescriptorModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    PFN_vkDestroyDescriptorPool real_destroy_pool = (PFN_vkDestroyDescriptorPool)
        get_real_proc(get_last_instance(), device, "vkDestroyDescriptorPool");

    for (auto it = m_cmd_states.begin(); it != m_cmd_states.end();) {
        if (it->second.device == device) {
            if (real_destroy_pool) {
                for (VkDescriptorPool pool : it->second.pools) {
                    real_destroy_pool(device, pool, NULL);
                }
            }
            it = m_cmd_states.erase(it);
        } else {
            ++it;
        }
    }
    m_pipeline_layouts.clear();
    m_push_layouts.clear();
}

void PushDescriptorModule::on_pre_create_descriptor_set_layout(
    VkDevice device,
    VkDescriptorSetLayoutCreateInfo& createInfo
) {
    if (createInfo.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
        createInfo.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        LOG_OPT_DEBUG("PushDescriptor: stripped PUSH_DESCRIPTOR_BIT from VkDescriptorSetLayoutCreateInfo");
    }
}

void PushDescriptorModule::on_post_create_descriptor_set_layout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    VkResult result,
    VkDescriptorSetLayout setLayout
) {
    if (result == VK_SUCCESS && pCreateInfo && setLayout != VK_NULL_HANDLE) {
        if (pCreateInfo->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_push_layouts.insert((uint64_t)(uintptr_t)setLayout);
            LOGI("PushDescriptor: registered push descriptor layout %p", setLayout);
        }
    }
}

void PushDescriptorModule::on_destroy_descriptor_set_layout(
    VkDevice device,
    VkDescriptorSetLayout setLayout
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_push_layouts.erase((uint64_t)(uintptr_t)setLayout);
}

void PushDescriptorModule::on_post_create_pipeline_layout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    VkResult result,
    VkPipelineLayout pipelineLayout
) {
    if (result == VK_SUCCESS && pCreateInfo && pipelineLayout != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<VkDescriptorSetLayout> layouts;
        if (pCreateInfo->setLayoutCount > 0 && pCreateInfo->pSetLayouts != NULL) {
            layouts.assign(pCreateInfo->pSetLayouts, pCreateInfo->pSetLayouts + pCreateInfo->setLayoutCount);
        }
        m_pipeline_layouts[(uint64_t)(uintptr_t)pipelineLayout] = std::move(layouts);
    }
}

void PushDescriptorModule::on_destroy_pipeline_layout(
    VkDevice device,
    VkPipelineLayout pipelineLayout
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pipeline_layouts.erase((uint64_t)(uintptr_t)pipelineLayout);
}

void PushDescriptorModule::on_post_allocate_command_buffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkResult result,
    VkCommandBuffer* pCommandBuffers
) {
    if (result == VK_SUCCESS && pAllocateInfo && pCommandBuffers) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            m_cmd_states[(uint64_t)(uintptr_t)pCommandBuffers[i]].device = device;
        }
    }
}

void PushDescriptorModule::on_free_command_buffers(
    VkDevice device,
    uint32_t count,
    const VkCommandBuffer* pCommandBuffers
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    PFN_vkDestroyDescriptorPool real_destroy_pool = (PFN_vkDestroyDescriptorPool)
        get_real_proc(get_last_instance(), device, "vkDestroyDescriptorPool");

    for (uint32_t i = 0; i < count; ++i) {
        auto it = m_cmd_states.find((uint64_t)(uintptr_t)pCommandBuffers[i]);
        if (it != m_cmd_states.end()) {
            if (real_destroy_pool) {
                for (VkDescriptorPool pool : it->second.pools) {
                    real_destroy_pool(device, pool, NULL);
                }
            }
            m_cmd_states.erase(it);
        }
    }
}

void PushDescriptorModule::on_begin_command_buffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo
) {
    reset_cmd_pools(commandBuffer);
}

void PushDescriptorModule::on_reset_command_buffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags
) {
    reset_cmd_pools(commandBuffer);
}

void PushDescriptorModule::reset_cmd_pools(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cmd_states.find((uint64_t)(uintptr_t)cmd);
    if (it == m_cmd_states.end()) return;

    PFN_vkResetDescriptorPool real_reset_pool = (PFN_vkResetDescriptorPool)
        get_real_proc(get_last_instance(), it->second.device, "vkResetDescriptorPool");
    if (real_reset_pool) {
        for (VkDescriptorPool pool : it->second.pools) {
            real_reset_pool(it->second.device, pool, 0);
        }
    }
    it->second.current_pool_idx = 0;
    it->second.current_pool_allocated = 0;
}

void PushDescriptorModule::on_pre_create_descriptor_update_template(
    VkDevice device,
    VkDescriptorUpdateTemplateCreateInfo& createInfo
) {
    if (createInfo.templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR) {
        createInfo.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        LOG_OPT_DEBUG("PushDescriptor: redirected templateType to DESCRIPTOR_SET");
    }
}

VkDevice PushDescriptorModule::get_device_for_cmd(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cmd_states.find((uint64_t)(uintptr_t)cmd);
    if (it != m_cmd_states.end() && it->second.device != VK_NULL_HANDLE) {
        return it->second.device;
    }
    return LayerManager::get().get_primary_device();
}

VkDescriptorSetLayout PushDescriptorModule::get_set_layout(VkPipelineLayout layout, uint32_t set) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pipeline_layouts.find((uint64_t)(uintptr_t)layout);
    if (it != m_pipeline_layouts.end()) {
        if (set < it->second.size()) {
            return it->second[set];
        }
    }
    return VK_NULL_HANDLE;
}

VkDescriptorPool PushDescriptorModule::create_pool(VkDevice device, uint32_t maxSets) {
    PFN_vkCreateDescriptorPool real_create_pool = (PFN_vkCreateDescriptorPool)
        get_real_proc(get_last_instance(), device, "vkCreateDescriptorPool");
    if (!real_create_pool) return VK_NULL_HANDLE;

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, maxSets * 2 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxSets * 4 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxSets * 4 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxSets * 2 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, maxSets * 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, maxSets * 2 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxSets * 4 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxSets * 2 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, maxSets },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = sizeof(poolSizes) / sizeof(poolSizes[0]);
    poolInfo.pPoolSizes = poolSizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult res = real_create_pool(device, &poolInfo, NULL, &pool);
    if (res != VK_SUCCESS) {
        LOGE("PushDescriptor: failed to create internal descriptor pool: %d", res);
        return VK_NULL_HANDLE;
    }
    return pool;
}

VkDescriptorSet PushDescriptorModule::allocate_push_set(VkDevice device, VkCommandBuffer cmd, VkDescriptorSetLayout setLayout) {
    std::lock_guard<std::mutex> lock(m_mutex);
    CmdPushState& state = m_cmd_states[(uint64_t)(uintptr_t)cmd];
    state.device = device;

    PFN_vkAllocateDescriptorSets real_alloc = (PFN_vkAllocateDescriptorSets)
        get_real_proc(get_last_instance(), device, "vkAllocateDescriptorSets");
    if (!real_alloc) return VK_NULL_HANDLE;

    constexpr uint32_t MAX_SETS_PER_POOL = 256;

    if (state.pools.empty()) {
        VkDescriptorPool pool = create_pool(device, MAX_SETS_PER_POOL);
        if (pool == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        state.pools.push_back(pool);
        state.current_pool_idx = 0;
        state.current_pool_allocated = 0;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = state.pools[state.current_pool_idx];
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &setLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult res = real_alloc(device, &allocInfo, &set);
    if (res == VK_SUCCESS && set != VK_NULL_HANDLE) {
        state.current_pool_allocated++;
        return set;
    }

    if (state.current_pool_idx + 1 < state.pools.size()) {
        state.current_pool_idx++;
        state.current_pool_allocated = 0;
    } else {
        VkDescriptorPool new_pool = create_pool(device, MAX_SETS_PER_POOL);
        if (new_pool == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        state.pools.push_back(new_pool);
        state.current_pool_idx = state.pools.size() - 1;
        state.current_pool_allocated = 0;
    }

    allocInfo.descriptorPool = state.pools[state.current_pool_idx];
    res = real_alloc(device, &allocInfo, &set);
    if (res == VK_SUCCESS) {
        state.current_pool_allocated++;
        return set;
    }

    LOGE("PushDescriptor: failed to allocate descriptor set: %d", res);
    return VK_NULL_HANDLE;
}

bool PushDescriptorModule::on_cmd_push_descriptor_set(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t set,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites
) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    if (device == VK_NULL_HANDLE) return false;

    VkDescriptorSetLayout setLayout = get_set_layout(layout, set);
    if (setLayout == VK_NULL_HANDLE) {
        LOGE("PushDescriptor: no descriptor set layout registered for pipeline layout %p set %u", layout, set);
        return false;
    }

    VkDescriptorSet descSet = allocate_push_set(device, commandBuffer, setLayout);
    if (descSet == VK_NULL_HANDLE) {
        LOGE("PushDescriptor: failed to allocate internal descriptor set!");
        return true;
    }

    constexpr uint32_t SBO_LIMIT = 16;
    VkWriteDescriptorSet stackWrites[SBO_LIMIT];
    VkWriteDescriptorSet* modWrites = (descriptorWriteCount <= SBO_LIMIT) ?
        stackWrites : (VkWriteDescriptorSet*) malloc(sizeof(VkWriteDescriptorSet) * descriptorWriteCount);
    if (!modWrites) return true;

    memcpy(modWrites, pDescriptorWrites, sizeof(VkWriteDescriptorSet) * descriptorWriteCount);
    for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
        modWrites[i].dstSet = descSet;
    }

    PFN_vkUpdateDescriptorSets real_update = (PFN_vkUpdateDescriptorSets)
        get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSets");
    if (real_update) {
        real_update(device, descriptorWriteCount, modWrites, 0, NULL);
    }

    if (modWrites != stackWrites) free(modWrites);

    PFN_vkCmdBindDescriptorSets real_bind = (PFN_vkCmdBindDescriptorSets)
        get_real_proc(get_last_instance(), device, "vkCmdBindDescriptorSets");
    if (real_bind) {
        real_bind(commandBuffer, pipelineBindPoint, layout, set, 1, &descSet, 0, NULL);
    }

    LOG_OPT_DEBUG("PushDescriptor: emulated vkCmdPushDescriptorSetKHR for cmd %p set %u", commandBuffer, set);
    return true;
}

bool PushDescriptorModule::on_cmd_push_descriptor_set_with_template(
    VkCommandBuffer commandBuffer,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    VkPipelineLayout layout,
    uint32_t set,
    const void* pData
) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    if (device == VK_NULL_HANDLE) return false;

    VkDescriptorSetLayout setLayout = get_set_layout(layout, set);
    if (setLayout == VK_NULL_HANDLE) return false;

    VkDescriptorSet descSet = allocate_push_set(device, commandBuffer, setLayout);
    if (descSet == VK_NULL_HANDLE) return true;

    PFN_vkUpdateDescriptorSetWithTemplate real_update_template =
        (PFN_vkUpdateDescriptorSetWithTemplate) get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSetWithTemplate");
    if (!real_update_template) {
        real_update_template = (PFN_vkUpdateDescriptorSetWithTemplate)
            get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSetWithTemplateKHR");
    }

    if (real_update_template) {
        real_update_template(device, descSet, descriptorUpdateTemplate, pData);
    }

    PFN_vkCmdBindDescriptorSets real_bind = (PFN_vkCmdBindDescriptorSets)
        get_real_proc(get_last_instance(), device, "vkCmdBindDescriptorSets");
    if (real_bind) {
        real_bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, set, 1, &descSet, 0, NULL);
    }

    LOG_OPT_DEBUG("PushDescriptor: emulated vkCmdPushDescriptorSetWithTemplateKHR for cmd %p set %u", commandBuffer, set);
    return true;
}

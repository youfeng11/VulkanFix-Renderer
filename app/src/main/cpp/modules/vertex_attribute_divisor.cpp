#include "vertex_attribute_divisor.h"

VertexAttributeDivisorModule::VertexAttributeDivisorModule() {
    LOGI("Initialized Vulkan Vertex Attribute Divisor emulation module");
}

bool VertexAttributeDivisorModule::is_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_phys_mutex);
    auto it = m_phys_device_native.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_device_native.end()) {
        return it->second;
    }
    return false;
}

void VertexAttributeDivisorModule::set_device_native(VkPhysicalDevice physDev, bool native_support) {
    std::lock_guard<std::mutex> lock(m_phys_mutex);
    m_phys_device_native[(uint64_t)(uintptr_t)physDev] = native_support;
}

void VertexAttributeDivisorModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    bool has_ext = false;
    bool has_khr = false;

    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0) {
            has_ext = true;
        }
        if (strcmp(ext.extensionName, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0) {
            has_khr = true;
        }
    }

    set_device_native(physicalDevice, has_ext && has_khr);

    if (!has_ext) {
        VkExtensionProperties extProps{};
        strncpy(extProps.extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
        extProps.specVersion = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION;
        extensions.push_back(extProps);
        LOGI("Injected extension: %s (v%u)", VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION);
    }

    if (!has_khr) {
        VkExtensionProperties khrProps{};
        strncpy(khrProps.extensionName, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
        khrProps.specVersion = VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION;
        extensions.push_back(khrProps);
        LOGI("Injected extension: %s (v%u)", VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION);
    }
}

void VertexAttributeDivisorModule::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_device_native(physicalDevice) || !pFeatures) return;

    void** curr = &pFeatures->pNext;
    while (*curr != NULL) {
        VkBaseOutStructure* header = (VkBaseOutStructure*) *curr;
        if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT ||
            header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR) {
            pUserData = header;
            *curr = header->pNext;
            break;
        }
        curr = (void**) &header->pNext;
    }
}

void VertexAttributeDivisorModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pUserData || !pFeatures) return;

    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT* divisor_ext =
        (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT*) pUserData;
    divisor_ext->pNext = pFeatures->pNext;
    pFeatures->pNext = divisor_ext;

    divisor_ext->vertexAttributeInstanceRateDivisor = VK_TRUE;
    divisor_ext->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
    LOG_OPT_DEBUG("Supplied vertexAttributeInstanceRateDivisor = VK_TRUE");
}

void VertexAttributeDivisorModule::on_pre_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_device_native(physicalDevice) || !pProperties) return;

    void** curr = &pProperties->pNext;
    while (*curr != NULL) {
        VkBaseOutStructure* header = (VkBaseOutStructure*) *curr;
        if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT ||
            header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR) {
            pUserData = header;
            *curr = header->pNext;
            break;
        }
        curr = (void**) &header->pNext;
    }
}

void VertexAttributeDivisorModule::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pUserData || !pProperties) return;

    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT* divisor_props =
        (VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT*) pUserData;
    divisor_props->pNext = pProperties->pNext;
    pProperties->pNext = divisor_props;

    divisor_props->maxVertexAttribDivisor = UINT32_MAX;
    LOG_OPT_DEBUG("Supplied maxVertexAttribDivisor = UINT32_MAX");
}

void VertexAttributeDivisorModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_device_native(physicalDevice) || !pCreateInfo) return;

    // 1. Filter out divisor extensions from enabledExtensions
    for (auto it = enabledExtensions.begin(); it != enabledExtensions.end(); ) {
        if (strcmp(*it, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0 ||
            strcmp(*it, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0) {
            LOGI("vkCreateDevice: safely filtered out requested extension '%s'", *it);
            it = enabledExtensions.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Unlink divisor features struct from pCreateInfo->pNext
    void** curr = (void**) &pCreateInfo->pNext;
    while (*curr != NULL) {
        VkBaseOutStructure* header = (VkBaseOutStructure*) *curr;
        if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT ||
            header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR) {
            pUserData = header;
            *curr = header->pNext;
            LOGI("vkCreateDevice: safely unlinked divisor features struct from pNext");
            break;
        }
        curr = (void**) &header->pNext;
    }
}

void VertexAttributeDivisorModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    // No-op
}

bool VertexAttributeDivisorModule::needs_pipeline_interception(
    VkDevice device,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos
) {
    if (!pCreateInfos || createInfoCount == 0) return false;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        const VkPipelineVertexInputStateCreateInfo* vi = pCreateInfos[i].pVertexInputState;
        if (vi != NULL && vi->pNext != NULL) {
            const VkBaseInStructure* node = (const VkBaseInStructure*) vi->pNext;
            while (node != NULL) {
                if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT ||
                    node->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR) {
                    return true;
                }
                node = node->pNext;
            }
        }
    }

    return false;
}

void VertexAttributeDivisorModule::on_modify_pipeline_create_info(
    VkDevice device,
    uint32_t index,
    VkGraphicsPipelineCreateInfo& createInfo,
    VkPipelineVertexInputStateCreateInfo& viState,
    std::vector<void*>& allocationsToFree
) {
    if (viState.pNext == NULL) return;

    bool has_divisor_state = false;
    bool has_zero_divisor = false;
    const VkPipelineVertexInputDivisorStateCreateInfoEXT* divState = NULL;

    const VkBaseInStructure* node = (const VkBaseInStructure*) viState.pNext;
    while (node != NULL) {
        if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT ||
            node->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR) {
            has_divisor_state = true;
            divState = (const VkPipelineVertexInputDivisorStateCreateInfoEXT*) node;
            for (uint32_t d = 0; d < divState->vertexBindingDivisorCount; d++) {
                uint32_t b = divState->pVertexBindingDivisors[d].binding;
                uint32_t div = divState->pVertexBindingDivisors[d].divisor;
                if (div == 0) {
                    has_zero_divisor = true;
                } else if (div != 1) {
                    LOGW("Pipeline %u: binding %u uses divisor %u (device lacks hardware support, falling back to 1)", index, b, div);
                }
            }
            break;
        }
        node = node->pNext;
    }

    if (!has_divisor_state) return;

    // Emulate divisor = 0: set stride = 0 so GPU reads constant element across all instances
    if (has_zero_divisor && viState.vertexBindingDescriptionCount > 0 && viState.pVertexBindingDescriptions != NULL) {
        VkVertexInputBindingDescription* modBindings = (VkVertexInputBindingDescription*)
            malloc(sizeof(VkVertexInputBindingDescription) * viState.vertexBindingDescriptionCount);
        if (modBindings) {
            memcpy(modBindings, viState.pVertexBindingDescriptions, sizeof(VkVertexInputBindingDescription) * viState.vertexBindingDescriptionCount);
            for (uint32_t d = 0; d < divState->vertexBindingDivisorCount; d++) {
                if (divState->pVertexBindingDivisors[d].divisor == 0) {
                    uint32_t target_b = divState->pVertexBindingDivisors[d].binding;
                    for (uint32_t b = 0; b < viState.vertexBindingDescriptionCount; b++) {
                        if (modBindings[b].binding == target_b) {
                            modBindings[b].stride = 0;
                            LOG_OPT_DEBUG("Pipeline %u: binding %u emulated divisor=0 by setting stride=0", index, target_b);
                            break;
                        }
                    }
                }
            }
            viState.pVertexBindingDescriptions = modBindings;
            allocationsToFree.push_back(modBindings);
        }
    }

    // Strip divisor state struct from pNext
    void** curr = (void**) &viState.pNext;
    while (*curr != NULL) {
        VkBaseOutStructure* h = (VkBaseOutStructure*) *curr;
        if (h->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT ||
            h->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR) {
            *curr = (void*) h->pNext;
            LOG_OPT_DEBUG("Pipeline %u: stripped divisor state create info from pVertexInputState->pNext", index);
            break;
        }
        curr = (void**) &h->pNext;
    }

    createInfo.pVertexInputState = &viState;
}

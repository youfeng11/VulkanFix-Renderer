#include "vulkan_1_1.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include <cstring>
#include <algorithm>
#include <inttypes.h>

REGISTER_LAYER_MODULE(Vulkan11Module);

static const char* const s_vulkan_1_1_extensions[] = {
    VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME,
    VK_KHR_DEVICE_GROUP_EXTENSION_NAME,
    VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    VK_KHR_MAINTENANCE2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE3_EXTENSION_NAME,
    VK_KHR_MULTIVIEW_EXTENSION_NAME,
    VK_KHR_RELAXED_BLOCK_LAYOUT_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME,
    VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
    VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
};

Vulkan11Module::Vulkan11Module() {
    LOGI("Vulkan11Module initialized");
}

bool Vulkan11Module::is_phys_device_native(VkPhysicalDevice physDev) {
    if (!physDev) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    VkPhysicalDeviceProperties props{};
    PFN_vkGetPhysicalDeviceProperties real_fn =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_fn) {
        real_fn(physDev, &props);
    }

    bool native = (props.apiVersion >= VK_API_VERSION_1_1);
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    LOGI("Vulkan11Module: PhysicalDevice %p native Vulkan 1.1 support: %d (api: 0x%x)",
         physDev, native, props.apiVersion);
    return native;
}

bool Vulkan11Module::is_device_native(VkDevice device) {
    if (!device) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_needs_emulation.find((uint64_t)(uintptr_t)device);
    if (it != m_device_needs_emulation.end()) {
        return !it->second;
    }
    return false;
}

void Vulkan11Module::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    if (is_phys_device_native(physicalDevice)) return;

    for (const char* extName : s_vulkan_1_1_extensions) {
        if (!vku::has_extension(extensions, extName)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, extName, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = 1;
            extensions.push_back(prop);
            LOG_OPT_DEBUG("Vulkan11: injected extension %s", extName);
        }
    }
}

void Vulkan11Module::on_get_properties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties
) {
    if (!pProperties) return;
    if (pProperties->apiVersion < VK_API_VERSION_1_1) {
        pProperties->apiVersion = VK_API_VERSION_1_1;
        LOG_OPT_DEBUG("Vulkan11: promoted device apiVersion to 1.1 in vkGetPhysicalDeviceProperties");
    }
}

struct Properties11UnlinkData {
    void* v11_props = nullptr;
    void* id_props = nullptr;
    void* subgroup_props = nullptr;
    void* point_clip_props = nullptr;
    void* multiview_props = nullptr;
    void* protected_props = nullptr;
    void* maintenance3_props = nullptr;
};

void Vulkan11Module::on_pre_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pProperties) return;

    auto* unlinks = new Properties11UnlinkData();
    unlinks->v11_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES);
    unlinks->id_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
    unlinks->subgroup_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES);
    unlinks->point_clip_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES);
    unlinks->multiview_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES);
    unlinks->protected_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES);
    unlinks->maintenance3_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES);
    pUserData = unlinks;
}

void Vulkan11Module::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties) return;
    if (pProperties->properties.apiVersion < VK_API_VERSION_1_1) {
        pProperties->properties.apiVersion = VK_API_VERSION_1_1;
    }

    if (pUserData) {
        auto* unlinks = reinterpret_cast<Properties11UnlinkData*>(pUserData);

        if (unlinks->v11_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceVulkan11Properties*>(unlinks->v11_props);
            memset(p->deviceUUID, 0x11, sizeof(p->deviceUUID));
            memset(p->driverUUID, 0x11, sizeof(p->driverUUID));
            memset(p->deviceLUID, 0, sizeof(p->deviceLUID));
            p->deviceNodeMask = 0;
            p->deviceLUIDValid = VK_FALSE;
            p->subgroupSize = 32;
            p->subgroupSupportedStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
            p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
            p->subgroupQuadOperationsInAllStages = VK_FALSE;
            p->pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
            p->maxMultiviewViewCount = 6;
            p->maxMultiviewInstanceIndex = 134217727;
            p->protectedNoFault = VK_FALSE;
            p->maxPerSetDescriptors = 1024;
            p->maxMemoryAllocationSize = 0x80000000ULL;
            vku::relink_pnext(pProperties->pNext, unlinks->v11_props);
        }

        if (unlinks->id_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceIDProperties*>(unlinks->id_props);
            memset(p->deviceUUID, 0x11, sizeof(p->deviceUUID));
            memset(p->driverUUID, 0x11, sizeof(p->driverUUID));
            memset(p->deviceLUID, 0, sizeof(p->deviceLUID));
            p->deviceNodeMask = 0;
            p->deviceLUIDValid = VK_FALSE;
            vku::relink_pnext(pProperties->pNext, unlinks->id_props);
        }

        if (unlinks->subgroup_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(unlinks->subgroup_props);
            p->subgroupSize = 32;
            p->supportedStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
            p->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
            p->quadOperationsInAllStages = VK_FALSE;
            vku::relink_pnext(pProperties->pNext, unlinks->subgroup_props);
        }

        if (unlinks->point_clip_props) {
            auto* p = reinterpret_cast<VkPhysicalDevicePointClippingProperties*>(unlinks->point_clip_props);
            p->pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
            vku::relink_pnext(pProperties->pNext, unlinks->point_clip_props);
        }

        if (unlinks->multiview_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceMultiviewProperties*>(unlinks->multiview_props);
            p->maxMultiviewViewCount = 6;
            p->maxMultiviewInstanceIndex = 134217727;
            vku::relink_pnext(pProperties->pNext, unlinks->multiview_props);
        }

        if (unlinks->protected_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceProtectedMemoryProperties*>(unlinks->protected_props);
            p->protectedNoFault = VK_FALSE;
            vku::relink_pnext(pProperties->pNext, unlinks->protected_props);
        }

        if (unlinks->maintenance3_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceMaintenance3Properties*>(unlinks->maintenance3_props);
            p->maxPerSetDescriptors = 1024;
            p->maxMemoryAllocationSize = 0x80000000ULL;
            vku::relink_pnext(pProperties->pNext, unlinks->maintenance3_props);
        }

        delete unlinks;
    }
}

void Vulkan11Module::on_get_features(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures
) {
    if (!pFeatures || is_phys_device_native(physicalDevice)) return;
    pFeatures->shaderClipDistance = VK_TRUE;
    pFeatures->shaderCullDistance = VK_TRUE;
}

struct Features11UnlinkData {
    void* v11_features = nullptr;
    void* storage_16bit_features = nullptr;
    void* multiview_features = nullptr;
    void* var_ptrs_features = nullptr;
    void* protected_features = nullptr;
    void* sampler_ycbcr_features = nullptr;
    void* shader_draw_params_features = nullptr;
    void* device_group_info = nullptr;
};

void Vulkan11Module::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    auto* unlinks = new Features11UnlinkData();
    unlinks->v11_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    unlinks->storage_16bit_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES);
    unlinks->multiview_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES);
    unlinks->var_ptrs_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES);
    unlinks->protected_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES);
    unlinks->sampler_ycbcr_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES);
    unlinks->shader_draw_params_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES);
    pUserData = unlinks;
}

void Vulkan11Module::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures || !pUserData) return;
    auto* unlinks = reinterpret_cast<Features11UnlinkData*>(pUserData);

    if (unlinks->v11_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(unlinks->v11_features);
        f->storageBuffer16BitAccess = VK_FALSE;
        f->uniformAndStorageBuffer16BitAccess = VK_FALSE;
        f->storagePushConstant16 = VK_FALSE;
        f->storageInputOutput16 = VK_FALSE;
        f->multiview = VK_TRUE;
        f->multiviewGeometryShader = VK_FALSE;
        f->multiviewTessellationShader = VK_FALSE;
        f->variablePointersStorageBuffer = VK_TRUE;
        f->variablePointers = VK_FALSE;
        f->protectedMemory = VK_FALSE;
        f->samplerYcbcrConversion = VK_FALSE;
        f->shaderDrawParameters = VK_TRUE;
        vku::relink_pnext(pFeatures->pNext, unlinks->v11_features);
    }

    if (unlinks->storage_16bit_features) {
        auto* f = reinterpret_cast<VkPhysicalDevice16BitStorageFeatures*>(unlinks->storage_16bit_features);
        f->storageBuffer16BitAccess = VK_FALSE;
        f->uniformAndStorageBuffer16BitAccess = VK_FALSE;
        f->storagePushConstant16 = VK_FALSE;
        f->storageInputOutput16 = VK_FALSE;
        vku::relink_pnext(pFeatures->pNext, unlinks->storage_16bit_features);
    }

    if (unlinks->multiview_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceMultiviewFeatures*>(unlinks->multiview_features);
        f->multiview = VK_TRUE;
        f->multiviewGeometryShader = VK_FALSE;
        f->multiviewTessellationShader = VK_FALSE;
        vku::relink_pnext(pFeatures->pNext, unlinks->multiview_features);
    }

    if (unlinks->var_ptrs_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceVariablePointersFeatures*>(unlinks->var_ptrs_features);
        f->variablePointersStorageBuffer = VK_TRUE;
        f->variablePointers = VK_FALSE;
        vku::relink_pnext(pFeatures->pNext, unlinks->var_ptrs_features);
    }

    if (unlinks->protected_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceProtectedMemoryFeatures*>(unlinks->protected_features);
        f->protectedMemory = VK_FALSE;
        vku::relink_pnext(pFeatures->pNext, unlinks->protected_features);
    }

    if (unlinks->sampler_ycbcr_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(unlinks->sampler_ycbcr_features);
        f->samplerYcbcrConversion = VK_FALSE;
        vku::relink_pnext(pFeatures->pNext, unlinks->sampler_ycbcr_features);
    }

    if (unlinks->shader_draw_params_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceShaderDrawParametersFeatures*>(unlinks->shader_draw_params_features);
        f->shaderDrawParameters = VK_TRUE;
        vku::relink_pnext(pFeatures->pNext, unlinks->shader_draw_params_features);
    }

    delete unlinks;
}

void Vulkan11Module::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // Filter out extensions not natively supported by the Vulkan 1.0 driver
    std::vector<VkExtensionProperties> nativeExts;
    PFN_vkEnumerateDeviceExtensionProperties real_enum =
        (PFN_vkEnumerateDeviceExtensionProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    if (real_enum) {
        uint32_t count = 0;
        real_enum(physicalDevice, nullptr, &count, nullptr);
        if (count > 0) {
            nativeExts.resize(count);
            real_enum(physicalDevice, nullptr, &count, nativeExts.data());
        }
    }

    for (auto it = enabledExtensions.begin(); it != enabledExtensions.end();) {
        const char* name = *it;
        bool isPromoted11 = false;
        for (const char* ext : s_vulkan_1_1_extensions) {
            if (strcmp(name, ext) == 0) {
                isPromoted11 = true;
                break;
            }
        }
        if (isPromoted11 && !vku::has_extension(nativeExts, name)) {
            LOGI("Vulkan11: stripped emulated extension %s from vkCreateDevice", name);
            it = enabledExtensions.erase(it);
        } else {
            ++it;
        }
    }

    auto* unlinks = new Features11UnlinkData();
    unlinks->v11_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    unlinks->storage_16bit_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES);
    unlinks->multiview_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES);
    unlinks->var_ptrs_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES);
    unlinks->protected_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES);
    unlinks->sampler_ycbcr_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES);
    unlinks->shader_draw_params_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES);
    unlinks->device_group_info = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO);
    pUserData = unlinks;
}

void Vulkan11Module::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_needs_emulation[(uint64_t)(uintptr_t)device] = !native;
        LOGI("Device %p created: Vulkan 1.1 native=%d", device, native);
    }

    if (pUserData) {
        delete reinterpret_cast<Features11UnlinkData*>(pUserData);
    }
}

void Vulkan11Module::on_destroy_device(VkDevice device) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_needs_emulation.erase((uint64_t)(uintptr_t)device);
    }
    std::lock_guard<std::mutex> lock(m_template_mutex);
    m_templates.clear();
}

// ============================================================================
// Memory & Binding 2
// ============================================================================

bool Vulkan11Module::on_bind_buffer_memory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindBufferMemoryInfo* pBindInfos,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    PFN_vkBindBufferMemory2 real_fn =
        (PFN_vkBindBufferMemory2) get_real_proc(get_last_instance(), device, "vkBindBufferMemory2");
    if (!real_fn) {
        real_fn = (PFN_vkBindBufferMemory2) get_real_proc(get_last_instance(), device, "vkBindBufferMemory2KHR");
    }
    if (real_fn) {
        outResult = real_fn(device, bindInfoCount, pBindInfos);
        return true;
    }

    if (bindInfoCount == 0 || !pBindInfos) {
        outResult = VK_SUCCESS;
        return true;
    }

    PFN_vkBindBufferMemory real_bind =
        (PFN_vkBindBufferMemory) get_real_proc(get_last_instance(), device, "vkBindBufferMemory");
    if (!real_bind) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    outResult = VK_SUCCESS;
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        VkResult r = real_bind(device, pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) {
            outResult = r;
            return true;
        }
    }
    return true;
}

bool Vulkan11Module::on_bind_image_memory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindImageMemoryInfo* pBindInfos,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    PFN_vkBindImageMemory2 real_fn =
        (PFN_vkBindImageMemory2) get_real_proc(get_last_instance(), device, "vkBindImageMemory2");
    if (!real_fn) {
        real_fn = (PFN_vkBindImageMemory2) get_real_proc(get_last_instance(), device, "vkBindImageMemory2KHR");
    }
    if (real_fn) {
        outResult = real_fn(device, bindInfoCount, pBindInfos);
        return true;
    }

    if (bindInfoCount == 0 || !pBindInfos) {
        outResult = VK_SUCCESS;
        return true;
    }

    PFN_vkBindImageMemory real_bind =
        (PFN_vkBindImageMemory) get_real_proc(get_last_instance(), device, "vkBindImageMemory");
    if (!real_bind) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    outResult = VK_SUCCESS;
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        VkResult r = real_bind(device, pBindInfos[i].image, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) {
            outResult = r;
            return true;
        }
    }
    return true;
}

bool Vulkan11Module::on_get_buffer_memory_requirements2(
    VkDevice device,
    const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    if (is_device_native(device)) return false;

    PFN_vkGetBufferMemoryRequirements2 real_fn =
        (PFN_vkGetBufferMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements2");
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements2KHR");
    }
    if (real_fn) {
        real_fn(device, pInfo, pMemoryRequirements);
        return true;
    }

    if (!pInfo || !pMemoryRequirements) return true;

    PFN_vkGetBufferMemoryRequirements real_gmr =
        (PFN_vkGetBufferMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements");
    if (real_gmr) {
        real_gmr(device, pInfo->buffer, &pMemoryRequirements->memoryRequirements);
    }

    auto* dedicated = vku::find_pnext_mut<VkMemoryDedicatedRequirements>(
        pMemoryRequirements->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
    if (dedicated) {
        dedicated->prefersDedicatedAllocation = VK_FALSE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
    }
    return true;
}

bool Vulkan11Module::on_get_image_memory_requirements2(
    VkDevice device,
    const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    if (is_device_native(device)) return false;

    PFN_vkGetImageMemoryRequirements2 real_fn =
        (PFN_vkGetImageMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements2");
    if (!real_fn) {
        real_fn = (PFN_vkGetImageMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements2KHR");
    }
    if (real_fn) {
        real_fn(device, pInfo, pMemoryRequirements);
        return true;
    }

    if (!pInfo || !pMemoryRequirements) return true;

    PFN_vkGetImageMemoryRequirements real_gmr =
        (PFN_vkGetImageMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements");
    if (real_gmr) {
        real_gmr(device, pInfo->image, &pMemoryRequirements->memoryRequirements);
    }

    auto* dedicated = vku::find_pnext_mut<VkMemoryDedicatedRequirements>(
        pMemoryRequirements->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
    if (dedicated) {
        dedicated->prefersDedicatedAllocation = VK_FALSE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
    }
    return true;
}

bool Vulkan11Module::on_get_image_sparse_memory_requirements2(
    VkDevice device,
    const VkImageSparseMemoryRequirementsInfo2* pInfo,
    uint32_t* pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2* pSparseMemoryRequirements
) {
    if (is_device_native(device)) return false;

    PFN_vkGetImageSparseMemoryRequirements2 real_fn =
        (PFN_vkGetImageSparseMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageSparseMemoryRequirements2");
    if (!real_fn) {
        real_fn = (PFN_vkGetImageSparseMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageSparseMemoryRequirements2KHR");
    }
    if (real_fn) {
        real_fn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
        return true;
    }

    if (!pInfo || !pSparseMemoryRequirementCount) return true;

    PFN_vkGetImageSparseMemoryRequirements real_smr =
        (PFN_vkGetImageSparseMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetImageSparseMemoryRequirements");
    if (real_smr) {
        if (!pSparseMemoryRequirements) {
            real_smr(device, pInfo->image, pSparseMemoryRequirementCount, nullptr);
        } else {
            uint32_t count = *pSparseMemoryRequirementCount;
            std::vector<VkSparseImageMemoryRequirements> nativeReqs(count);
            real_smr(device, pInfo->image, &count, nativeReqs.data());
            for (uint32_t i = 0; i < count; ++i) {
                pSparseMemoryRequirements[i].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2;
                pSparseMemoryRequirements[i].pNext = nullptr;
                pSparseMemoryRequirements[i].memoryRequirements = nativeReqs[i];
            }
            *pSparseMemoryRequirementCount = count;
        }
        return true;
    }

    *pSparseMemoryRequirementCount = 0;
    return true;
}

// ============================================================================
// Descriptor Update Template
// ============================================================================

bool Vulkan11Module::on_create_descriptor_update_template(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    PFN_vkCreateDescriptorUpdateTemplate real_fn =
        (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplateKHR");
    }
    if (real_fn) {
        outResult = real_fn(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
        return true;
    }

    if (!pCreateInfo || !pDescriptorUpdateTemplate) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    auto tmpl = std::make_shared<EmulatedTemplate>();
    tmpl->templateType = pCreateInfo->templateType;
    tmpl->pipelineLayout = pCreateInfo->pipelineLayout;
    tmpl->set = pCreateInfo->set;
    if (pCreateInfo->pDescriptorUpdateEntries && pCreateInfo->descriptorUpdateEntryCount > 0) {
        tmpl->entries.assign(
            pCreateInfo->pDescriptorUpdateEntries,
            pCreateInfo->pDescriptorUpdateEntries + pCreateInfo->descriptorUpdateEntryCount
        );
    }

    static std::atomic<uint64_t> s_next_template_id{0x11000000ULL};
    uint64_t handleVal = ++s_next_template_id;
    VkDescriptorUpdateTemplate handle = (VkDescriptorUpdateTemplate)(uintptr_t)handleVal;

    std::lock_guard<std::mutex> lock(m_template_mutex);
    m_templates[handleVal] = tmpl;

    *pDescriptorUpdateTemplate = handle;
    outResult = VK_SUCCESS;
    return true;
}

bool Vulkan11Module::on_destroy_descriptor_update_template(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks* pAllocator
) {
    if (is_device_native(device)) return false;

    PFN_vkDestroyDescriptorUpdateTemplate real_fn =
        (PFN_vkDestroyDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorUpdateTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkDestroyDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorUpdateTemplateKHR");
    }
    if (real_fn) {
        real_fn(device, descriptorUpdateTemplate, pAllocator);
        return true;
    }

    std::lock_guard<std::mutex> lock(m_template_mutex);
    m_templates.erase((uint64_t)(uintptr_t)descriptorUpdateTemplate);
    return true;
}

bool Vulkan11Module::on_update_descriptor_set_with_template(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const void* pData
) {
    if (is_device_native(device)) return false;

    PFN_vkUpdateDescriptorSetWithTemplate real_fn =
        (PFN_vkUpdateDescriptorSetWithTemplate) get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSetWithTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkUpdateDescriptorSetWithTemplate) get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSetWithTemplateKHR");
    }
    if (real_fn) {
        real_fn(device, descriptorSet, descriptorUpdateTemplate, pData);
        return true;
    }

    std::shared_ptr<EmulatedTemplate> tmpl;
    {
        std::lock_guard<std::mutex> lock(m_template_mutex);
        auto it = m_templates.find((uint64_t)(uintptr_t)descriptorUpdateTemplate);
        if (it != m_templates.end()) {
            tmpl = it->second;
        }
    }

    if (!tmpl || !pData) return true;

    PFN_vkUpdateDescriptorSets real_update =
        (PFN_vkUpdateDescriptorSets) get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSets");
    if (!real_update) return true;

    for (const auto& entry : tmpl->entries) {
        for (uint32_t i = 0; i < entry.descriptorCount; ++i) {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.dstBinding = entry.dstBinding;
            write.dstArrayElement = entry.dstArrayElement + i;
            write.descriptorCount = 1;
            write.descriptorType = entry.descriptorType;

            const char* entryPtr = ((const char*) pData) + entry.offset + i * entry.stride;

            switch (entry.descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    write.pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(entryPtr);
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    write.pBufferInfo = reinterpret_cast<const VkDescriptorBufferInfo*>(entryPtr);
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    write.pTexelBufferView = reinterpret_cast<const VkBufferView*>(entryPtr);
                    break;
                default:
                    break;
            }
            real_update(device, 1, &write, 0, nullptr);
        }
    }
    return true;
}

// ============================================================================
// Maintenance 3
// ============================================================================

bool Vulkan11Module::on_get_descriptor_set_layout_support(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    VkDescriptorSetLayoutSupport* pSupport
) {
    if (is_device_native(device)) return false;

    PFN_vkGetDescriptorSetLayoutSupport real_fn =
        (PFN_vkGetDescriptorSetLayoutSupport) get_real_proc(get_last_instance(), device, "vkGetDescriptorSetLayoutSupport");
    if (!real_fn) {
        real_fn = (PFN_vkGetDescriptorSetLayoutSupport) get_real_proc(get_last_instance(), device, "vkGetDescriptorSetLayoutSupportKHR");
    }
    if (real_fn) {
        real_fn(device, pCreateInfo, pSupport);
        return true;
    }

    if (pSupport) {
        pSupport->supported = VK_TRUE;
        auto* varSupport = vku::find_pnext_mut<VkDescriptorSetVariableDescriptorCountLayoutSupport>(
            pSupport->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT);
        if (varSupport) {
            varSupport->maxVariableDescriptorCount = 1024;
        }
    }
    return true;
}

void Vulkan11Module::on_trim_command_pool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolTrimFlags flags
) {
    if (is_device_native(device)) return;

    PFN_vkTrimCommandPool real_fn =
        (PFN_vkTrimCommandPool) get_real_proc(get_last_instance(), device, "vkTrimCommandPool");
    if (!real_fn) {
        real_fn = (PFN_vkTrimCommandPool) get_real_proc(get_last_instance(), device, "vkTrimCommandPoolKHR");
    }
    if (real_fn) {
        real_fn(device, commandPool, flags);
    }
}

// ============================================================================
// Device Groups / Dispatch Base
// ============================================================================

void Vulkan11Module::on_cmd_set_device_mask(
    VkCommandBuffer commandBuffer,
    uint32_t deviceMask
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return;

    PFN_vkCmdSetDeviceMask real_fn =
        (PFN_vkCmdSetDeviceMask) get_real_proc(get_last_instance(), device, "vkCmdSetDeviceMask");
    if (!real_fn) {
        real_fn = (PFN_vkCmdSetDeviceMask) get_real_proc(get_last_instance(), device, "vkCmdSetDeviceMaskKHR");
    }
    if (real_fn) {
        real_fn(commandBuffer, deviceMask);
    }
}

bool Vulkan11Module::on_get_device_group_peer_memory_features(
    VkDevice device,
    uint32_t heapIndex,
    uint32_t localDeviceIndex,
    uint32_t remoteDeviceIndex,
    VkPeerMemoryFeatureFlags* pPeerMemoryFeatures
) {
    if (is_device_native(device)) return false;

    PFN_vkGetDeviceGroupPeerMemoryFeatures real_fn =
        (PFN_vkGetDeviceGroupPeerMemoryFeatures) get_real_proc(get_last_instance(), device, "vkGetDeviceGroupPeerMemoryFeatures");
    if (!real_fn) {
        real_fn = (PFN_vkGetDeviceGroupPeerMemoryFeatures) get_real_proc(get_last_instance(), device, "vkGetDeviceGroupPeerMemoryFeaturesKHR");
    }
    if (real_fn) {
        real_fn(device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
        return true;
    }

    if (pPeerMemoryFeatures) {
        *pPeerMemoryFeatures = 0;
    }
    return true;
}

bool Vulkan11Module::on_cmd_dispatch_base(
    VkCommandBuffer commandBuffer,
    uint32_t baseGroupX,
    uint32_t baseGroupY,
    uint32_t baseGroupZ,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    PFN_vkCmdDispatchBase real_fn =
        (PFN_vkCmdDispatchBase) get_real_proc(get_last_instance(), device, "vkCmdDispatchBase");
    if (!real_fn) {
        real_fn = (PFN_vkCmdDispatchBase) get_real_proc(get_last_instance(), device, "vkCmdDispatchBaseKHR");
    }
    if (real_fn) {
        real_fn(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
        return true;
    }

    if (baseGroupX == 0 && baseGroupY == 0 && baseGroupZ == 0) {
        PFN_vkCmdDispatch real_disp =
            (PFN_vkCmdDispatch) get_real_proc(get_last_instance(), device, "vkCmdDispatch");
        if (real_disp) {
            real_disp(commandBuffer, groupCountX, groupCountY, groupCountZ);
        }
        return true;
    }
    return false;
}

bool Vulkan11Module::on_enumerate_physical_device_groups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties,
    VkResult& outResult
) {
    VkInstance inst = (instance != VK_NULL_HANDLE) ? instance : get_last_instance();
    PFN_vkEnumeratePhysicalDeviceGroups real_fn =
        (PFN_vkEnumeratePhysicalDeviceGroups) get_real_proc(inst, VK_NULL_HANDLE, "vkEnumeratePhysicalDeviceGroups");
    if (!real_fn) {
        real_fn = (PFN_vkEnumeratePhysicalDeviceGroups) get_real_proc(inst, VK_NULL_HANDLE, "vkEnumeratePhysicalDeviceGroupsKHR");
    }
    if (real_fn) {
        outResult = real_fn(inst, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
        return true;
    }

    PFN_vkEnumeratePhysicalDevices real_epd =
        (PFN_vkEnumeratePhysicalDevices) get_real_proc(inst, VK_NULL_HANDLE, "vkEnumeratePhysicalDevices");
    if (!real_epd) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    uint32_t physCount = 0;
    real_epd(inst, &physCount, nullptr);
    if (!pPhysicalDeviceGroupProperties) {
        *pPhysicalDeviceGroupCount = physCount;
        outResult = VK_SUCCESS;
        return true;
    }

    uint32_t toFill = std::min(*pPhysicalDeviceGroupCount, physCount);
    std::vector<VkPhysicalDevice> phys(physCount);
    real_epd(inst, &physCount, phys.data());

    for (uint32_t i = 0; i < toFill; ++i) {
        auto& group = pPhysicalDeviceGroupProperties[i];
        group.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
        group.pNext = nullptr;
        group.physicalDeviceCount = 1;
        group.physicalDevices[0] = phys[i];
        for (uint32_t j = 1; j < VK_MAX_DEVICE_GROUP_SIZE; ++j) {
            group.physicalDevices[j] = VK_NULL_HANDLE;
        }
        group.subsetAllocation = VK_FALSE;
    }
    *pPhysicalDeviceGroupCount = toFill;
    outResult = (toFill < physCount) ? VK_INCOMPLETE : VK_SUCCESS;
    return true;
}

#include "vulkan_1_1_core.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include <cstring>
#include <algorithm>
#include <inttypes.h>

REGISTER_LAYER_MODULE(Vulkan11CoreModule);

static const char* const s_vulkan_1_1_core_extensions[] = {
    VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
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

Vulkan11CoreModule::Vulkan11CoreModule() {
    LOGI("Vulkan11CoreModule initialized");
}

bool Vulkan11CoreModule::is_phys_device_native(VkPhysicalDevice physDev) {
    if (!physDev) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    uint32_t realApiVer = VK_API_VERSION_1_0;
    PFN_vkGetPhysicalDeviceProperties real_fn =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_fn) {
        VkPhysicalDeviceProperties props{};
        real_fn(physDev, &props);
        realApiVer = props.apiVersion;
    }
    m_phys_real_api_version[(uint64_t)(uintptr_t)physDev] = realApiVer;

    bool native = (realApiVer >= VK_API_VERSION_1_1);
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    LOGI("Vulkan11Core: PhysicalDevice %p native Vulkan 1.1 support: %d (api: 0x%x)",
         physDev, native ? 1 : 0, realApiVer);
    return native;
}

uint32_t Vulkan11CoreModule::get_phys_real_api_version(VkPhysicalDevice physDev) {
    if (!physDev) return VK_API_VERSION_1_0;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_real_api_version.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_real_api_version.end()) {
        return it->second;
    }
    PFN_vkGetPhysicalDeviceProperties real_props =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_props) {
        VkPhysicalDeviceProperties props{};
        real_props(physDev, &props);
        m_phys_real_api_version[(uint64_t)(uintptr_t)physDev] = props.apiVersion;
        return props.apiVersion;
    }
    return VK_API_VERSION_1_0;
}

bool Vulkan11CoreModule::is_device_native(VkDevice device) {
    if (!device) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_needs_emulation.find((uint64_t)(uintptr_t)device);
    if (it != m_device_needs_emulation.end()) {
        return !it->second;
    }
    return false;
}

void Vulkan11CoreModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    if (is_phys_device_native(physicalDevice)) return;

    for (const char* extName : s_vulkan_1_1_core_extensions) {
        if (!vku::has_extension(extensions, extName)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, extName, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = 1;
            extensions.push_back(prop);
            LOG_OPT_DEBUG("Vulkan11Core: injected extension %s", extName);
        }
    }
}

void Vulkan11CoreModule::on_get_properties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties
) {
    if (!pProperties) return;
    if (pProperties->apiVersion < VK_API_VERSION_1_1) {
        pProperties->apiVersion = VK_API_VERSION_1_1;
        LOG_OPT_DEBUG("Vulkan11Core: promoted device apiVersion to 1.1 in vkGetPhysicalDeviceProperties");
    }
}

struct Properties11UnlinkData {
    void* v11_props = nullptr;
    void* id_props = nullptr;
    void* subgroup_props = nullptr;
    void* point_clip_props = nullptr;
    void* protected_props = nullptr;
};

void Vulkan11CoreModule::on_pre_get_properties2(
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
    unlinks->protected_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES);
    pUserData = unlinks;
}

void Vulkan11CoreModule::on_post_get_properties2(
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

        if (unlinks->protected_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceProtectedMemoryProperties*>(unlinks->protected_props);
            p->protectedNoFault = VK_FALSE;
            vku::relink_pnext(pProperties->pNext, unlinks->protected_props);
        }

        delete unlinks;
    }
}

void Vulkan11CoreModule::on_get_features(
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
    void* var_ptrs_features = nullptr;
    void* protected_features = nullptr;
    void* sampler_ycbcr_features = nullptr;
    void* shader_draw_params_features = nullptr;
};

void Vulkan11CoreModule::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    auto* unlinks = new Features11UnlinkData();
    unlinks->v11_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    unlinks->storage_16bit_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES);
    unlinks->var_ptrs_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES);
    unlinks->protected_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES);
    unlinks->sampler_ycbcr_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES);
    unlinks->shader_draw_params_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES);
    pUserData = unlinks;
}

void Vulkan11CoreModule::on_post_get_features2(
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

void Vulkan11CoreModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // Filter out core 1.1 extensions if not supported natively
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
        for (const char* ext : s_vulkan_1_1_core_extensions) {
            if (strcmp(name, ext) == 0) {
                isPromoted11 = true;
                break;
            }
        }
        if (isPromoted11 && !vku::has_extension(nativeExts, name)) {
            LOGI("Vulkan11Core: stripped emulated extension %s from vkCreateDevice", name);
            it = enabledExtensions.erase(it);
        } else {
            ++it;
        }
    }

    auto* unlinks = new Features11UnlinkData();
    unlinks->v11_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    unlinks->storage_16bit_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES);
    unlinks->var_ptrs_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES);
    unlinks->protected_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES);
    unlinks->sampler_ycbcr_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES);
    unlinks->shader_draw_params_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES);
    pUserData = unlinks;
}

void Vulkan11CoreModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_needs_emulation[(uint64_t)(uintptr_t)device] = !native;
        LOGI("Device %p created: Vulkan 1.1 core native=%d", device, native);
    }

    if (pUserData) {
        delete reinterpret_cast<Features11UnlinkData*>(pUserData);
    }
}

void Vulkan11CoreModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_needs_emulation.erase((uint64_t)(uintptr_t)device);
}

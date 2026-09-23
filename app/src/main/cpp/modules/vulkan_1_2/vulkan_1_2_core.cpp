#include "vulkan_1_2_core.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>

REGISTER_LAYER_MODULE(Vulkan12CoreModule);

static const char* const s_vulkan_1_2_core_extensions[] = {
    VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME,
    VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME,
    VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
    VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME,
    VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME,
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME,
    VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
    VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
};

static inline VkImageLayout sanitize_layout(VkImageLayout layout) {
    if ((int32_t)layout == 1000241000 /* VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL */ ||
        (int32_t)layout == 1000241002 /* VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL */) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if ((int32_t)layout == 1000241001 /* VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL */ ||
        (int32_t)layout == 1000241003 /* VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL */) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    return layout;
}

Vulkan12CoreModule::Vulkan12CoreModule() {
    LOGI("Vulkan12CoreModule initialized");
}

bool Vulkan12CoreModule::is_phys_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    uint32_t realApiVer = VK_API_VERSION_1_0;
    PFN_vkGetPhysicalDeviceProperties real_props =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_props) {
        VkPhysicalDeviceProperties props{};
        real_props(physDev, &props);
        realApiVer = props.apiVersion;
    }
    m_phys_real_api_version[(uint64_t)(uintptr_t)physDev] = realApiVer;

    const char* force_emu = getenv("FORCE_EMULATE_VULKAN_1_2");
    if (force_emu && (strcmp(force_emu, "1") == 0 || strcasecmp(force_emu, "true") == 0)) {
        LOGI("FORCE_EMULATE_VULKAN_1_2 set, enabling emulation for physical device %p (real api: 0x%x)", physDev, realApiVer);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }

    bool native = (realApiVer >= VK_API_VERSION_1_2);
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native Vulkan 1.2 support (native api: 0x%x), enabling Vulkan 1.2 emulation layer!", physDev, realApiVer);
    } else {
        LOGI("Physical device %p natively supports Vulkan 1.2+ (native api: 0x%x)", physDev, realApiVer);
    }
    return native;
}

uint32_t Vulkan12CoreModule::get_phys_real_api_version(VkPhysicalDevice physDev) {
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

bool Vulkan12CoreModule::is_device_native(VkDevice device) {
    if (!device) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_needs_emulation.find((uint64_t)(uintptr_t)device);
    if (it != m_device_needs_emulation.end()) {
        return !it->second;
    }
    return false;
}

void Vulkan12CoreModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    if (is_phys_device_native(physicalDevice)) return;

    for (const char* extName : s_vulkan_1_2_core_extensions) {
        if (!vku::has_extension(extensions, extName)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, extName, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = 1;
            extensions.push_back(prop);
            LOG_OPT_DEBUG("Vulkan12Core: injected extension %s", extName);
        }
    }
}

void Vulkan12CoreModule::on_get_properties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties
) {
    if (!pProperties) return;
    if (pProperties->apiVersion < VK_API_VERSION_1_2) {
        pProperties->apiVersion = VK_API_VERSION_1_2;
        LOG_OPT_DEBUG("Vulkan12Core: promoted device apiVersion to 1.2 in vkGetPhysicalDeviceProperties");
    }
}

struct Properties12UnlinkData {
    void* v12_props = nullptr;
    void* driver_props = nullptr;
    void* depth_stencil_resolve_props = nullptr;
    void* float_controls_props = nullptr;
    void* sampler_filter_minmax_props = nullptr;
};

void Vulkan12CoreModule::on_pre_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pProperties) return;

    auto* unlinks = new Properties12UnlinkData();
    unlinks->v12_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);
    unlinks->driver_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    unlinks->depth_stencil_resolve_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES);
    unlinks->float_controls_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES);
    unlinks->sampler_filter_minmax_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES);
    pUserData = unlinks;
}

void Vulkan12CoreModule::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties) return;
    if (pProperties->properties.apiVersion < VK_API_VERSION_1_2) {
        pProperties->properties.apiVersion = VK_API_VERSION_1_2;
    }

    char driverInfoStr[VK_MAX_DRIVER_INFO_SIZE];
    uint32_t realApiVer = get_phys_real_api_version(physicalDevice);
    uint32_t realMajor = VK_VERSION_MAJOR(realApiVer);
    uint32_t realMinor = VK_VERSION_MINOR(realApiVer);
    uint32_t realPatch = VK_VERSION_PATCH(realApiVer);

    char realVerStr[32];
    if (realPatch > 0) {
        snprintf(realVerStr, sizeof(realVerStr), "%u.%u.%u", realMajor, realMinor, realPatch);
    } else {
        snprintf(realVerStr, sizeof(realVerStr), "%u.%u", realMajor, realMinor);
    }

    uint32_t emuApiVer = pProperties->properties.apiVersion;
    uint32_t emuMajor = VK_VERSION_MAJOR(emuApiVer);
    uint32_t emuMinor = VK_VERSION_MINOR(emuApiVer);
    char emuVerStr[32];
    if (VK_VERSION_PATCH(emuApiVer) > 0) {
        snprintf(emuVerStr, sizeof(emuVerStr), "%u.%u.%u", emuMajor, emuMinor, VK_VERSION_PATCH(emuApiVer));
    } else {
        snprintf(emuVerStr, sizeof(emuVerStr), "%u.%u", emuMajor, emuMinor);
    }

    bool isNative = is_phys_device_native(physicalDevice);
    if (isNative) {
        snprintf(driverInfoStr, sizeof(driverInfoStr), "%s (%s), Vulkan %s",
                 PROJECT_VERSION_NAME, PROJECT_VERSION_CODE, realVerStr);
    } else {
        snprintf(driverInfoStr, sizeof(driverInfoStr), "%s (%s), Vulkan %s (Vulkan %s)",
                 PROJECT_VERSION_NAME, PROJECT_VERSION_CODE, emuVerStr, realVerStr);
    }

    if (pUserData) {
        auto* unlinks = reinterpret_cast<Properties12UnlinkData*>(pUserData);

        if (unlinks->v12_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceVulkan12Properties*>(unlinks->v12_props);
            p->driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
            strncpy(p->driverName, "Vulkan Fix", VK_MAX_DRIVER_NAME_SIZE - 1);
            p->driverName[VK_MAX_DRIVER_NAME_SIZE - 1] = '\0';
            strncpy(p->driverInfo, driverInfoStr, VK_MAX_DRIVER_INFO_SIZE - 1);
            p->driverInfo[VK_MAX_DRIVER_INFO_SIZE - 1] = '\0';
            p->conformanceVersion = {1, 2, 0, 0};
            p->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            p->supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            p->independentResolveNone = VK_TRUE;
            p->independentResolve = VK_TRUE;
            p->maxTimelineSemaphoreValueDifference = ~0ULL;
            vku::relink_pnext(pProperties->pNext, unlinks->v12_props);
        }

        if (unlinks->driver_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceDriverProperties*>(unlinks->driver_props);
            p->driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
            strncpy(p->driverName, "Vulkan Fix", VK_MAX_DRIVER_NAME_SIZE - 1);
            p->driverName[VK_MAX_DRIVER_NAME_SIZE - 1] = '\0';
            strncpy(p->driverInfo, driverInfoStr, VK_MAX_DRIVER_INFO_SIZE - 1);
            p->driverInfo[VK_MAX_DRIVER_INFO_SIZE - 1] = '\0';
            p->conformanceVersion = {1, 2, 0, 0};
            vku::relink_pnext(pProperties->pNext, unlinks->driver_props);
        }

        if (unlinks->depth_stencil_resolve_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceDepthStencilResolveProperties*>(unlinks->depth_stencil_resolve_props);
            p->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            p->supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            p->independentResolveNone = VK_TRUE;
            p->independentResolve = VK_TRUE;
            vku::relink_pnext(pProperties->pNext, unlinks->depth_stencil_resolve_props);
        }

        if (unlinks->float_controls_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceFloatControlsProperties*>(unlinks->float_controls_props);
            p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->shaderSignedZeroInfNanPreserveFloat16 = VK_TRUE;
            p->shaderSignedZeroInfNanPreserveFloat32 = VK_TRUE;
            p->shaderSignedZeroInfNanPreserveFloat64 = VK_TRUE;
            p->shaderDenormPreserveFloat16 = VK_FALSE;
            p->shaderDenormPreserveFloat32 = VK_FALSE;
            p->shaderDenormPreserveFloat64 = VK_FALSE;
            p->shaderDenormFlushToZeroFloat16 = VK_FALSE;
            p->shaderDenormFlushToZeroFloat32 = VK_FALSE;
            p->shaderDenormFlushToZeroFloat64 = VK_FALSE;
            p->shaderRoundingModeRTEFloat16 = VK_TRUE;
            p->shaderRoundingModeRTEFloat32 = VK_TRUE;
            p->shaderRoundingModeRTEFloat64 = VK_TRUE;
            p->shaderRoundingModeRTZFloat16 = VK_FALSE;
            p->shaderRoundingModeRTZFloat32 = VK_FALSE;
            p->shaderRoundingModeRTZFloat64 = VK_FALSE;
            vku::relink_pnext(pProperties->pNext, unlinks->float_controls_props);
        }

        if (unlinks->sampler_filter_minmax_props) {
            auto* p = reinterpret_cast<VkPhysicalDeviceSamplerFilterMinmaxProperties*>(unlinks->sampler_filter_minmax_props);
            p->filterMinmaxSingleComponentFormats = VK_TRUE;
            p->filterMinmaxImageComponentMapping = VK_TRUE;
            vku::relink_pnext(pProperties->pNext, unlinks->sampler_filter_minmax_props);
        }

        delete unlinks;
    }

    auto* dp_existing = vku::find_pnext_mut<VkPhysicalDeviceDriverProperties>(
        pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    if (dp_existing) {
        strncpy(dp_existing->driverName, "Vulkan Fix", VK_MAX_DRIVER_NAME_SIZE - 1);
        dp_existing->driverName[VK_MAX_DRIVER_NAME_SIZE - 1] = '\0';
        strncpy(dp_existing->driverInfo, driverInfoStr, VK_MAX_DRIVER_INFO_SIZE - 1);
        dp_existing->driverInfo[VK_MAX_DRIVER_INFO_SIZE - 1] = '\0';
    }
}

struct Features12UnlinkData {
    void* v12_features = nullptr;
    void* sep_ds_features = nullptr;
    void* scalar_block_features = nullptr;
    void* ubo_std_layout_features = nullptr;
    void* desc_indexing_features = nullptr;
    void* subgroup_ext_features = nullptr;
};

void Vulkan12CoreModule::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    auto* unlinks = new Features12UnlinkData();
    unlinks->v12_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    unlinks->sep_ds_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES);
    unlinks->scalar_block_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES);
    unlinks->ubo_std_layout_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES);
    unlinks->desc_indexing_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
    unlinks->subgroup_ext_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES);
    pUserData = unlinks;
}

void Vulkan12CoreModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures || !pUserData) return;
    auto* unlinks = reinterpret_cast<Features12UnlinkData*>(pUserData);

    if (unlinks->v12_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(unlinks->v12_features);
        f->samplerMirrorClampToEdge = VK_TRUE;
        f->drawIndirectCount = VK_TRUE;
        f->storageBuffer8BitAccess = VK_FALSE;
        f->uniformAndStorageBuffer8BitAccess = VK_FALSE;
        f->storagePushConstant8 = VK_FALSE;
        f->shaderBufferInt64Atomics = VK_FALSE;
        f->shaderSharedInt64Atomics = VK_FALSE;
        f->shaderFloat16 = VK_FALSE;
        f->shaderInt8 = VK_FALSE;
        f->descriptorIndexing = VK_TRUE;
        f->shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
        f->shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
        f->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
        f->shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
        f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        f->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        f->shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
        f->shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
        f->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
        f->descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        f->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f->descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        f->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        f->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
        f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
        f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        f->descriptorBindingPartiallyBound = VK_TRUE;
        f->descriptorBindingVariableDescriptorCount = VK_TRUE;
        f->runtimeDescriptorArray = VK_TRUE;
        f->samplerFilterMinmax = VK_TRUE;
        f->scalarBlockLayout = VK_TRUE;
        f->imagelessFramebuffer = VK_TRUE;
        f->uniformBufferStandardLayout = VK_TRUE;
        f->shaderSubgroupExtendedTypes = VK_TRUE;
        f->separateDepthStencilLayouts = VK_TRUE;
        f->hostQueryReset = VK_TRUE;
        f->timelineSemaphore = VK_TRUE;
        f->bufferDeviceAddress = VK_TRUE;
        f->bufferDeviceAddressCaptureReplay = VK_FALSE;
        f->bufferDeviceAddressMultiDevice = VK_FALSE;
        f->vulkanMemoryModel = VK_FALSE;
        f->vulkanMemoryModelDeviceScope = VK_FALSE;
        f->vulkanMemoryModelAvailabilityVisibilityChains = VK_FALSE;
        f->shaderOutputViewportIndex = VK_FALSE;
        f->shaderOutputLayer = VK_FALSE;
        f->subgroupBroadcastDynamicId = VK_FALSE;
        vku::relink_pnext(pFeatures->pNext, unlinks->v12_features);
    }

    if (unlinks->sep_ds_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures*>(unlinks->sep_ds_features);
        f->separateDepthStencilLayouts = VK_TRUE;
        vku::relink_pnext(pFeatures->pNext, unlinks->sep_ds_features);
    }

    if (unlinks->scalar_block_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceScalarBlockLayoutFeatures*>(unlinks->scalar_block_features);
        f->scalarBlockLayout = VK_TRUE;
        vku::relink_pnext(pFeatures->pNext, unlinks->scalar_block_features);
    }

    if (unlinks->ubo_std_layout_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceUniformBufferStandardLayoutFeatures*>(unlinks->ubo_std_layout_features);
        f->uniformBufferStandardLayout = VK_TRUE;
        vku::relink_pnext(pFeatures->pNext, unlinks->ubo_std_layout_features);
    }

    if (unlinks->desc_indexing_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(unlinks->desc_indexing_features);
        f->shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
        f->shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
        f->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
        f->shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
        f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        f->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        f->shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
        f->shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
        f->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
        f->descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        f->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f->descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        f->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        f->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
        f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
        f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        f->descriptorBindingPartiallyBound = VK_TRUE;
        f->descriptorBindingVariableDescriptorCount = VK_TRUE;
        f->runtimeDescriptorArray = VK_TRUE;
        vku::relink_pnext(pFeatures->pNext, unlinks->desc_indexing_features);
    }

    if (unlinks->subgroup_ext_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures*>(unlinks->subgroup_ext_features);
        f->shaderSubgroupExtendedTypes = VK_TRUE;
        vku::relink_pnext(pFeatures->pNext, unlinks->subgroup_ext_features);
    }

    delete unlinks;
}

void Vulkan12CoreModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

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
        bool isPromoted12 = false;
        for (const char* ext : s_vulkan_1_2_core_extensions) {
            if (strcmp(name, ext) == 0) {
                isPromoted12 = true;
                break;
            }
        }
        if (isPromoted12 && !vku::has_extension(nativeExts, name)) {
            LOGI("Vulkan12Core: stripped emulated extension %s from vkCreateDevice", name);
            it = enabledExtensions.erase(it);
        } else {
            ++it;
        }
    }

    auto* unlinks = new Features12UnlinkData();
    unlinks->v12_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    unlinks->sep_ds_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES);
    unlinks->scalar_block_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES);
    unlinks->ubo_std_layout_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES);
    unlinks->desc_indexing_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
    unlinks->subgroup_ext_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES);
    pUserData = unlinks;
}

void Vulkan12CoreModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_needs_emulation[(uint64_t)(uintptr_t)device] = !native;
        LOGI("Device %p created: Vulkan 1.2 core native=%d", device, native);
    }

    if (pUserData) {
        delete reinterpret_cast<Features12UnlinkData*>(pUserData);
    }
}

void Vulkan12CoreModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_needs_emulation.erase((uint64_t)(uintptr_t)device);
}


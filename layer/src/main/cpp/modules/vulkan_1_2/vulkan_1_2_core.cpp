#include "vulkan_1_2_core.h"

#include <mutex>
#include <unordered_map>
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include "imageless_framebuffer_manager.h"
#include "layout_compat.h"
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
    if (layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if (layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    return layout;
}

Vulkan12CoreModule::Vulkan12CoreModule() {
    LOGI("Vulkan12CoreModule initialized");
}

bool Vulkan12CoreModule::is_phys_device_native(VkPhysicalDevice physDev) {
    {
        std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
        auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
        if (it != m_phys_native_support.end()) {
            return it->second;
        }
    }

    uint32_t realApiVer = VK_API_VERSION_1_0;
    PFN_vkGetPhysicalDeviceProperties real_props =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_props) {
        VkPhysicalDeviceProperties props{};
        real_props(physDev, &props);
        realApiVer = props.apiVersion;
    }

    EmulationMode mode = parse_emulation_mode("VULKAN_FIX_EMULATE_VULKAN_1_2", "FORCE_EMULATE_VULKAN_1_2");
    if (mode == EmulationMode::ForceEmulate) {
        LOGI("Vulkan 1.2 Core: FORCE EMULATE enabled for physical device %p (real api: 0x%x)", physDev, realApiVer);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_real_api_version[(uint64_t)(uintptr_t)physDev] = realApiVer;
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }
    if (mode == EmulationMode::Skip) {
        LOGI("Vulkan 1.2 Core: SKIP/BYPASS enabled for physical device %p (real api: 0x%x)", physDev, realApiVer);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_real_api_version[(uint64_t)(uintptr_t)physDev] = realApiVer;
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = true;
        return true;
    }

    bool native = (realApiVer >= VK_API_VERSION_1_2);
    {
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_real_api_version[(uint64_t)(uintptr_t)physDev] = realApiVer;
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    }
    if (!native) {
        LOGI("Physical device %p lacks native Vulkan 1.2 support (native api: 0x%x), enabling Vulkan 1.2 emulation layer!", physDev, realApiVer);
    } else {
        LOGI("Physical device %p natively supports Vulkan 1.2+ (native api: 0x%x)", physDev, realApiVer);
    }
    return native;
}

uint32_t Vulkan12CoreModule::get_phys_real_api_version(VkPhysicalDevice physDev) {
    if (!physDev) return VK_API_VERSION_1_0;
    {
        std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
        auto it = m_phys_real_api_version.find((uint64_t)(uintptr_t)physDev);
        if (it != m_phys_real_api_version.end()) {
            return it->second;
        }
    }
    PFN_vkGetPhysicalDeviceProperties real_props =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_props) {
        VkPhysicalDeviceProperties props{};
        real_props(physDev, &props);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_real_api_version[(uint64_t)(uintptr_t)physDev] = props.apiVersion;
        return props.apiVersion;
    }
    return VK_API_VERSION_1_0;
}

bool Vulkan12CoreModule::is_device_native(VkDevice device) {
    if (!device) return false;
    if (__builtin_expect(device == m_primary_dev.load(std::memory_order_relaxed), 1)) {
        return m_primary_native.load(std::memory_order_relaxed);
    }
    std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
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
    if (claim_vulkan_1_2() && pProperties->apiVersion < VK_API_VERSION_1_2) {
        pProperties->apiVersion = VK_API_VERSION_1_2;
        LOG_OPT_DEBUG("Vulkan12Core: promoted device apiVersion to 1.2 in vkGetPhysicalDeviceProperties");
    }
    (void)physicalDevice;
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
    if (claim_vulkan_1_2() && pProperties->properties.apiVersion < VK_API_VERSION_1_2) {
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

namespace {

bool driver_has_extension(VkPhysicalDevice physicalDevice, const char* name) {
    if (physicalDevice == VK_NULL_HANDLE || name == nullptr) return false;
    // The feature query probes a dozen extensions in a row. Enumerating the
    // driver extension list for each one is wasteful; cache it per physical
    // device (the list is immutable for the lifetime of the process).
    static std::mutex cache_mutex;
    static std::unordered_map<uint64_t, std::vector<VkExtensionProperties>> cache;
    const uint64_t key = (uint64_t)(uintptr_t)physicalDevice;
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)
            get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
        if (!enumerate) return false;
        uint32_t count = 0;
        if (enumerate(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS) return false;
        std::vector<VkExtensionProperties> extensions(count);
        if (count > 0 && enumerate(physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS) {
            return false;
        }
        it = cache.emplace(key, std::move(extensions)).first;
    }
    return vku::has_extension(it->second, name);
}

void add_extension(std::vector<const char*>& extensions, const char* name) {
    for (const char* existing : extensions) {
        if (existing != nullptr && strcmp(existing, name) == 0) return;
    }
    extensions.push_back(name);
}

bool pnext_has_type(const void* pNext, VkStructureType type) {
    const auto* current = reinterpret_cast<const VkBaseInStructure*>(pNext);
    while (current != nullptr) {
        if (current->sType == type) return true;
        current = current->pNext;
    }
    return false;
}

thread_local VkPhysicalDeviceBufferDeviceAddressFeatures* g_bda_features = nullptr;
thread_local VkPhysicalDeviceDescriptorIndexingFeatures* g_di_features = nullptr;
thread_local VkPhysicalDeviceScalarBlockLayoutFeatures* g_scalar_features = nullptr;
thread_local VkPhysicalDeviceUniformBufferStandardLayoutFeatures* g_ubo_features = nullptr;
thread_local VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures* g_subgroup_features = nullptr;
thread_local VkPhysicalDevice8BitStorageFeatures* g_8bit_features = nullptr;
thread_local VkPhysicalDeviceShaderFloat16Int8Features* g_float16_int8_features = nullptr;
thread_local VkPhysicalDeviceVulkanMemoryModelFeatures* g_memory_model_features = nullptr;
thread_local VkPhysicalDeviceImagelessFramebufferFeatures* g_imageless_features = nullptr;
thread_local VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures* g_separate_ds_features = nullptr;

void copy_descriptor_indexing(
    const VkPhysicalDeviceVulkan12Features* source,
    VkPhysicalDeviceDescriptorIndexingFeatures* target
) {
    target->shaderInputAttachmentArrayDynamicIndexing = source->shaderInputAttachmentArrayDynamicIndexing;
    target->shaderUniformTexelBufferArrayDynamicIndexing = source->shaderUniformTexelBufferArrayDynamicIndexing;
    target->shaderStorageTexelBufferArrayDynamicIndexing = source->shaderStorageTexelBufferArrayDynamicIndexing;
    target->shaderUniformBufferArrayNonUniformIndexing = source->shaderUniformBufferArrayNonUniformIndexing;
    target->shaderSampledImageArrayNonUniformIndexing = source->shaderSampledImageArrayNonUniformIndexing;
    target->shaderStorageBufferArrayNonUniformIndexing = source->shaderStorageBufferArrayNonUniformIndexing;
    target->shaderStorageImageArrayNonUniformIndexing = source->shaderStorageImageArrayNonUniformIndexing;
    target->shaderInputAttachmentArrayNonUniformIndexing = source->shaderInputAttachmentArrayNonUniformIndexing;
    target->shaderUniformTexelBufferArrayNonUniformIndexing = source->shaderUniformTexelBufferArrayNonUniformIndexing;
    target->shaderStorageTexelBufferArrayNonUniformIndexing = source->shaderStorageTexelBufferArrayNonUniformIndexing;
    target->descriptorBindingUniformBufferUpdateAfterBind = source->descriptorBindingUniformBufferUpdateAfterBind;
    target->descriptorBindingSampledImageUpdateAfterBind = source->descriptorBindingSampledImageUpdateAfterBind;
    target->descriptorBindingStorageImageUpdateAfterBind = source->descriptorBindingStorageImageUpdateAfterBind;
    target->descriptorBindingStorageBufferUpdateAfterBind = source->descriptorBindingStorageBufferUpdateAfterBind;
    target->descriptorBindingUniformTexelBufferUpdateAfterBind = source->descriptorBindingUniformTexelBufferUpdateAfterBind;
    target->descriptorBindingStorageTexelBufferUpdateAfterBind = source->descriptorBindingStorageTexelBufferUpdateAfterBind;
    target->descriptorBindingUpdateUnusedWhilePending = source->descriptorBindingUpdateUnusedWhilePending;
    target->descriptorBindingPartiallyBound = source->descriptorBindingPartiallyBound;
    target->descriptorBindingVariableDescriptorCount = source->descriptorBindingVariableDescriptorCount;
    target->runtimeDescriptorArray = source->runtimeDescriptorArray;
}

}  // namespace

void Vulkan12CoreModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures || !pUserData) return;
    auto* unlinks = reinterpret_cast<Features12UnlinkData*>(pUserData);

    if (unlinks->v12_features) {
        auto* f = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(unlinks->v12_features);
        const bool native_sampler_mirror = driver_has_extension(
            physicalDevice, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
        const bool native_draw_count = driver_has_extension(
            physicalDevice, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
        const bool native_bda = driver_has_extension(
            physicalDevice, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        const bool native_sampler_minmax = driver_has_extension(
            physicalDevice, VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
        const bool native_di = driver_has_extension(
            physicalDevice, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        const bool native_scalar = driver_has_extension(
            physicalDevice, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
        const bool native_ubo = driver_has_extension(
            physicalDevice, VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME);
        const bool native_sg = driver_has_extension(
            physicalDevice, VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);
        const bool native_8bit = driver_has_extension(physicalDevice, VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
        const bool native_float16_int8 = driver_has_extension(physicalDevice, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        const bool native_memory_model = driver_has_extension(physicalDevice, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
        const bool native_imageless = driver_has_extension(physicalDevice, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME);
        const bool native_sep_ds = driver_has_extension(physicalDevice, VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
        f->samplerMirrorClampToEdge = native_sampler_mirror ? VK_TRUE : VK_FALSE;
        f->drawIndirectCount = native_draw_count ? VK_TRUE : VK_FALSE;
        f->storageBuffer8BitAccess = native_8bit ? VK_TRUE : VK_FALSE;
        f->uniformAndStorageBuffer8BitAccess = native_8bit ? VK_TRUE : VK_FALSE;
        f->storagePushConstant8 = native_8bit ? VK_TRUE : VK_FALSE;
        f->shaderBufferInt64Atomics = VK_FALSE;
        f->shaderSharedInt64Atomics = VK_FALSE;
        f->shaderFloat16 = native_float16_int8 ? VK_TRUE : VK_FALSE;
        f->shaderInt8 = native_float16_int8 ? VK_TRUE : VK_FALSE;
        // Descriptor indexing/scalar/UBO layout are only exposed once their
        // extension feature structs are passed through at device creation.
        f->descriptorIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderInputAttachmentArrayDynamicIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderUniformTexelBufferArrayDynamicIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderStorageTexelBufferArrayDynamicIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderUniformBufferArrayNonUniformIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderSampledImageArrayNonUniformIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderStorageBufferArrayNonUniformIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderStorageImageArrayNonUniformIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderInputAttachmentArrayNonUniformIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderUniformTexelBufferArrayNonUniformIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->shaderStorageTexelBufferArrayNonUniformIndexing = native_di ? VK_TRUE : VK_FALSE;
        f->descriptorBindingUniformBufferUpdateAfterBind = native_di ? VK_TRUE : VK_FALSE;
        f->descriptorBindingSampledImageUpdateAfterBind = native_di ? VK_TRUE : VK_FALSE;
        f->descriptorBindingStorageImageUpdateAfterBind = native_di ? VK_TRUE : VK_FALSE;
        f->descriptorBindingStorageBufferUpdateAfterBind = native_di ? VK_TRUE : VK_FALSE;
        f->descriptorBindingUniformTexelBufferUpdateAfterBind = native_di ? VK_TRUE : VK_FALSE;
        f->descriptorBindingStorageTexelBufferUpdateAfterBind = native_di ? VK_TRUE : VK_FALSE;
        f->descriptorBindingUpdateUnusedWhilePending = native_di ? VK_TRUE : VK_FALSE;
        f->descriptorBindingPartiallyBound = native_di ? VK_TRUE : VK_FALSE;
        f->descriptorBindingVariableDescriptorCount = native_di ? VK_TRUE : VK_FALSE;
        f->runtimeDescriptorArray = native_di ? VK_TRUE : VK_FALSE;
        f->samplerFilterMinmax = native_sampler_minmax ? VK_TRUE : VK_FALSE;
        f->scalarBlockLayout = native_scalar ? VK_TRUE : VK_FALSE;
        f->imagelessFramebuffer = native_imageless ? VK_TRUE : VK_FALSE;
        f->uniformBufferStandardLayout = native_ubo ? VK_TRUE : VK_FALSE;
        f->shaderSubgroupExtendedTypes = native_sg ? VK_TRUE : VK_FALSE;
        f->separateDepthStencilLayouts = native_sep_ds ? VK_TRUE : VK_FALSE;
        f->hostQueryReset = VK_TRUE;
        f->timelineSemaphore = VK_TRUE;
        f->bufferDeviceAddress = native_bda ? VK_TRUE : VK_FALSE;
        f->bufferDeviceAddressCaptureReplay = VK_FALSE;
        f->bufferDeviceAddressMultiDevice = VK_FALSE;
        f->vulkanMemoryModel = native_memory_model ? VK_TRUE : VK_FALSE;
        f->vulkanMemoryModelDeviceScope = native_memory_model ? VK_TRUE : VK_FALSE;
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

    // Native promotion: this driver exposes 1.1/1.2 extensions that provide
    // the corresponding Vulkan 1.2 features. Re-link the application's
    // extension feature structs and enable those extensions for the driver.
    auto relink_native = [&](void* struct_ptr, const char* extension_name) {
        if (struct_ptr == nullptr) return;
        if (!vku::has_extension(nativeExts, extension_name)) return;
        vku::relink_pnext(pCreateInfo->pNext, struct_ptr);
        add_extension(enabledExtensions, extension_name);
    };
    relink_native(unlinks->sep_ds_features, VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
    relink_native(unlinks->scalar_block_features, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
    relink_native(unlinks->ubo_std_layout_features, VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME);
    relink_native(unlinks->desc_indexing_features, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    relink_native(unlinks->subgroup_ext_features, VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);

    delete g_bda_features;
    g_bda_features = nullptr;
    delete g_di_features;
    g_di_features = nullptr;
    delete g_scalar_features;
    g_scalar_features = nullptr;
    delete g_ubo_features;
    g_ubo_features = nullptr;
    delete g_subgroup_features;
    g_subgroup_features = nullptr;
    if (vku::has_extension(nativeExts, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
        add_extension(enabledExtensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        if (unlinks->v12_features != nullptr &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)) {
            const auto* v12 = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(unlinks->v12_features);
            auto* bda = new VkPhysicalDeviceBufferDeviceAddressFeatures{};
            bda->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
            bda->bufferDeviceAddress = v12->bufferDeviceAddress;
            bda->bufferDeviceAddressCaptureReplay = VK_FALSE;
            bda->bufferDeviceAddressMultiDevice = VK_FALSE;
            bda->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = bda;
            g_bda_features = bda;
        }
    }
    if (unlinks->v12_features != nullptr) {
        const auto* v12 = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(unlinks->v12_features);
        if (vku::has_extension(nativeExts, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) &&
            v12->descriptorIndexing &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES)) {
            auto* di = new VkPhysicalDeviceDescriptorIndexingFeatures{};
            di->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            copy_descriptor_indexing(v12, di);
            di->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = di;
            g_di_features = di;
            add_extension(enabledExtensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        }
        if (vku::has_extension(nativeExts, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME) &&
            v12->scalarBlockLayout &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES)) {
            auto* scalar = new VkPhysicalDeviceScalarBlockLayoutFeatures{};
            scalar->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
            scalar->scalarBlockLayout = v12->scalarBlockLayout;
            scalar->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = scalar;
            g_scalar_features = scalar;
            add_extension(enabledExtensions, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
        }
        if (vku::has_extension(nativeExts, VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME) &&
            v12->uniformBufferStandardLayout &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES)) {
            auto* ubo = new VkPhysicalDeviceUniformBufferStandardLayoutFeatures{};
            ubo->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES;
            ubo->uniformBufferStandardLayout = v12->uniformBufferStandardLayout;
            ubo->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = ubo;
            g_ubo_features = ubo;
            add_extension(enabledExtensions, VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME);
        }
        if (vku::has_extension(nativeExts, VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME) &&
            v12->shaderSubgroupExtendedTypes &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES)) {
            auto* subgroup = new VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures{};
            subgroup->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES;
            subgroup->shaderSubgroupExtendedTypes = v12->shaderSubgroupExtendedTypes;
            subgroup->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = subgroup;
            g_subgroup_features = subgroup;
            add_extension(enabledExtensions, VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);
        }
        if (vku::has_extension(nativeExts, VK_KHR_8BIT_STORAGE_EXTENSION_NAME) &&
            v12->storageBuffer8BitAccess &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES)) {
            auto* storage = new VkPhysicalDevice8BitStorageFeatures{};
            storage->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
            storage->storageBuffer8BitAccess = v12->storageBuffer8BitAccess;
            storage->uniformAndStorageBuffer8BitAccess = v12->uniformAndStorageBuffer8BitAccess;
            storage->storagePushConstant8 = v12->storagePushConstant8;
            storage->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = storage;
            g_8bit_features = storage;
            add_extension(enabledExtensions, VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
        }
        if (vku::has_extension(nativeExts, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) &&
            v12->shaderFloat16 &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES)) {
            auto* shader = new VkPhysicalDeviceShaderFloat16Int8Features{};
            shader->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
            shader->shaderFloat16 = v12->shaderFloat16;
            shader->shaderInt8 = v12->shaderInt8;
            shader->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = shader;
            g_float16_int8_features = shader;
            add_extension(enabledExtensions, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        }
        if (vku::has_extension(nativeExts, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME) &&
            v12->vulkanMemoryModel &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES)) {
            auto* memory = new VkPhysicalDeviceVulkanMemoryModelFeatures{};
            memory->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
            memory->vulkanMemoryModel = v12->vulkanMemoryModel;
            memory->vulkanMemoryModelDeviceScope = v12->vulkanMemoryModelDeviceScope;
            memory->vulkanMemoryModelAvailabilityVisibilityChains = v12->vulkanMemoryModelAvailabilityVisibilityChains;
            memory->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = memory;
            g_memory_model_features = memory;
            add_extension(enabledExtensions, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
        }
        if (vku::has_extension(nativeExts, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME) &&
            v12->imagelessFramebuffer &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES)) {
            auto* imageless = new VkPhysicalDeviceImagelessFramebufferFeatures{};
            imageless->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES;
            imageless->imagelessFramebuffer = v12->imagelessFramebuffer;
            imageless->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = imageless;
            g_imageless_features = imageless;
            add_extension(enabledExtensions, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME);
        }
        if (vku::has_extension(nativeExts, VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME) &&
            v12->separateDepthStencilLayouts &&
            !pnext_has_type(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES)) {
            auto* separate = new VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures{};
            separate->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES;
            separate->separateDepthStencilLayouts = v12->separateDepthStencilLayouts;
            separate->pNext = const_cast<void*>(pCreateInfo->pNext);
            pCreateInfo->pNext = separate;
            g_separate_ds_features = separate;
            add_extension(enabledExtensions, VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
        }
    }
    if (vku::has_extension(nativeExts, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)) {
        add_extension(enabledExtensions, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
    }
    if (vku::has_extension(nativeExts, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)) {
        add_extension(enabledExtensions, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    }
    if (vku::has_extension(nativeExts, VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME)) {
        add_extension(enabledExtensions, VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
    }
}

void Vulkan12CoreModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        {
            std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
            m_device_needs_emulation[(uint64_t)(uintptr_t)device] = !native;
            if (m_primary_dev.load(std::memory_order_relaxed) == VK_NULL_HANDLE) {
                m_primary_native.store(native, std::memory_order_relaxed);
                m_primary_dev.store(device, std::memory_order_release);
            }
        }

        const char* force_env = getenv("VULKAN_FIX_EMULATE_IMAGELESS_FRAMEBUFFER");
        const bool force = (force_env != nullptr) &&
            (strcmp(force_env, "1") == 0 || strcasecmp(force_env, "force") == 0 ||
             strcasecmp(force_env, "true") == 0);
        const bool emulate_imageless = force || !driver_has_extension(physicalDevice, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME);
        ImagelessFramebufferManager::get().set_device_emulated(device, emulate_imageless);

        const char* ds_env = getenv("VULKAN_FIX_EMULATE_SEPARATE_DS_LAYOUTS");
        const bool force_ds = (ds_env != nullptr) &&
            (strcmp(ds_env, "1") == 0 || strcasecmp(ds_env, "force") == 0 ||
             strcasecmp(ds_env, "true") == 0);
        const bool emulate_separate_ds = force_ds || !driver_has_extension(physicalDevice, VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
        LayoutCompat::get().set_device_emulated(device, emulate_separate_ds);

        LOGI("Device %p created: Vulkan 1.2 core native=%d, imageless=%s, separate D/S layouts=%s",
             device, native ? 1 : 0,
             emulate_imageless ? "EMULATED" : "NATIVE",
             emulate_separate_ds ? "EMULATED" : "NATIVE");
    }

    if (pUserData) {
        delete reinterpret_cast<Features12UnlinkData*>(pUserData);
    }
}

void Vulkan12CoreModule::on_destroy_device(VkDevice device) {
    if (m_primary_dev.load(std::memory_order_relaxed) == device) {
        m_primary_dev.store(VK_NULL_HANDLE, std::memory_order_relaxed);
        m_primary_native.store(false, std::memory_order_relaxed);
    }
    ImagelessFramebufferManager::get().remove_device(device);
    LayoutCompat::get().remove_device(device);
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_device_needs_emulation.erase((uint64_t)(uintptr_t)device);
}













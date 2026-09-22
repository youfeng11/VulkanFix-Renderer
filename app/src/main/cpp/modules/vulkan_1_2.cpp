#include "vulkan_1_2.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>

REGISTER_LAYER_MODULE(Vulkan12Module);

static const char* const s_vulkan_1_2_extensions[] = {
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
    VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
    VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME,
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
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
};

static inline VkImageLayout sanitize_layout(VkImageLayout layout) {
    // Map Vulkan 1.2 separate depth/stencil layouts to 1.0 standard layouts if needed
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

Vulkan12Module::Vulkan12Module() {
    LOGI("Initialized Vulkan 1.2 full emulation module");
}

bool Vulkan12Module::is_phys_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    const char* force_emu = getenv("FORCE_EMULATE_VULKAN_1_2");
    if (force_emu && (strcmp(force_emu, "1") == 0 || strcasecmp(force_emu, "true") == 0)) {
        LOGI("FORCE_EMULATE_VULKAN_1_2 set, enabling emulation for physical device %p", physDev);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }

    bool native = false;
    PFN_vkGetPhysicalDeviceProperties real_props =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_props) {
        VkPhysicalDeviceProperties props{};
        real_props(physDev, &props);
        if (props.apiVersion >= VK_API_VERSION_1_2) {
            native = true;
        }
    }

    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native Vulkan 1.2 support, enabling Vulkan 1.2 emulation layer!", physDev);
    } else {
        LOGI("Physical device %p natively supports Vulkan 1.2+", physDev);
    }
    return native;
}

bool Vulkan12Module::is_device_native(VkDevice device) {
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_needs_emulation.find((uint64_t)(uintptr_t)device);
    if (it != m_device_needs_emulation.end()) {
        return !it->second;
    }
    return false;
}

void Vulkan12Module::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    if (is_phys_device_native(physicalDevice)) return;

    for (const char* extName : s_vulkan_1_2_extensions) {
        if (!vku::has_extension(extensions, extName)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, extName, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = 1;
            extensions.push_back(prop);
            LOG_OPT_DEBUG("Vulkan12: injected extension %s", extName);
        }
    }
}

void Vulkan12Module::on_get_properties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties
) {
    if (!pProperties) return;
    if (pProperties->apiVersion < VK_API_VERSION_1_2) {
        pProperties->apiVersion = VK_API_VERSION_1_2;
        LOG_OPT_DEBUG("Vulkan12: promoted device apiVersion to 1.2 in vkGetPhysicalDeviceProperties");
    }
}

struct PropertiesUnlinkData {
    void* v12_props = nullptr;
    void* driver_props = nullptr;
    void* float_props = nullptr;
    void* indexing_props = nullptr;
    void* timeline_props = nullptr;
    void* minmax_props = nullptr;
};

void Vulkan12Module::on_pre_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pProperties) return;

    auto* unlinks = new PropertiesUnlinkData();
    unlinks->v12_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);
    unlinks->driver_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    unlinks->float_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES);
    unlinks->indexing_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES);
    unlinks->timeline_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES);
    unlinks->minmax_props = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES);
    pUserData = unlinks;
}

void Vulkan12Module::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties) return;

    if (pProperties->properties.apiVersion < VK_API_VERSION_1_2) {
        pProperties->properties.apiVersion = VK_API_VERSION_1_2;
    }

    if (pUserData) {
        auto* unlinks = reinterpret_cast<PropertiesUnlinkData*>(pUserData);

        if (unlinks->driver_props) {
            auto* dp = vku::relink_pnext<VkPhysicalDeviceDriverProperties>(pProperties->pNext, unlinks->driver_props);
            if (dp) {
                dp->driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
                strncpy(dp->driverName, "Vulkan-1.2-Layer", VK_MAX_DRIVER_NAME_SIZE - 1);
                strncpy(dp->driverInfo, "Emulated Vulkan 1.2 Compatibility Layer", VK_MAX_DRIVER_INFO_SIZE - 1);
                dp->conformanceVersion = {1, 2, 0, 0};
            }
        }

        if (unlinks->v12_props) {
            auto* v12 = vku::relink_pnext<VkPhysicalDeviceVulkan12Properties>(pProperties->pNext, unlinks->v12_props);
            if (v12) {
                v12->driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
                strncpy(v12->driverName, "Vulkan-1.2-Layer", VK_MAX_DRIVER_NAME_SIZE - 1);
                strncpy(v12->driverInfo, "Emulated Vulkan 1.2 Compatibility Layer", VK_MAX_DRIVER_INFO_SIZE - 1);
                v12->conformanceVersion = {1, 2, 0, 0};
                v12->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
                v12->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
                v12->shaderSignedZeroInfNanPreserveFloat16 = VK_TRUE;
                v12->shaderSignedZeroInfNanPreserveFloat32 = VK_TRUE;
                v12->shaderSignedZeroInfNanPreserveFloat64 = VK_TRUE;
                v12->maxUpdateAfterBindDescriptorsInAllPools = 500000;
                v12->shaderUniformBufferArrayNonUniformIndexingNative = VK_TRUE;
                v12->shaderSampledImageArrayNonUniformIndexingNative = VK_TRUE;
                v12->shaderStorageBufferArrayNonUniformIndexingNative = VK_TRUE;
                v12->shaderStorageImageArrayNonUniformIndexingNative = VK_TRUE;
                v12->shaderInputAttachmentArrayNonUniformIndexingNative = VK_TRUE;
                v12->robustBufferAccessUpdateAfterBind = VK_TRUE;
                v12->quadDivergentImplicitLod = VK_TRUE;
                v12->maxPerStageDescriptorUpdateAfterBindSamplers = 1048576;
                v12->maxPerStageDescriptorUpdateAfterBindUniformBuffers = 1048576;
                v12->maxPerStageDescriptorUpdateAfterBindStorageBuffers = 1048576;
                v12->maxPerStageDescriptorUpdateAfterBindSampledImages = 1048576;
                v12->maxPerStageDescriptorUpdateAfterBindStorageImages = 1048576;
                v12->maxPerStageDescriptorUpdateAfterBindInputAttachments = 1048576;
                v12->maxPerStageUpdateAfterBindResources = 1048576;
                v12->maxDescriptorSetUpdateAfterBindSamplers = 1048576;
                v12->maxDescriptorSetUpdateAfterBindUniformBuffers = 1048576;
                v12->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 8;
                v12->maxDescriptorSetUpdateAfterBindStorageBuffers = 1048576;
                v12->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 8;
                v12->maxDescriptorSetUpdateAfterBindSampledImages = 1048576;
                v12->maxDescriptorSetUpdateAfterBindStorageImages = 1048576;
                v12->maxDescriptorSetUpdateAfterBindInputAttachments = 1048576;
                v12->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT | VK_RESOLVE_MODE_AVERAGE_BIT | VK_RESOLVE_MODE_MIN_BIT | VK_RESOLVE_MODE_MAX_BIT;
                v12->supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT | VK_RESOLVE_MODE_MIN_BIT | VK_RESOLVE_MODE_MAX_BIT;
                v12->independentResolveNone = VK_TRUE;
                v12->independentResolve = VK_TRUE;
                v12->filterMinmaxSingleComponentFormats = VK_TRUE;
                v12->filterMinmaxImageComponentMapping = VK_TRUE;
                v12->maxTimelineSemaphoreValueDifference = UINT64_MAX;
                v12->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
            }
        }

        if (unlinks->timeline_props) {
            auto* tp = vku::relink_pnext<VkPhysicalDeviceTimelineSemaphoreProperties>(pProperties->pNext, unlinks->timeline_props);
            if (tp) {
                tp->maxTimelineSemaphoreValueDifference = UINT64_MAX;
            }
        }

        delete unlinks;
    }
}

void Vulkan12Module::on_get_features(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures
) {
    if (!pFeatures) return;
    pFeatures->samplerAnisotropy = VK_TRUE;
}

struct FeaturesUnlinkData {
    void* v12_features = nullptr;
    void* timeline_features = nullptr;
    void* host_query_features = nullptr;
    void* scalar_block_features = nullptr;
    void* imageless_fb_features = nullptr;
    void* separate_ds_features = nullptr;
    void* ubo_std_features = nullptr;
    void* desc_indexing_features = nullptr;
    void* bda_features = nullptr;
};

void Vulkan12Module::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    auto* unlinks = new FeaturesUnlinkData();
    unlinks->v12_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    unlinks->timeline_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
    unlinks->host_query_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
    unlinks->scalar_block_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES);
    unlinks->imageless_fb_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES);
    unlinks->separate_ds_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES);
    unlinks->ubo_std_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES);
    unlinks->desc_indexing_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
    unlinks->bda_features = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
    pUserData = unlinks;
}

void Vulkan12Module::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures) return;

    if (pUserData) {
        auto* unlinks = reinterpret_cast<FeaturesUnlinkData*>(pUserData);

        if (unlinks->v12_features) {
            auto* v12 = vku::relink_pnext<VkPhysicalDeviceVulkan12Features>(pFeatures->pNext, unlinks->v12_features);
            if (v12) {
                v12->samplerMirrorClampToEdge = VK_TRUE;
                v12->drawIndirectCount = VK_TRUE;
                v12->descriptorIndexing = VK_TRUE;
                v12->shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
                v12->shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
                v12->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
                v12->shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
                v12->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
                v12->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
                v12->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
                v12->shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
                v12->shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
                v12->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
                v12->descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
                v12->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
                v12->descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
                v12->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
                v12->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
                v12->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
                v12->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
                v12->descriptorBindingPartiallyBound = VK_TRUE;
                v12->descriptorBindingVariableDescriptorCount = VK_TRUE;
                v12->runtimeDescriptorArray = VK_TRUE;
                v12->samplerFilterMinmax = VK_TRUE;
                v12->scalarBlockLayout = VK_TRUE;
                v12->imagelessFramebuffer = VK_TRUE;
                v12->uniformBufferStandardLayout = VK_TRUE;
                v12->shaderSubgroupExtendedTypes = VK_TRUE;
                v12->separateDepthStencilLayouts = VK_TRUE;
                v12->hostQueryReset = VK_TRUE;
                v12->timelineSemaphore = VK_TRUE;
                v12->bufferDeviceAddress = VK_TRUE;
                v12->shaderOutputViewportIndex = VK_TRUE;
                v12->shaderOutputLayer = VK_TRUE;
                v12->subgroupBroadcastDynamicId = VK_TRUE;
                LOG_OPT_DEBUG("Vulkan12: populated features in VkPhysicalDeviceVulkan12Features");
            }
        }

        if (unlinks->timeline_features) {
            auto* tf = vku::relink_pnext<VkPhysicalDeviceTimelineSemaphoreFeatures>(pFeatures->pNext, unlinks->timeline_features);
            if (tf) tf->timelineSemaphore = VK_TRUE;
        }

        if (unlinks->host_query_features) {
            auto* hq = vku::relink_pnext<VkPhysicalDeviceHostQueryResetFeatures>(pFeatures->pNext, unlinks->host_query_features);
            if (hq) hq->hostQueryReset = VK_TRUE;
        }

        if (unlinks->scalar_block_features) {
            auto* sb = vku::relink_pnext<VkPhysicalDeviceScalarBlockLayoutFeatures>(pFeatures->pNext, unlinks->scalar_block_features);
            if (sb) sb->scalarBlockLayout = VK_TRUE;
        }

        if (unlinks->imageless_fb_features) {
            auto* ifb = vku::relink_pnext<VkPhysicalDeviceImagelessFramebufferFeatures>(pFeatures->pNext, unlinks->imageless_fb_features);
            if (ifb) ifb->imagelessFramebuffer = VK_TRUE;
        }

        if (unlinks->separate_ds_features) {
            auto* sds = vku::relink_pnext<VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures>(pFeatures->pNext, unlinks->separate_ds_features);
            if (sds) sds->separateDepthStencilLayouts = VK_TRUE;
        }

        if (unlinks->ubo_std_features) {
            auto* ubo = vku::relink_pnext<VkPhysicalDeviceUniformBufferStandardLayoutFeatures>(pFeatures->pNext, unlinks->ubo_std_features);
            if (ubo) ubo->uniformBufferStandardLayout = VK_TRUE;
        }

        if (unlinks->desc_indexing_features) {
            auto* di = vku::relink_pnext<VkPhysicalDeviceDescriptorIndexingFeatures>(pFeatures->pNext, unlinks->desc_indexing_features);
            if (di) {
                di->runtimeDescriptorArray = VK_TRUE;
                di->descriptorBindingPartiallyBound = VK_TRUE;
            }
        }

        if (unlinks->bda_features) {
            auto* bda = vku::relink_pnext<VkPhysicalDeviceBufferDeviceAddressFeatures>(pFeatures->pNext, unlinks->bda_features);
            if (bda) bda->bufferDeviceAddress = VK_TRUE;
        }

        delete unlinks;
    }
}

void Vulkan12Module::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // 1. Strip non-native 1.2 extensions from enabledExtensions
    for (const char* extName : s_vulkan_1_2_extensions) {
        if (vku::strip_extension(enabledExtensions, extName)) {
            LOG_OPT_DEBUG("Vulkan12: stripped %s from enabledExtensions for native driver", extName);
        }
    }

    // 2. Unlink Vulkan 1.2 feature structures
    auto* unlinks = new FeaturesUnlinkData();
    unlinks->v12_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    unlinks->timeline_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
    unlinks->host_query_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
    unlinks->scalar_block_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES);
    unlinks->imageless_fb_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES);
    unlinks->separate_ds_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES);
    unlinks->ubo_std_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES);
    unlinks->desc_indexing_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
    unlinks->bda_features = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
    pUserData = unlinks;

    LOGI("vkCreateDevice: safely prepared device creation with Vulkan 1.2 emulation");
}

void Vulkan12Module::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_needs_emulation[(uint64_t)(uintptr_t)device] = !native;
        LOGI("Device %p created: Vulkan 1.2 native=%d", device, native);
    }

    if (pUserData) {
        delete reinterpret_cast<FeaturesUnlinkData*>(pUserData);
    }
}

void Vulkan12Module::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_needs_emulation.erase((uint64_t)(uintptr_t)device);
}

// ============================================================================
// Semaphore & Timeline Semaphore
// ============================================================================

void Vulkan12Module::on_pre_create_semaphore(
    VkDevice device,
    VkSemaphoreCreateInfo& createInfo,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_device_native(device)) return;

    auto* typeInfo = vku::find_pnext_mut<VkSemaphoreTypeCreateInfo>(
        createInfo.pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);

    if (typeInfo && typeInfo->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
        uint64_t* initVal = new uint64_t(typeInfo->initialValue);
        pUserData = initVal;

        // Unlink so native 1.0/1.1 driver creates a valid standard semaphore without error
        vku::unlink_pnext(createInfo.pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        LOG_OPT_DEBUG("Vulkan12: unlinked VkSemaphoreTypeCreateInfo, emulating timeline semaphore initialValue=%" PRIu64, *initVal);
    }
}

void Vulkan12Module::on_post_create_semaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    VkResult result,
    VkSemaphore semaphore,
    void* pUserData
) {
    if (result == VK_SUCCESS && semaphore != VK_NULL_HANDLE && pUserData) {
        uint64_t initVal = *reinterpret_cast<uint64_t*>(pUserData);
        delete reinterpret_cast<uint64_t*>(pUserData);

        auto state = std::make_shared<TimelineSemaphoreState>();
        state->counter.store(initVal);

        std::lock_guard<std::mutex> lock(m_semaphore_mutex);
        m_timeline_semaphores[(uint64_t)(uintptr_t)semaphore] = state;
        LOG_OPT_DEBUG("Vulkan12: registered emulated timeline semaphore %p with initial value %" PRIu64,
                      VK_HANDLE(semaphore), initVal);
    }
}

void Vulkan12Module::on_destroy_semaphore(
    VkDevice device,
    VkSemaphore semaphore
) {
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    m_timeline_semaphores.erase((uint64_t)(uintptr_t)semaphore);
}

Vulkan12Module::FenceHolder::~FenceHolder() {
    if (isInternal && fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        PFN_vkDestroyFence real_df =
            (PFN_vkDestroyFence) get_real_proc(get_last_instance(), device, "vkDestroyFence");
        if (real_df) real_df(device, fence, nullptr);
    }
}

void Vulkan12Module::check_pending_signals_locked(std::shared_ptr<TimelineSemaphoreState>& state) {
    if (!state) return;
    PFN_vkGetFenceStatus real_gfs = nullptr;

    auto it = state->pendingSignals.begin();
    while (it != state->pendingSignals.end()) {
        auto fh = it->fenceHolder;
        if (fh && fh->fence != VK_NULL_HANDLE) {
            if (!real_gfs) {
                real_gfs = (PFN_vkGetFenceStatus) get_real_proc(get_last_instance(), fh->device, "vkGetFenceStatus");
            }
            if (real_gfs && real_gfs(fh->device, fh->fence) == VK_SUCCESS) {
                if (it->targetValue > state->counter.load()) {
                    state->counter.store(it->targetValue);
                    state->cv.notify_all();
                }
                it = state->pendingSignals.erase(it);
                continue;
            }
        }
        ++it;
    }
}

bool Vulkan12Module::is_timeline_semaphore(VkSemaphore semaphore) {
    if (semaphore == VK_NULL_HANDLE) return false;
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    return m_timeline_semaphores.find((uint64_t)(uintptr_t)semaphore) != m_timeline_semaphores.end();
}

void Vulkan12Module::on_queue_wait_idle(VkQueue queue) {
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    for (auto& pair : m_timeline_semaphores) {
        auto& state = pair.second;
        for (auto& ps : state->pendingSignals) {
            if (ps.targetValue > state->counter.load()) {
                state->counter.store(ps.targetValue);
            }
        }
        state->pendingSignals.clear();
        state->cv.notify_all();
    }
}

void Vulkan12Module::on_device_wait_idle(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_semaphore_mutex);
    for (auto& pair : m_timeline_semaphores) {
        auto& state = pair.second;
        for (auto& ps : state->pendingSignals) {
            if (ps.targetValue > state->counter.load()) {
                state->counter.store(ps.targetValue);
            }
        }
        state->pendingSignals.clear();
        state->cv.notify_all();
    }
}

bool Vulkan12Module::on_get_semaphore_counter_value(
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t* pValue,
    VkResult& outResult
) {
    if (!pValue) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    std::shared_ptr<TimelineSemaphoreState> state;
    {
        std::lock_guard<std::mutex> lock(m_semaphore_mutex);
        auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)semaphore);
        if (it != m_timeline_semaphores.end()) {
            state = it->second;
            check_pending_signals_locked(state);
        }
    }

    if (state) {
        *pValue = state->counter.load();
        outResult = VK_SUCCESS;
        return true;
    }
    return false;
}

bool Vulkan12Module::on_wait_semaphores(
    VkDevice device,
    const VkSemaphoreWaitInfo* pWaitInfo,
    uint64_t timeout,
    VkResult& outResult
) {
    if (!pWaitInfo || pWaitInfo->semaphoreCount == 0) {
        outResult = VK_SUCCESS;
        return true;
    }

    std::vector<std::shared_ptr<TimelineSemaphoreState>> states(pWaitInfo->semaphoreCount);
    bool has_emulated = false;

    {
        std::lock_guard<std::mutex> lock(m_semaphore_mutex);
        for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
            auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)pWaitInfo->pSemaphores[i]);
            if (it != m_timeline_semaphores.end()) {
                states[i] = it->second;
                has_emulated = true;
                check_pending_signals_locked(states[i]);
            }
        }
    }

    if (!has_emulated) return false;

    bool waitAny = (pWaitInfo->flags & VK_SEMAPHORE_WAIT_ANY_BIT);
    auto startTime = std::chrono::steady_clock::now();

    PFN_vkWaitForFences real_wff =
        (PFN_vkWaitForFences) get_real_proc(get_last_instance(), device, "vkWaitForFences");

    while (true) {
        // 1. Check if condition already satisfied
        {
            std::lock_guard<std::mutex> lock(m_semaphore_mutex);
            for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
                if (states[i]) {
                    check_pending_signals_locked(states[i]);
                }
            }
        }

        bool satisfied = waitAny ? false : true;
        for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
            uint64_t current = states[i] ? states[i]->counter.load() : pWaitInfo->pValues[i];
            bool met = (current >= pWaitInfo->pValues[i]);
            if (waitAny && met) {
                satisfied = true;
                break;
            } else if (!waitAny && !met) {
                satisfied = false;
                break;
            }
        }

        if (satisfied) {
            outResult = VK_SUCCESS;
            return true;
        }

        if (timeout == 0) {
            outResult = VK_TIMEOUT;
            return true;
        }

        uint64_t elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (timeout != UINT64_MAX && elapsedNs >= timeout) {
            outResult = VK_TIMEOUT;
            return true;
        }

        uint64_t remainingTimeout = (timeout == UINT64_MAX) ? UINT64_MAX : (timeout - elapsedNs);

        // 2. Find a pending fence to wait on
        std::shared_ptr<FenceHolder> fenceToWait = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_semaphore_mutex);
            for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
                if (states[i] && states[i]->counter.load() < pWaitInfo->pValues[i]) {
                    for (auto& ps : states[i]->pendingSignals) {
                        if (ps.targetValue >= pWaitInfo->pValues[i]) {
                            fenceToWait = ps.fenceHolder;
                            break;
                        }
                    }
                    if (fenceToWait) break;
                    if (!states[i]->pendingSignals.empty()) {
                        fenceToWait = states[i]->pendingSignals.back().fenceHolder;
                        break;
                    }
                }
            }
        }

        if (fenceToWait && fenceToWait->fence != VK_NULL_HANDLE && real_wff) {
            uint64_t sliceTimeout = std::min<uint64_t>(remainingTimeout, 50000000ULL); // 50ms
            VkResult wr = real_wff(fenceToWait->device, 1, &fenceToWait->fence, VK_TRUE, sliceTimeout);
            if (wr == VK_SUCCESS || wr == VK_TIMEOUT) {
                continue;
            } else {
                outResult = wr;
                return true;
            }
        } else {
            std::unique_lock<std::mutex> lock(m_semaphore_mutex);
            if (states[0]) {
                states[0]->cv.wait_for(lock, std::chrono::milliseconds(5));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }
}

bool Vulkan12Module::on_signal_semaphore(
    VkDevice device,
    const VkSemaphoreSignalInfo* pSignalInfo,
    VkResult& outResult
) {
    if (!pSignalInfo) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    std::shared_ptr<TimelineSemaphoreState> state;
    {
        std::lock_guard<std::mutex> lock(m_semaphore_mutex);
        auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)pSignalInfo->semaphore);
        if (it != m_timeline_semaphores.end()) {
            state = it->second;
        }
    }

    if (state) {
        state->counter.store(pSignalInfo->value);
        state->cv.notify_all();
        outResult = VK_SUCCESS;
        return true;
    }
    return false;
}

bool Vulkan12Module::on_queue_submit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence,
    VkResult& outResult
) {
    VkDevice device = LayerManager::get().get_device_for_queue(queue);
    if (is_device_native(device)) return false;

    PFN_vkQueueSubmit real_fn =
        (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueSubmit");
    if (!real_fn) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    if (submitCount == 0 || !pSubmits) {
        outResult = real_fn(queue, submitCount, pSubmits, fence);
        return true;
    }

    struct SignalTarget {
        VkSemaphore semaphore;
        uint64_t targetValue;
    };
    std::vector<SignalTarget> emulatedSignals;

    // Check all submits for timeline semaphore signals
    for (uint32_t s = 0; s < submitCount; ++s) {
        auto* timelineInfo = vku::find_pnext<VkTimelineSemaphoreSubmitInfo>(
            pSubmits[s].pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        if (timelineInfo && timelineInfo->pSignalSemaphoreValues) {
            for (uint32_t i = 0; i < timelineInfo->signalSemaphoreValueCount; ++i) {
                if (i < pSubmits[s].signalSemaphoreCount) {
                    VkSemaphore sem = pSubmits[s].pSignalSemaphores[i];
                    if (is_timeline_semaphore(sem)) {
                        emulatedSignals.push_back({sem, timelineInfo->pSignalSemaphoreValues[i]});
                    }
                }
            }
        }
    }

    // Determine fence to use
    std::shared_ptr<FenceHolder> fenceHolder = nullptr;
    VkFence fenceToSubmit = fence;

    if (!emulatedSignals.empty()) {
        if (fence != VK_NULL_HANDLE) {
            fenceHolder = std::make_shared<FenceHolder>();
            fenceHolder->device = device;
            fenceHolder->fence = fence;
            fenceHolder->isInternal = false;
        } else {
            VkFence internalFence = VK_NULL_HANDLE;
            PFN_vkCreateFence real_cf =
                (PFN_vkCreateFence) get_real_proc(get_last_instance(), device, "vkCreateFence");
            VkFenceCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (real_cf && real_cf(device, &fci, nullptr, &internalFence) == VK_SUCCESS) {
                fenceHolder = std::make_shared<FenceHolder>();
                fenceHolder->device = device;
                fenceHolder->fence = internalFence;
                fenceHolder->isInternal = true;
                fenceToSubmit = internalFence;
            }
        }
    }

    // Clean submits of emulated timeline semaphores
    struct CleanData {
        std::vector<VkSemaphore> waitSemaphores;
        std::vector<VkPipelineStageFlags> waitDstStageMask;
        std::vector<VkSemaphore> signalSemaphores;
    };
    std::vector<CleanData> cleanData(submitCount);
    std::vector<VkSubmitInfo> cleanSubmits(submitCount);

    for (uint32_t s = 0; s < submitCount; ++s) {
        const auto& orig = pSubmits[s];
        auto& cd = cleanData[s];
        auto& cs = cleanSubmits[s];
        cs = orig;

        if (orig.pWaitSemaphores && orig.waitSemaphoreCount > 0) {
            for (uint32_t i = 0; i < orig.waitSemaphoreCount; ++i) {
                if (!is_timeline_semaphore(orig.pWaitSemaphores[i])) {
                    cd.waitSemaphores.push_back(orig.pWaitSemaphores[i]);
                    if (orig.pWaitDstStageMask) {
                        cd.waitDstStageMask.push_back(orig.pWaitDstStageMask[i]);
                    } else {
                        cd.waitDstStageMask.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                    }
                }
            }
            cs.waitSemaphoreCount = (uint32_t) cd.waitSemaphores.size();
            cs.pWaitSemaphores = cd.waitSemaphores.empty() ? nullptr : cd.waitSemaphores.data();
            cs.pWaitDstStageMask = cd.waitDstStageMask.empty() ? nullptr : cd.waitDstStageMask.data();
        }

        if (orig.pSignalSemaphores && orig.signalSemaphoreCount > 0) {
            for (uint32_t i = 0; i < orig.signalSemaphoreCount; ++i) {
                if (!is_timeline_semaphore(orig.pSignalSemaphores[i])) {
                    cd.signalSemaphores.push_back(orig.pSignalSemaphores[i]);
                }
            }
            cs.signalSemaphoreCount = (uint32_t) cd.signalSemaphores.size();
            cs.pSignalSemaphores = cd.signalSemaphores.empty() ? nullptr : cd.signalSemaphores.data();
        }

        vku::unlink_pnext(cs.pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
    }

    outResult = real_fn(queue, submitCount, cleanSubmits.data(), fenceToSubmit);
    if (outResult == VK_SUCCESS) {
        if (!emulatedSignals.empty()) {
            std::lock_guard<std::mutex> lock(m_semaphore_mutex);
            for (const auto& sig : emulatedSignals) {
                auto it = m_timeline_semaphores.find((uint64_t)(uintptr_t)sig.semaphore);
                if (it != m_timeline_semaphores.end()) {
                    if (fenceHolder && fenceHolder->fence != VK_NULL_HANDLE) {
                        it->second->pendingSignals.push_back({sig.targetValue, fenceHolder});
                    } else {
                        if (sig.targetValue > it->second->counter.load()) {
                            it->second->counter.store(sig.targetValue);
                            it->second->cv.notify_all();
                        }
                    }
                }
            }
        }
    }
    return true;
}

// ============================================================================
// Host Query Reset
// ============================================================================

bool Vulkan12Module::on_reset_query_pool(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount
) {
    if (is_device_native(device)) return false;

    PFN_vkResetQueryPool real_fn =
        (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPool");
    if (!real_fn) {
        real_fn = (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPoolEXT");
    }
    if (real_fn) {
        real_fn(device, queryPool, firstQuery, queryCount);
        return true;
    }

    // Emulate via short-lived command buffer
    PFN_vkCreateCommandPool real_cp = (PFN_vkCreateCommandPool) get_real_proc(get_last_instance(), device, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool real_dp = (PFN_vkDestroyCommandPool) get_real_proc(get_last_instance(), device, "vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers real_ac = (PFN_vkAllocateCommandBuffers) get_real_proc(get_last_instance(), device, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer real_bc = (PFN_vkBeginCommandBuffer) get_real_proc(get_last_instance(), device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer real_ec = (PFN_vkEndCommandBuffer) get_real_proc(get_last_instance(), device, "vkEndCommandBuffer");
    PFN_vkCmdResetQueryPool real_cmd_reset = (PFN_vkCmdResetQueryPool) get_real_proc(get_last_instance(), device, "vkCmdResetQueryPool");
    PFN_vkQueueSubmit real_qs = (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueSubmit");
    PFN_vkQueueWaitIdle real_qwi = (PFN_vkQueueWaitIdle) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueWaitIdle");

    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    if (!LayerManager::get().get_device_queue_info(device, queue, queueFamilyIndex)) {
        PFN_vkGetDeviceQueue real_gdq = (PFN_vkGetDeviceQueue) get_real_proc(get_last_instance(), device, "vkGetDeviceQueue");
        if (real_gdq) real_gdq(device, 0, 0, &queue);
    }

    if (real_cp && real_dp && real_ac && real_bc && real_ec && real_cmd_reset && real_qs && queue) {
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo poolCI{};
        poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolCI.queueFamilyIndex = queueFamilyIndex;

        if (real_cp(device, &poolCI, nullptr, &cmdPool) == VK_SUCCESS) {
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = cmdPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;

            VkCommandBuffer cmd = VK_NULL_HANDLE;
            if (real_ac(device, &allocInfo, &cmd) == VK_SUCCESS) {
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

                real_bc(cmd, &beginInfo);
                real_cmd_reset(cmd, queryPool, firstQuery, queryCount);
                real_ec(cmd);

                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &cmd;

                real_qs(queue, 1, &submitInfo, VK_NULL_HANDLE);
                if (real_qwi) real_qwi(queue);
            }
            real_dp(device, cmdPool, nullptr);
        }
    }
    return true;
}

// ============================================================================
// RenderPass2
// ============================================================================

bool Vulkan12Module::on_create_render_pass2(
    VkDevice device,
    const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    PFN_vkCreateRenderPass2KHR real_fn =
        (PFN_vkCreateRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCreateRenderPass2KHR");
    if (!real_fn) {
        real_fn = (PFN_vkCreateRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCreateRenderPass2");
    }
    if (real_fn) {
        outResult = real_fn(device, pCreateInfo, pAllocator, pRenderPass);
        return true;
    }

    if (!pCreateInfo) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    // Convert VkRenderPassCreateInfo2 to Vulkan 1.0 VkRenderPassCreateInfo
    std::vector<VkAttachmentDescription> attachments(pCreateInfo->attachmentCount);
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
        const auto& src = pCreateInfo->pAttachments[i];
        auto& dst = attachments[i];
        dst.flags = src.flags;
        dst.format = src.format;
        dst.samples = src.samples;
        dst.loadOp = src.loadOp;
        dst.storeOp = src.storeOp;
        dst.stencilLoadOp = src.stencilLoadOp;
        dst.stencilStoreOp = src.stencilStoreOp;
        dst.initialLayout = sanitize_layout(src.initialLayout);
        dst.finalLayout = sanitize_layout(src.finalLayout);
    }

    std::vector<std::vector<VkAttachmentReference>> inputRefs(pCreateInfo->subpassCount);
    std::vector<std::vector<VkAttachmentReference>> colorRefs(pCreateInfo->subpassCount);
    std::vector<std::vector<VkAttachmentReference>> resolveRefs(pCreateInfo->subpassCount);
    std::vector<VkAttachmentReference> dsRefs(pCreateInfo->subpassCount);
    std::vector<bool> hasDs(pCreateInfo->subpassCount, false);

    std::vector<VkSubpassDescription> subpasses(pCreateInfo->subpassCount);
    for (uint32_t s = 0; s < pCreateInfo->subpassCount; ++s) {
        const auto& src = pCreateInfo->pSubpasses[s];
        auto& dst = subpasses[s];
        dst.flags = src.flags;
        dst.pipelineBindPoint = src.pipelineBindPoint;

        if (src.inputAttachmentCount > 0 && src.pInputAttachments) {
            inputRefs[s].resize(src.inputAttachmentCount);
            for (uint32_t i = 0; i < src.inputAttachmentCount; ++i) {
                inputRefs[s][i].attachment = src.pInputAttachments[i].attachment;
                inputRefs[s][i].layout = sanitize_layout(src.pInputAttachments[i].layout);
            }
            dst.inputAttachmentCount = src.inputAttachmentCount;
            dst.pInputAttachments = inputRefs[s].data();
        }

        if (src.colorAttachmentCount > 0 && src.pColorAttachments) {
            colorRefs[s].resize(src.colorAttachmentCount);
            for (uint32_t i = 0; i < src.colorAttachmentCount; ++i) {
                colorRefs[s][i].attachment = src.pColorAttachments[i].attachment;
                colorRefs[s][i].layout = sanitize_layout(src.pColorAttachments[i].layout);
            }
            dst.colorAttachmentCount = src.colorAttachmentCount;
            dst.pColorAttachments = colorRefs[s].data();
        }

        if (src.pResolveAttachments) {
            resolveRefs[s].resize(src.colorAttachmentCount);
            for (uint32_t i = 0; i < src.colorAttachmentCount; ++i) {
                resolveRefs[s][i].attachment = src.pResolveAttachments[i].attachment;
                resolveRefs[s][i].layout = sanitize_layout(src.pResolveAttachments[i].layout);
            }
            dst.pResolveAttachments = resolveRefs[s].data();
        }

        if (src.pDepthStencilAttachment) {
            dsRefs[s].attachment = src.pDepthStencilAttachment->attachment;
            dsRefs[s].layout = sanitize_layout(src.pDepthStencilAttachment->layout);
            hasDs[s] = true;
            dst.pDepthStencilAttachment = &dsRefs[s];
        }

        dst.preserveAttachmentCount = src.preserveAttachmentCount;
        dst.pPreserveAttachments = src.pPreserveAttachments;
    }

    std::vector<VkSubpassDependency> dependencies(pCreateInfo->dependencyCount);
    for (uint32_t i = 0; i < pCreateInfo->dependencyCount; ++i) {
        const auto& src = pCreateInfo->pDependencies[i];
        auto& dst = dependencies[i];
        dst.srcSubpass = src.srcSubpass;
        dst.dstSubpass = src.dstSubpass;
        dst.srcStageMask = src.srcStageMask;
        dst.dstStageMask = src.dstStageMask;
        dst.srcAccessMask = src.srcAccessMask;
        dst.dstAccessMask = src.dstAccessMask;
        dst.dependencyFlags = src.dependencyFlags;
    }

    VkRenderPassCreateInfo ci1{};
    ci1.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci1.pNext = nullptr;
    ci1.flags = pCreateInfo->flags;
    ci1.attachmentCount = (uint32_t)attachments.size();
    ci1.pAttachments = attachments.empty() ? nullptr : attachments.data();
    ci1.subpassCount = (uint32_t)subpasses.size();
    ci1.pSubpasses = subpasses.empty() ? nullptr : subpasses.data();
    ci1.dependencyCount = (uint32_t)dependencies.size();
    ci1.pDependencies = dependencies.empty() ? nullptr : dependencies.data();

    PFN_vkCreateRenderPass real_crp =
        (PFN_vkCreateRenderPass) get_real_proc(get_last_instance(), device, "vkCreateRenderPass");
    if (real_crp) {
        outResult = real_crp(device, &ci1, pAllocator, pRenderPass);
    } else {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
    }
    return true;
}

bool Vulkan12Module::on_cmd_begin_render_pass2(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    PFN_vkCmdBeginRenderPass2KHR real_fn =
        (PFN_vkCmdBeginRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass2KHR");
    if (!real_fn) {
        real_fn = (PFN_vkCmdBeginRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass2");
    }
    if (real_fn) {
        real_fn(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
        return true;
    }

    PFN_vkCmdBeginRenderPass real_begin =
        (PFN_vkCmdBeginRenderPass) get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass");
    if (real_begin) {
        VkSubpassContents contents = pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE;
        real_begin(commandBuffer, pRenderPassBegin, contents);
    }
    return true;
}

bool Vulkan12Module::on_cmd_next_subpass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassBeginInfo* pSubpassBeginInfo,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    PFN_vkCmdNextSubpass2KHR real_fn =
        (PFN_vkCmdNextSubpass2KHR) get_real_proc(get_last_instance(), device, "vkCmdNextSubpass2KHR");
    if (!real_fn) {
        real_fn = (PFN_vkCmdNextSubpass2KHR) get_real_proc(get_last_instance(), device, "vkCmdNextSubpass2");
    }
    if (real_fn) {
        real_fn(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
        return true;
    }

    PFN_vkCmdNextSubpass real_next =
        (PFN_vkCmdNextSubpass) get_real_proc(get_last_instance(), device, "vkCmdNextSubpass");
    if (real_next) {
        VkSubpassContents contents = pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE;
        real_next(commandBuffer, contents);
    }
    return true;
}

bool Vulkan12Module::on_cmd_end_render_pass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    PFN_vkCmdEndRenderPass2KHR real_fn =
        (PFN_vkCmdEndRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCmdEndRenderPass2KHR");
    if (!real_fn) {
        real_fn = (PFN_vkCmdEndRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCmdEndRenderPass2");
    }
    if (real_fn) {
        real_fn(commandBuffer, pSubpassEndInfo);
        return true;
    }

    PFN_vkCmdEndRenderPass real_end =
        (PFN_vkCmdEndRenderPass) get_real_proc(get_last_instance(), device, "vkCmdEndRenderPass");
    if (real_end) {
        real_end(commandBuffer);
    }
    return true;
}

// ============================================================================
// Draw Indirect Count
// ============================================================================

bool Vulkan12Module::on_cmd_draw_indirect_count(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

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
        return true;
    }

    if (maxDrawCount == 0) return true;
    if (stride == 0) stride = sizeof(VkDrawIndirectCommand);

    PFN_vkCmdDrawIndirect real_draw =
        (PFN_vkCmdDrawIndirect) get_real_proc(get_last_instance(), device, "vkCmdDrawIndirect");
    if (real_draw) {
        real_draw(commandBuffer, buffer, offset, maxDrawCount, stride);
    }
    return true;
}

bool Vulkan12Module::on_cmd_draw_indexed_indirect_count(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

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
        return true;
    }

    if (maxDrawCount == 0) return true;
    if (stride == 0) stride = sizeof(VkDrawIndexedIndirectCommand);

    PFN_vkCmdDrawIndexedIndirect real_draw =
        (PFN_vkCmdDrawIndexedIndirect) get_real_proc(get_last_instance(), device, "vkCmdDrawIndexedIndirect");
    if (real_draw) {
        real_draw(commandBuffer, buffer, offset, maxDrawCount, stride);
    }
    return true;
}

// ============================================================================
// Buffer Device Address
// ============================================================================

bool Vulkan12Module::on_get_buffer_device_address(
    VkDevice device,
    const VkBufferDeviceAddressInfo* pInfo,
    VkDeviceAddress& outAddress
) {
    if (is_device_native(device)) return false;

    PFN_vkGetBufferDeviceAddressKHR real_fn =
        (PFN_vkGetBufferDeviceAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferDeviceAddressKHR");
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferDeviceAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferDeviceAddress");
    }
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferDeviceAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferDeviceAddressEXT");
    }
    if (real_fn) {
        outAddress = real_fn(device, pInfo);
        return true;
    }

    outAddress = pInfo ? (VkDeviceAddress)(uintptr_t)pInfo->buffer : 0;
    return true;
}

#include "vk_common.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "renderdoc_manager.h"
#include "shader/shader_rewriter.h"
#include "shader/subgroup_broadcast_lowering.h"
#include "shader/uniform_member_pruning.h"
#include "shader/sampler_alias_dedup.h"
#include "imageless_framebuffer_manager.h"
#include "layout_compat.h"
#include "vk_pnext.h"
#include "core/vertex_store_emulator.h"
#include "core/tbdr_barrier_optimizer.h"
#include "core/timeline_scheduler.h"
#include "core/extension_dependencies.h"
#include "core/native_instance_state.h"
#include "core/shader_module_cache.h"
#include "core/legacy_query_array.h"
#include <chrono>
#include <thread>
#include <string.h>
#include <string>
#include <vector>
#include <atomic>

// Highest instance version the real loader accepts. Used to clamp the version
// requested by the application when the layer advertises Vulkan 1.2 on an older
// loader.
static uint32_t query_real_instance_version() {
    static std::atomic<uint32_t> s_version{0};
    const uint32_t cached = s_version.load(std::memory_order_relaxed);
    if (cached != 0) return cached;

    PFN_vkEnumerateInstanceVersion fn = (PFN_vkEnumerateInstanceVersion)
        get_real_proc(VK_NULL_HANDLE, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t version = VK_API_VERSION_1_0;
    if (fn != nullptr) {
        uint32_t reported = 0;
        if (fn(&reported) == VK_SUCCESS && reported != 0) {
            version = reported;
        }
    }
    s_version.store(version, std::memory_order_relaxed);
    return version;
}

__attribute__((constructor))
static void init_vulkan_layer() {
    LOGI("Initializing Vulkan Layer (Modular Extension & Feature Supplement)...");
    init_real_vulkan();

    auto& manager = LayerManager::get();
    manager.init_registered_modules();

    register_vulkan_ptr();
    LOGI("Vulkan Layer initialization completed successfully");
}

extern "C" {

// ============================================================================
// Core Intercepted Entry Points
// ============================================================================

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance
) {
    init_real_vulkan();
    PFN_vkCreateInstance real_fn =
        (PFN_vkCreateInstance) get_real_proc(VK_NULL_HANDLE, VK_NULL_HANDLE, "vkCreateInstance");
    if (!real_fn) {
        LOGE("vkCreateInstance: driver symbol not found!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!pCreateInfo) {
        return real_fn(pCreateInfo, pAllocator, pInstance);
    }

    RenderDocManager::get().init();

    // Check natively supported instance extensions
    PFN_vkEnumerateInstanceExtensionProperties real_enum =
        (PFN_vkEnumerateInstanceExtensionProperties) get_real_proc(VK_NULL_HANDLE, VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    std::vector<std::string> nativeExts;
    auto gatherInstanceExtensions = [&](const char *layer) {
        if (!real_enum) return VK_ERROR_INITIALIZATION_FAILED;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            uint32_t count = 0;
            auto status = real_enum(layer, &count, nullptr);
            if (status != VK_SUCCESS) return status;
            std::vector<VkExtensionProperties> values(count);
            status = count ? real_enum(layer, &count, values.data()) : VK_SUCCESS;
            if (status == VK_INCOMPLETE) continue;
            if (status != VK_SUCCESS) return status;
            for (uint32_t i = 0; i < count; ++i) nativeExts.emplace_back(values[i].extensionName);
            return VK_SUCCESS;
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    VkResult enumerationResult = gatherInstanceExtensions(nullptr);

    // Also include extensions provided by enabled layers (e.g. RenderDoc / validation layers)
    if (pCreateInfo->enabledLayerCount > 0 && pCreateInfo->ppEnabledLayerNames) {
        for (uint32_t l = 0; enumerationResult == VK_SUCCESS && l < pCreateInfo->enabledLayerCount; ++l) {
            const char* layerName = pCreateInfo->ppEnabledLayerNames[l];
            if (layerName) enumerationResult = gatherInstanceExtensions(layerName);
        }
    }
    if (enumerationResult != VK_SUCCESS) {
        if (pInstance) *pInstance = VK_NULL_HANDLE;
        return enumerationResult;
    }

    // Always preserve debug utils and debug report if RenderDoc is active or requested
    if (RenderDocManager::get().is_available()) {
        nativeExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        nativeExts.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    }

    VkInstanceCreateInfo modCI = *pCreateInfo;
    std::vector<const char*> enabledExts;
    if (pCreateInfo->enabledExtensionCount > 0 && pCreateInfo->ppEnabledExtensionNames) {
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
            const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
            bool isNative = false;
            for (const auto& ne : nativeExts) {
                if (ne == ext) { isNative = true; break; }
            }
            if (isNative) {
                enabledExts.push_back(ext);
            } else {
                LOGI("vkCreateInstance: stripped unsupported instance extension: %s", ext);
            }
        }
        modCI.enabledExtensionCount = (uint32_t)enabledExts.size();
        modCI.ppEnabledExtensionNames = enabledExts.empty() ? nullptr : enabledExts.data();
    }

    // If we advertised 1.2 to the application but the loader is older, create
    // the instance with the loader's real version; all 1.2 entry points are
    // served by the layer and its emulation modules.
    VkApplicationInfo appInfoCopy{};
    if (modCI.pApplicationInfo != nullptr) {
        const uint32_t realVersion = query_real_instance_version();
        // Vulkan 1.1+ loaders must accept higher application versions. Keep
        // that target so a newer physical device can still expose its API.
        if (realVersion < VK_API_VERSION_1_1 && modCI.pApplicationInfo->apiVersion > realVersion) {
            appInfoCopy = *modCI.pApplicationInfo;
            appInfoCopy.apiVersion = realVersion;
            modCI.pApplicationInfo = &appInfoCopy;
            LOGI("vkCreateInstance: clamped apiVersion 0x%x -> real loader 0x%x",
                 pCreateInfo->pApplicationInfo->apiVersion, realVersion);
        }
    }

    vkfix::ExtensionEnvironment environment;
    environment.instance_creation = true;
    const uint32_t targetApi = modCI.pApplicationInfo && modCI.pApplicationInfo->apiVersion
                                   ? modCI.pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    const uint32_t publicApi = pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion
                                   ? pCreateInfo->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    environment.instance_api = std::min(targetApi, query_real_instance_version());
    environment.available.insert(nativeExts.begin(), nativeExts.end());
    // Core 1.1 queries can require instance extensions on the 1.0 loader.
    // They must be enabled now, not first discovered during vkCreateDevice.
    auto promotedInstances = vkfix::promoted_instance_extensions(publicApi, environment);
    enabledExts.insert(enabledExts.end(), promotedInstances.begin(), promotedInstances.end());
    auto dependencyPlan = vkfix::close_extension_dependencies(enabledExts, environment);
    if (dependencyPlan.result != VK_SUCCESS) {
        LOGE("vkCreateInstance: missing native dependency: %s", dependencyPlan.missing.c_str());
        if (pInstance) *pInstance = VK_NULL_HANDLE;
        return dependencyPlan.result;
    }
    modCI.enabledExtensionCount = uint32_t(dependencyPlan.names.size());
    modCI.ppEnabledExtensionNames = dependencyPlan.names.empty() ? nullptr : dependencyPlan.names.data();
    VkResult res = real_fn(&modCI, pAllocator, pInstance);
    if (res == VK_SUCCESS && pInstance && *pInstance != VK_NULL_HANDLE) {
        std::vector<const char *> enabledLayers;
        if (pCreateInfo->enabledLayerCount && pCreateInfo->ppEnabledLayerNames)
            enabledLayers.assign(pCreateInfo->ppEnabledLayerNames,
                                 pCreateInfo->ppEnabledLayerNames + pCreateInfo->enabledLayerCount);
        vkfix::remember_native_instance(*pInstance, targetApi, query_real_instance_version(), dependencyPlan.names, enabledLayers, publicApi);
        set_last_instance(*pInstance);
        LOGI("Created VkInstance %p", *pInstance);
    }
    return res;
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator
) {
    RenderDocManager::get().on_instance_destroyed(instance);
    PFN_vkDestroyInstance real_fn =
        (PFN_vkDestroyInstance) get_real_proc(instance, VK_NULL_HANDLE, "vkDestroyInstance");
    if (real_fn) {
        real_fn(instance, pAllocator);
    }
    vkfix::forget_native_instance(instance);
    if (get_last_instance() == instance) {
        set_last_instance(VK_NULL_HANDLE);
    }
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties
) {
    return LayerManager::get().dispatch_enumerate_device_extensions(
        physicalDevice, pLayerName, pPropertyCount, pProperties);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures
) {
    LayerManager::get().dispatch_get_physical_device_features(physicalDevice, pFeatures);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures
) {
    LayerManager::get().dispatch_get_physical_device_features2(physicalDevice, pFeatures);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures
) {
    LayerManager::get().dispatch_get_physical_device_features2(physicalDevice, pFeatures);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties
) {
    LayerManager::get().dispatch_get_physical_device_properties2(physicalDevice, pProperties);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties
) {
    LayerManager::get().dispatch_get_physical_device_properties2(physicalDevice, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice
) {
    return LayerManager::get().dispatch_create_device(
        physicalDevice, pCreateInfo, pAllocator, pDevice);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_device(device, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateGraphicsPipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines
) {
    return LayerManager::get().dispatch_create_graphics_pipelines(
        device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateComputePipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines
) {
    return LayerManager::get().dispatch_create_compute_pipelines(
        device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreatePipelineCache(
    VkDevice device,
    const VkPipelineCacheCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineCache* pPipelineCache
) {
    return LayerManager::get().dispatch_create_pipeline_cache(
        device, pCreateInfo, pAllocator, pPipelineCache);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyPipelineCache(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_pipeline_cache(
        device, pipelineCache, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetPipelineCacheData(
    VkDevice device,
    VkPipelineCache pipelineCache,
    size_t* pDataSize,
    void* pData
) {
    return LayerManager::get().dispatch_get_pipeline_cache_data(
        device, pipelineCache, pDataSize, pData);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkMergePipelineCaches(
    VkDevice device,
    VkPipelineCache dstCache,
    uint32_t srcCacheCount,
    const VkPipelineCache* pSrcCaches
) {
    return LayerManager::get().dispatch_merge_pipeline_caches(
        device, dstCache, srcCacheCount, pSrcCaches);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout
) {
    return LayerManager::get().dispatch_create_descriptor_set_layout(
        device, pCreateInfo, pAllocator, pSetLayout);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyDescriptorSetLayout(
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_descriptor_set_layout(
        device, descriptorSetLayout, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreatePipelineLayout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineLayout* pPipelineLayout
) {
    return LayerManager::get().dispatch_create_pipeline_layout(
        device, pCreateInfo, pAllocator, pPipelineLayout);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyPipelineLayout(
    VkDevice device,
    VkPipelineLayout pipelineLayout,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_pipeline_layout(
        device, pipelineLayout, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers
) {
    return LayerManager::get().dispatch_allocate_command_buffers(
        device, pAllocateInfo, pCommandBuffers);
}

VK_LAYER_EXPORT void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice device,
    VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers
) {
    LayerManager::get().dispatch_free_command_buffers(
        device, commandPool, commandBufferCount, pCommandBuffers);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo
) {
    return LayerManager::get().dispatch_begin_command_buffer(
        commandBuffer, pBeginInfo);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkResetCommandBuffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags
) {
    return LayerManager::get().dispatch_reset_command_buffer(
        commandBuffer, flags);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate
) {
    return LayerManager::get().dispatch_create_descriptor_update_template(
        device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplateKHR(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate
) {
    return LayerManager::get().dispatch_create_descriptor_update_template(
        device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_descriptor_update_template(
        device, descriptorUpdateTemplate, pAllocator);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyDescriptorUpdateTemplateKHR(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_descriptor_update_template(
        device, descriptorUpdateTemplate, pAllocator);
}

VK_LAYER_EXPORT void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const void* pData
) {
    LayerManager::get().dispatch_update_descriptor_set_with_template(
        device, descriptorSet, descriptorUpdateTemplate, pData);
}

VK_LAYER_EXPORT void VKAPI_CALL vkUpdateDescriptorSetWithTemplateKHR(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const void* pData
) {
    LayerManager::get().dispatch_update_descriptor_set_with_template(
        device, descriptorSet, descriptorUpdateTemplate, pData);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdPushDescriptorSetKHR(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t set,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites
) {
    LayerManager::get().dispatch_cmd_push_descriptor_set(
        commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdPushDescriptorSetWithTemplateKHR(
    VkCommandBuffer commandBuffer,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    VkPipelineLayout layout,
    uint32_t set,
    const void* pData
) {
    LayerManager::get().dispatch_cmd_push_descriptor_set_with_template(
        commandBuffer, descriptorUpdateTemplate, layout, set, pData);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage
) {
    return LayerManager::get().dispatch_create_image(device, pCreateInfo, pAllocator, pImage);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_image(device, image, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView
) {
    return LayerManager::get().dispatch_create_image_view(device, pCreateInfo, pAllocator, pView);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyImageView(
    VkDevice device,
    VkImageView imageView,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_image_view(device, imageView, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler
) {
    return LayerManager::get().dispatch_create_sampler(device, pCreateInfo, pAllocator, pSampler);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroySampler(
    VkDevice device,
    VkSampler sampler,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_sampler(device, sampler, pAllocator);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBeginRendering(
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo* pRenderingInfo
) {
    LayerManager::get().dispatch_cmd_begin_rendering(commandBuffer, pRenderingInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBeginRenderingKHR(
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo* pRenderingInfo
) {
    LayerManager::get().dispatch_cmd_begin_rendering(commandBuffer, pRenderingInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdEndRendering(
    VkCommandBuffer commandBuffer
) {
    LayerManager::get().dispatch_cmd_end_rendering(commandBuffer);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdEndRenderingKHR(
    VkCommandBuffer commandBuffer
) {
    LayerManager::get().dispatch_cmd_end_rendering(commandBuffer);
}

// ============================================================================
// VK_EXT_debug_utils Entry Points (RenderDoc & Tooling Markers)
// ============================================================================

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBeginDebugUtilsLabelEXT(
    VkCommandBuffer commandBuffer,
    const VkDebugUtilsLabelEXT* pLabelInfo
) {
    LayerManager::get().dispatch_cmd_begin_debug_utils_label(commandBuffer, pLabelInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdEndDebugUtilsLabelEXT(
    VkCommandBuffer commandBuffer
) {
    LayerManager::get().dispatch_cmd_end_debug_utils_label(commandBuffer);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdInsertDebugUtilsLabelEXT(
    VkCommandBuffer commandBuffer,
    const VkDebugUtilsLabelEXT* pLabelInfo
) {
    LayerManager::get().dispatch_cmd_insert_debug_utils_label(commandBuffer, pLabelInfo);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkSetDebugUtilsObjectNameEXT(
    VkDevice device,
    const VkDebugUtilsObjectNameInfoEXT* pNameInfo
) {
    return LayerManager::get().dispatch_set_debug_utils_object_name(device, pNameInfo);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkSetDebugUtilsObjectTagEXT(
    VkDevice device,
    const VkDebugUtilsObjectTagInfoEXT* pTagInfo
) {
    return LayerManager::get().dispatch_set_debug_utils_object_tag(device, pTagInfo);
}

// ============================================================================
// RenderDoc External In-App Capture Triggers (C API)
// ============================================================================

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vulkan_fix_trigger_capture(uint32_t num_frames) {
    RenderDocManager::get().trigger_capture(num_frames);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vulkan_fix_start_capture(VkDevice device) {
    RenderDocManager::get().start_capture(device);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vulkan_fix_end_capture(VkDevice device) {
    RenderDocManager::get().end_capture(device);
}

VK_LAYER_EXPORT VKAPI_ATTR uint32_t VKAPI_CALL vulkan_fix_is_renderdoc_attached() {
    return RenderDocManager::get().is_available() ? 1 : 0;
}
VK_LAYER_EXPORT uint32_t VKAPI_CALL vulkan_fix_start_capture_checked(VkDevice device) {
    return RenderDocManager::get().start_capture(device) ? 1 : 0;
}
VK_LAYER_EXPORT uint32_t VKAPI_CALL vulkan_fix_end_capture_checked(VkDevice device) {
    return RenderDocManager::get().end_capture(device) ? 1 : 0;
}

// ============================================================================
// Dispatchers: vkGetInstanceProcAddr & vkGetDeviceProcAddr
// ============================================================================

static inline PFN_vkVoidFunction lookup_layer_proc(const char* pName, bool is_device_only) {
    if (!pName) return NULL;

    #define MATCH(name) if (strcmp(pName, #name) == 0) return (PFN_vkVoidFunction) (void*) name

    if (strncmp(pName, "vulkan_fix_", 11) == 0) {
        MATCH(vulkan_fix_trigger_capture);
        MATCH(vulkan_fix_start_capture);
        MATCH(vulkan_fix_end_capture);
        MATCH(vulkan_fix_is_renderdoc_attached);
        MATCH(vulkan_fix_start_capture_checked);
        MATCH(vulkan_fix_end_capture_checked);
        return NULL;
    }

    if (pName[0] != 'v' || pName[1] != 'k') return NULL;

    switch (pName[2]) {
    case 'C': {
        if (pName[3] == 'm' && pName[4] == 'd') { // vkCmd... (Highest frequency hot path)
            MATCH(vkCmdBeginDebugUtilsLabelEXT);
            MATCH(vkCmdEndDebugUtilsLabelEXT);
            MATCH(vkCmdInsertDebugUtilsLabelEXT);
            MATCH(vkCmdDrawIndexed);
            MATCH(vkCmdDraw);
            MATCH(vkCmdBindPipeline);
            MATCH(vkCmdBindDescriptorSets);
            MATCH(vkCmdBindDescriptorSets2);
            MATCH(vkCmdBindDescriptorSets2KHR);
            MATCH(vkCmdPushConstants2);
            MATCH(vkCmdPushConstants2KHR);
            MATCH(vkCmdBindIndexBuffer2);
            MATCH(vkCmdBindIndexBuffer2KHR);
            MATCH(vkCmdBeginConditionalRenderingEXT);
            MATCH(vkCmdEndConditionalRenderingEXT);
            MATCH(vkCmdDrawMultiEXT);
            MATCH(vkCmdDrawMultiIndexedEXT);
            MATCH(vkCmdBindIndexBuffer);
            MATCH(vkCmdPushConstants);
            MATCH(vkCmdSetViewport);
            MATCH(vkCmdBeginQuery);
            MATCH(vkCmdEndQuery);
            MATCH(vkCmdBeginQueryIndexedEXT);
            MATCH(vkCmdEndQueryIndexedEXT);
            MATCH(vkCmdBeginTransformFeedbackEXT);
            MATCH(vkCmdEndTransformFeedbackEXT);
            MATCH(vkCmdBindVertexBuffers);
            MATCH(vkCmdBindVertexBuffers2);
            MATCH(vkCmdBindVertexBuffers2EXT);
            MATCH(vkCmdPushDescriptorSetKHR);
            MATCH(vkCmdPushDescriptorSetWithTemplateKHR);
            MATCH(vkCmdBeginRendering);
            MATCH(vkCmdBeginRenderingKHR);
            MATCH(vkCmdEndRendering);
            MATCH(vkCmdEndRenderingKHR);
            MATCH(vkCmdPipelineBarrier);
            MATCH(vkCmdPipelineBarrier2);
            MATCH(vkCmdPipelineBarrier2KHR);
            MATCH(vkCmdDrawIndexedIndirect);
            MATCH(vkCmdDrawIndirect);
            MATCH(vkCmdDrawIndexedIndirectCount);
            MATCH(vkCmdDrawIndexedIndirectCountKHR);
            MATCH(vkCmdDrawIndexedIndirectCountAMD);
            MATCH(vkCmdDrawIndirectCount);
            MATCH(vkCmdDrawIndirectCountKHR);
            MATCH(vkCmdDrawIndirectCountAMD);
            MATCH(vkCmdSetEvent2);
            MATCH(vkCmdSetEvent2KHR);
            MATCH(vkCmdResetEvent2);
            MATCH(vkCmdResetEvent2KHR);
            MATCH(vkCmdWaitEvents2);
            MATCH(vkCmdWaitEvents2KHR);
            MATCH(vkCmdWriteTimestamp2);
            MATCH(vkCmdWriteTimestamp2KHR);
            MATCH(vkCmdSetEvent);
            MATCH(vkCmdResetEvent);
            MATCH(vkCmdWaitEvents);
            MATCH(vkCmdWriteTimestamp);
            MATCH(vkCmdResetQueryPool);
            MATCH(vkCmdBeginRenderPass2);
            MATCH(vkCmdBeginRenderPass2KHR);
            MATCH(vkCmdNextSubpass2);
            MATCH(vkCmdNextSubpass2KHR);
            MATCH(vkCmdEndRenderPass2);
            MATCH(vkCmdEndRenderPass2KHR);
            MATCH(vkCmdDispatchBase);
            MATCH(vkCmdDispatchBaseKHR);
            MATCH(vkCmdSetDeviceMask);
            MATCH(vkCmdSetDeviceMaskKHR);
            MATCH(vkCmdBeginRenderPass);
            MATCH(vkCmdNextSubpass);
            MATCH(vkCmdEndRenderPass);
            MATCH(vkCmdDispatch);
            MATCH(vkCmdDispatchIndirect);
            MATCH(vkCmdCopyBuffer);
            MATCH(vkCmdCopyImage);
            MATCH(vkCmdCopyBufferToImage);
            MATCH(vkCmdCopyImageToBuffer);
            MATCH(vkCmdFillBuffer);
            MATCH(vkCmdUpdateBuffer);
            MATCH(vkCmdResolveImage);
            MATCH(vkCmdCopyQueryPoolResults);
            MATCH(vkCmdBlitImage);
            MATCH(vkCmdClearAttachments);
            MATCH(vkCmdClearColorImage);
            MATCH(vkCmdClearDepthStencilImage);
            MATCH(vkCmdExecuteCommands);
            return NULL;
        }
        // vkCreate...
        if (!is_device_only) {
            MATCH(vkCreateInstance);
            MATCH(vkCreateDevice);
            MATCH(vkCreateAndroidSurfaceKHR);
            MATCH(vkCreateDebugUtilsMessengerEXT);
        }
        MATCH(vkCreateGraphicsPipelines);
        MATCH(vkCreateBuffer);
        MATCH(vkCreateCommandPool);
        MATCH(vkCreateComputePipelines);
        MATCH(vkCreatePipelineCache);
        MATCH(vkCreateDescriptorSetLayout);
        MATCH(vkCreatePipelineLayout);
        MATCH(vkCreateDescriptorUpdateTemplate);
        MATCH(vkCreateDescriptorUpdateTemplateKHR);
        MATCH(vkCreateImage);
        MATCH(vkCreateImageView);
        MATCH(vkCreateSampler);
        MATCH(vkCreateShaderModule);
        MATCH(vkCreateShadersEXT);
        MATCH(vkCreateFramebuffer);
        MATCH(vkCreateSemaphore);
        MATCH(vkCreateRenderPass2);
        MATCH(vkCreateRenderPass);
        MATCH(vkCreateRenderPass2KHR);
        MATCH(vkCreateSamplerYcbcrConversion);
        MATCH(vkCreateSamplerYcbcrConversionKHR);
        MATCH(vkCreateSwapchainKHR);
        return NULL;
    }
    case 'D': {
        MATCH(vkDestroyPipeline);
        MATCH(vkDestroyPipelineCache);
        MATCH(vkDestroyDescriptorSetLayout);
        MATCH(vkDestroyPipelineLayout);
        MATCH(vkDestroyDescriptorUpdateTemplate);
        MATCH(vkDestroyDescriptorUpdateTemplateKHR);
        MATCH(vkDestroyImage);
        MATCH(vkDestroyImageView);
        MATCH(vkDestroySampler);
        MATCH(vkDestroyFramebuffer);
        MATCH(vkDestroyRenderPass);
        MATCH(vkDestroySemaphore);
        MATCH(vkDestroySamplerYcbcrConversion);
        MATCH(vkDestroySamplerYcbcrConversionKHR);
        MATCH(vkDestroySwapchainKHR);
        MATCH(vkDestroyDevice);
        MATCH(vkDestroyShaderModule);
        MATCH(vkDestroyBuffer);
        MATCH(vkDestroyCommandPool);
        if (!is_device_only) {
            MATCH(vkDestroyInstance);
            MATCH(vkDestroySurfaceKHR);
            MATCH(vkDestroyDebugUtilsMessengerEXT);
        }
        MATCH(vkDeviceWaitIdle);
        return NULL;
    }
    case 'G': {
        MATCH(vkGetDeviceProcAddr);
        if (!is_device_only) {
            MATCH(vkGetInstanceProcAddr);
            MATCH(vkGetPhysicalDeviceFeatures);
            MATCH(vkGetPhysicalDeviceFeatures2);
            MATCH(vkGetPhysicalDeviceFeatures2KHR);
            MATCH(vkGetPhysicalDeviceProperties);
            MATCH(vkGetPhysicalDeviceProperties2);
            MATCH(vkGetPhysicalDeviceProperties2KHR);
            MATCH(vkGetPhysicalDeviceSurfaceSupportKHR);
            MATCH(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
            MATCH(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
            MATCH(vkGetPhysicalDeviceSurfaceFormatsKHR);
            MATCH(vkGetPhysicalDeviceSurfaceFormats2KHR);
            MATCH(vkGetPhysicalDeviceSurfacePresentModesKHR);
        }
        MATCH(vkGetDeviceQueue);
        MATCH(vkGetFenceStatus);
        MATCH(vkGetDeviceQueue2);
        MATCH(vkGetSemaphoreCounterValue);
        MATCH(vkGetSemaphoreCounterValueKHR);
        MATCH(vkGetBufferDeviceAddress);
        MATCH(vkGetBufferDeviceAddressKHR);
        MATCH(vkGetBufferDeviceAddressEXT);
        MATCH(vkGetBufferOpaqueCaptureAddress);
        MATCH(vkGetBufferOpaqueCaptureAddressKHR);
        MATCH(vkGetDeviceMemoryOpaqueCaptureAddress);
        MATCH(vkGetDeviceMemoryOpaqueCaptureAddressKHR);
        MATCH(vkGetBufferMemoryRequirements2);
        MATCH(vkGetBufferMemoryRequirements2KHR);
        MATCH(vkGetImageMemoryRequirements2);
        MATCH(vkGetImageMemoryRequirements2KHR);
        MATCH(vkGetImageSparseMemoryRequirements2);
        MATCH(vkGetImageSparseMemoryRequirements2KHR);
        MATCH(vkGetDescriptorSetLayoutSupport);
        MATCH(vkGetDescriptorSetLayoutSupportKHR);
        MATCH(vkGetDeviceGroupPeerMemoryFeatures);
        MATCH(vkGetDeviceGroupPeerMemoryFeaturesKHR);
        MATCH(vkGetDeviceGroupPresentCapabilitiesKHR);
        MATCH(vkGetDeviceGroupSurfacePresentModesKHR);
        MATCH(vkGetPhysicalDeviceFormatProperties2);
        MATCH(vkGetPhysicalDeviceFormatProperties2KHR);
        MATCH(vkGetPhysicalDeviceImageFormatProperties2);
        MATCH(vkGetPhysicalDeviceImageFormatProperties2KHR);
        MATCH(vkGetPhysicalDeviceQueueFamilyProperties2);
        MATCH(vkGetPhysicalDeviceQueueFamilyProperties2KHR);
        MATCH(vkGetPhysicalDeviceMemoryProperties2);
        MATCH(vkGetPhysicalDeviceMemoryProperties2KHR);
        MATCH(vkGetPhysicalDeviceSparseImageFormatProperties2);
        MATCH(vkGetPhysicalDeviceSparseImageFormatProperties2KHR);
        MATCH(vkGetPhysicalDeviceExternalBufferProperties);
        MATCH(vkGetPhysicalDeviceExternalBufferPropertiesKHR);
        MATCH(vkGetPhysicalDeviceExternalFenceProperties);
        MATCH(vkGetPhysicalDeviceExternalFencePropertiesKHR);
        MATCH(vkGetPhysicalDeviceExternalSemaphoreProperties);
        MATCH(vkGetPhysicalDeviceExternalSemaphorePropertiesKHR);
        MATCH(vkGetSwapchainImagesKHR);
        MATCH(vkGetPipelineCacheData);
        return NULL;
    }
    case 'M': {
        MATCH(vkMergePipelineCaches);
        return NULL;
    }
    case 'Q': {
        MATCH(vkQueueSubmit);
        MATCH(vkQueueBindSparse);
        MATCH(vkQueueSubmit2);
        MATCH(vkQueueSubmit2KHR);
        MATCH(vkQueueWaitIdle);
        MATCH(vkQueuePresentKHR);
        return NULL;
    }
    case 'A': {
        MATCH(vkAllocateCommandBuffers);
        MATCH(vkAcquireNextImageKHR);
        MATCH(vkAcquireNextImage2KHR);
        return NULL;
    }
    case 'B': {
        MATCH(vkBeginCommandBuffer);
        MATCH(vkBindBufferMemory2);
        MATCH(vkBindBufferMemory2KHR);
        MATCH(vkBindImageMemory2);
        MATCH(vkBindImageMemory2KHR);
        return NULL;
    }
    case 'F': {
        MATCH(vkFreeCommandBuffers);
        return NULL;
    }
    case 'R': {
        MATCH(vkResetCommandBuffer);
        MATCH(vkResetCommandPool);
        MATCH(vkResetQueryPool);
        MATCH(vkResetQueryPoolEXT);
        return NULL;
    }
    case 'S': {
        MATCH(vkSetDebugUtilsObjectNameEXT);
        MATCH(vkSetDebugUtilsObjectTagEXT);
        if (!is_device_only) {
            MATCH(vkSubmitDebugUtilsMessageEXT);
        }
        MATCH(vkSignalSemaphore);
        MATCH(vkSignalSemaphoreKHR);
        return NULL;
    }
    case 'W': {
        MATCH(vkWaitForFences);
        MATCH(vkWaitSemaphores);
        MATCH(vkWaitSemaphoresKHR);
        return NULL;
    }
    case 'U': {
        MATCH(vkUpdateDescriptorSetWithTemplate);
        MATCH(vkUpdateDescriptorSetWithTemplateKHR);
        return NULL;
    }
    case 'T': {
        MATCH(vkTrimCommandPool);
        MATCH(vkTrimCommandPoolKHR);
        return NULL;
    }
    case 'E': {
        MATCH(vkEndCommandBuffer);
        if (!is_device_only) {
            MATCH(vkEnumeratePhysicalDevices);
            MATCH(vkEnumerateDeviceExtensionProperties);
            MATCH(vkEnumerateInstanceVersion);
        }
        MATCH(vkEnumeratePhysicalDeviceGroups);
        MATCH(vkEnumeratePhysicalDeviceGroupsKHR);
        return NULL;
    }
    default:
        break;
    }

    #undef MATCH
    return NULL;
}

static PFN_vkVoidFunction track_native_command(const char *name, PFN_vkVoidFunction proc) {
    if (proc && strncmp(name, "vkCmd", 5) == 0 && TBDRBarrierOptimizer::get().is_enabled())
        TBDRBarrierOptimizer::get().on_untracked_command(name);
    return proc;
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                    const char *pName) {
    if (!pName)
        return NULL;

    PFN_vkVoidFunction custom_proc = LayerManager::get().get_custom_proc(pName);
    if (custom_proc)
        return track_native_command(pName, custom_proc);

    PFN_vkVoidFunction matched = lookup_layer_proc(pName, false);
    if (matched)
        return matched;

    init_real_vulkan();
    PFN_vkGetInstanceProcAddr real_gipa = get_real_instance_proc_addr();
    if (real_gipa) {
        return track_native_command(pName, real_gipa(instance, pName));
    }
    return NULL;
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                  const char *pName) {
    if (!pName)
        return NULL;

    PFN_vkVoidFunction custom_proc = LayerManager::get().get_custom_proc(pName);
    if (custom_proc)
        return track_native_command(pName, custom_proc);

    init_real_vulkan();
    PFN_vkGetDeviceProcAddr real_gdpa = get_real_device_proc_addr();

    // Native shortcuts must not hide actions while exact barrier dedup is active.
    if (strncmp(pName, "vkCmd", 5) == 0 && TBDRBarrierOptimizer::get().is_enabled()) {
        if (auto tracked = lookup_layer_proc(pName, true))
            return tracked;
    }

    // Breakthrough 1: Zero-Overhead Direct Bypass
    // If the device natively supports the requested feature and requires no layer emulation,
    // directly return the real driver's function pointer, completely bypassing the layer!
    if (device != VK_NULL_HANDLE &&
        !LayerManager::get().should_intercept_device_proc(device, pName)) {
        if (real_gdpa) {
            PFN_vkVoidFunction real_fn = real_gdpa(device, pName);
            if (real_fn)
                return track_native_command(pName, real_fn);
        }
    }

    PFN_vkVoidFunction matched = lookup_layer_proc(pName, true);
    if (matched)
        return matched;

    if (real_gdpa && device != VK_NULL_HANDLE) {
        return track_native_command(pName, real_gdpa(device, pName));
    }

    PFN_vkGetInstanceProcAddr real_gipa = get_real_instance_proc_addr();
    if (real_gipa) {
        return track_native_command(pName, real_gipa(get_last_instance(), pName));
    }
    return NULL;
}

// ============================================================================
// Standard Vulkan 1.0 Forwarding Stubs (with static lazy pointer caching)
// ============================================================================

// Base exports must share the provider that created their object handles.
#include "core/vulkan10_forwarders.inc"

#define FORWARD_INST(ret, name, args, params)                                                      \
    VK_LAYER_EXPORT ret VKAPI_CALL name args {                                                     \
        typedef ret(VKAPI_PTR *fn_t) args;                                                         \
        static fn_t s_cached_##name = nullptr;                                                     \
        if (__builtin_expect(!s_cached_##name, 0)) {                                               \
            s_cached_##name = (fn_t)get_real_proc(get_last_instance(), VK_NULL_HANDLE, #name);     \
        }                                                                                          \
        if (__builtin_expect(s_cached_##name != nullptr, 1))                                       \
            return s_cached_##name params;                                                         \
        return (ret)0;                                                                             \
    }

#define FORWARD_DEV(ret, name, dev_arg, args, params) \
    VK_LAYER_EXPORT ret VKAPI_CALL name args { \
        typedef ret (VKAPI_PTR *fn_t) args; \
        static fn_t s_cached_##name = nullptr; \
        if (__builtin_expect(!s_cached_##name, 0)) { \
            s_cached_##name = (fn_t) get_real_proc(get_last_instance(), dev_arg, #name); \
        } \
        if (__builtin_expect(s_cached_##name != nullptr, 1)) return s_cached_##name params; \
        return (ret)0; \
    }

#define FORWARD_DEV_VOID(name, dev_arg, args, params) \
    VK_LAYER_EXPORT void VKAPI_CALL name args { \
        typedef void (VKAPI_PTR *fn_t) args; \
        static fn_t s_cached_##name = nullptr; \
        if (__builtin_expect(!s_cached_##name, 0)) { \
            s_cached_##name = (fn_t) get_real_proc(get_last_instance(), dev_arg, #name); \
        } \
        if (__builtin_expect(s_cached_##name != nullptr, 1)) s_cached_##name params; \
    }

#define FORWARD_QUEUE(ret, name, args, params) \
    VK_LAYER_EXPORT ret VKAPI_CALL name args { \
        typedef ret (VKAPI_PTR *fn_t) args; \
        static fn_t s_cached_##name = nullptr; \
        if (__builtin_expect(!s_cached_##name, 0)) { \
            s_cached_##name = (fn_t) get_real_proc(get_last_instance(), VK_NULL_HANDLE, #name); \
        } \
        if (__builtin_expect(s_cached_##name != nullptr, 1)) return s_cached_##name params; \
        return (ret)0; \
    }

#define TRACK_CORE_COMMAND(name, args, call_args) \
    VK_LAYER_EXPORT void VKAPI_CALL name args { \
        TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer); \
        auto device = LayerManager::get().get_device_for_cmd(commandBuffer); \
        auto real = (PFN_##name)get_real_proc(get_last_instance(), device, #name); \
        if (real) real call_args; \
    }
TRACK_CORE_COMMAND(vkCmdSetEvent, (VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stages), (commandBuffer,event,stages))
TRACK_CORE_COMMAND(vkCmdResetEvent, (VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stages), (commandBuffer,event,stages))
TRACK_CORE_COMMAND(vkCmdWriteTimestamp, (VkCommandBuffer commandBuffer, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query), (commandBuffer,stage,pool,query))
TRACK_CORE_COMMAND(vkCmdResetQueryPool, (VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t first, uint32_t count), (commandBuffer,pool,first,count))
TRACK_CORE_COMMAND(vkCmdWaitEvents, (VkCommandBuffer commandBuffer, uint32_t count, const VkEvent* events, VkPipelineStageFlags src, VkPipelineStageFlags dst, uint32_t mc, const VkMemoryBarrier* memory, uint32_t bc, const VkBufferMemoryBarrier* buffers, uint32_t ic, const VkImageMemoryBarrier* images), (commandBuffer,count,events,src,dst,mc,memory,bc,buffers,ic,images))
#undef TRACK_CORE_COMMAND

VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t* pCount, VkPhysicalDevice* pDevices) {
    auto native = (PFN_vkEnumeratePhysicalDevices)get_real_proc(instance, VK_NULL_HANDLE, "vkEnumeratePhysicalDevices");
    if (!native) return VK_ERROR_INITIALIZATION_FAILED;
    auto result = native(instance, pCount, pDevices);
    if (pDevices && pCount && (result == VK_SUCCESS || result == VK_INCOMPLETE))
        vkfix::remember_physical_devices(instance, *pCount, pDevices);
    return result;
}
FORWARD_INST(VkResult, vkCreateDebugUtilsMessengerEXT, (VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pMessenger), (instance, pCreateInfo, pAllocator, pMessenger))
FORWARD_INST(void, vkDestroyDebugUtilsMessengerEXT, (VkInstance instance, VkDebugUtilsMessengerEXT messenger, const VkAllocationCallbacks* pAllocator), (instance, messenger, pAllocator))
FORWARD_INST(void, vkSubmitDebugUtilsMessageEXT, (VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData), (instance, messageSeverity, messageTypes, pCallbackData))

VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* pLayer,
    uint32_t* pCount,
    VkExtensionProperties* pProps
) {
    init_real_vulkan();
    PFN_vkEnumerateInstanceExtensionProperties real_fn =
        (PFN_vkEnumerateInstanceExtensionProperties) get_real_proc(VK_NULL_HANDLE, VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");

    std::vector<VkExtensionProperties> exts;
    if (real_fn) {
        uint32_t count = 0;
        if (real_fn(pLayer, &count, NULL) == VK_SUCCESS && count > 0) {
            exts.resize(count);
            real_fn(pLayer, &count, exts.data());
        }
    }

    auto add_ext = [&](const char* name, uint32_t spec) {
        for (const auto& e : exts) {
            if (strcmp(e.extensionName, name) == 0) return;
        }
        VkExtensionProperties p{};
        strncpy(p.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE - 1);
        p.specVersion = spec;
        exts.push_back(p);
    };

    add_ext(VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION);
    add_ext(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_SPEC_VERSION);
    add_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_SPEC_VERSION);

    if (!pProps) {
        if (pCount) *pCount = (uint32_t)exts.size();
        return VK_SUCCESS;
    }
    if (!pCount) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t toCopy = std::min(*pCount, (uint32_t)exts.size());
    memcpy(pProps, exts.data(), toCopy * sizeof(VkExtensionProperties));
    *pCount = toCopy;
    return (toCopy < exts.size()) ? VK_INCOMPLETE : VK_SUCCESS;
}
FORWARD_INST(VkResult, vkEnumerateInstanceLayerProperties, (uint32_t* pCount, VkLayerProperties* pProps), (pCount, pProps))
VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumerateInstanceVersion(
    uint32_t* pApiVersion
) {
    if (!pApiVersion) return VK_ERROR_INITIALIZATION_FAILED;
    init_real_vulkan();
    PFN_vkEnumerateInstanceVersion real_fn =
        (PFN_vkEnumerateInstanceVersion) get_real_proc(VK_NULL_HANDLE, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (real_fn) {
        VkResult res = real_fn(pApiVersion);
        // The layer claims Vulkan 1.2 support (see claim_vulkan_1_2). The real
        // version is still used internally, e.g. as the SPIR-V downgrade target.
        if (res == VK_SUCCESS && claim_vulkan_1_2() && *pApiVersion < VK_API_VERSION_1_2) {
            *pApiVersion = VK_API_VERSION_1_2;
        }
        return res;
    }
    *pApiVersion = claim_vulkan_1_2() ? VK_API_VERSION_1_2 : VK_API_VERSION_1_0;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties
) {
    LayerManager::get().dispatch_get_physical_device_properties(physicalDevice, pProperties);
}
FORWARD_INST(void, vkGetPhysicalDeviceFormatProperties, (VkPhysicalDevice physDev, VkFormat format, VkFormatProperties* pProps), (physDev, format, pProps))
FORWARD_INST(VkResult, vkGetPhysicalDeviceImageFormatProperties, (VkPhysicalDevice physDev, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* pProps), (physDev, format, type, tiling, usage, flags, pProps))
FORWARD_INST(void, vkGetPhysicalDeviceQueueFamilyProperties, (VkPhysicalDevice physDev, uint32_t* pCount, VkQueueFamilyProperties* pProps), (physDev, pCount, pProps))
FORWARD_INST(void, vkGetPhysicalDeviceMemoryProperties, (VkPhysicalDevice physDev, VkPhysicalDeviceMemoryProperties* pProps), (physDev, pProps))
FORWARD_INST(VkResult, vkEnumerateDeviceLayerProperties, (VkPhysicalDevice physDev, uint32_t* pCount, VkLayerProperties* pProps), (physDev, pCount, pProps))

VK_LAYER_EXPORT void VKAPI_CALL vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue
) {
    LayerManager::get().dispatch_get_device_queue(device, queueFamilyIndex, queueIndex, pQueue);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device,
    const VkDeviceQueueInfo2* pQueueInfo,
    VkQueue* pQueue
) {
    LayerManager::get().dispatch_get_device_queue2(device, pQueueInfo, pQueue);
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence
) {
    return LayerManager::get().dispatch_queue_submit(queue, submitCount, pSubmits, fence);
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
    return LayerManager::get().dispatch_queue_wait_idle(queue);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
    return LayerManager::get().dispatch_device_wait_idle(device);
}
FORWARD_DEV(VkResult, vkAllocateMemory, device, (VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory), (device, pAllocateInfo, pAllocator, pMemory))
FORWARD_DEV_VOID(vkFreeMemory, device, (VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator), (device, memory, pAllocator))
VK_LAYER_EXPORT VkResult VKAPI_CALL vkQueueBindSparse(VkQueue queue, uint32_t count,
                                                      const VkBindSparseInfo *infos,
                                                      VkFence fence) {
    VkResult result = VK_SUCCESS;
    if (vkfix::TimelineScheduler::get().bind_sparse(queue, count, infos, fence, result))
        return result;
    auto device = LayerManager::get().get_device_for_queue(queue);
    auto native =
        (PFN_vkQueueBindSparse)get_real_proc(get_last_instance(), device, "vkQueueBindSparse");
    return native ? native(queue, count, infos, fence) : VK_ERROR_INITIALIZATION_FAILED;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence) {
    auto error = vkfix::TimelineScheduler::get().error(device);
    if (error != VK_SUCCESS)
        return error;
    auto native =
        (PFN_vkGetFenceStatus)get_real_proc(get_last_instance(), device, "vkGetFenceStatus");
    return native ? vkfix::TimelineScheduler::get().native_fence_call(
                        device, [&] { return native(device, fence); })
                  : VK_ERROR_INITIALIZATION_FAILED;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t count,
                                                    const VkFence *fences, VkBool32 all,
                                                    uint64_t timeout) {
    auto native =
        (PFN_vkWaitForFences)get_real_proc(get_last_instance(), device, "vkWaitForFences");
    if (!native)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto &scheduler = vkfix::TimelineScheduler::get();
    if (!scheduler.active(device))
        return native(device, count, fences, all, timeout);
    auto began = std::chrono::steady_clock::now();
    while (true) {
        auto error = scheduler.error(device);
        if (error != VK_SUCCESS)
            return error;
        // The worker can still be about to submit the application's fence.
        // Probe while excluding that native call, then sleep outside the lock
        // so an unsignaled fence cannot prevent its own submission.
        auto result = scheduler.native_fence_call(
            device, [&] { return native(device, count, fences, all, 0); });
        if (result != VK_TIMEOUT)
            return result;
        auto elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - began)
                               .count());
        if (timeout != UINT64_MAX && elapsed >= timeout)
            return VK_TIMEOUT;
        auto remaining = timeout == UINT64_MAX ? 1000000ULL : timeout - elapsed;
        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>(remaining, 1000000ULL)));
    }
}
FORWARD_DEV(VkResult, vkMapMemory, device,
            (VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
             VkMemoryMapFlags flags, void **ppData),
            (device, memory, offset, size, flags, ppData))
FORWARD_DEV_VOID(vkUnmapMemory, device, (VkDevice device, VkDeviceMemory memory), (device, memory))
VK_LAYER_EXPORT VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule) {
    auto real_fn = (PFN_vkCreateShaderModule)get_real_proc(get_last_instance(), device, "vkCreateShaderModule");
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;
    auto create = [&](VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* allocator, VkShaderModule* out) {
        VkResult result = real_fn(d, ci, allocator, out);
        if (result == VK_SUCCESS && out && *out) {
            VertexStoreEmulator::get().shader(d, *out, ci);
            vkfix::ShaderModuleCache::get().remember(d, *out, ci);
        }
        return result;
    };

    if (!pCreateInfo || !pCreateInfo->pCode ||
        pCreateInfo->codeSize < 5 * sizeof(uint32_t) ||
        (pCreateInfo->codeSize % sizeof(uint32_t)) != 0) {
        return create(device, pCreateInfo, pAllocator, pShaderModule);
    }

    const uint32_t targetVersion = LayerManager::get().get_device_max_spirv_version(device);
    if (targetVersion == 0) {
        return create(device, pCreateInfo, pAllocator, pShaderModule);
    }

    // Fast path: on a modern driver there is nothing to lower. Checking the
    // header avoids a heap allocation + memcpy for every shader module.
    const uint32_t* code_words = pCreateInfo->pCode;
    const auto broadcast_support = LayerManager::get().get_device_subgroup_broadcast(device);
    const bool prune_uniforms = LayerManager::get().should_prune_uniform_members(device) &&
        vkfix::has_large_uniform_block(code_words, pCreateInfo->codeSize / sizeof(uint32_t));
    const bool dedup_samplers = LayerManager::get().should_dedup_sampler_aliases(device) &&
        vkfix::has_sampler_aliases(code_words, pCreateInfo->codeSize / sizeof(uint32_t));
    if (code_words[0] != 0x07230203u ||
        (code_words[1] <= targetVersion && !broadcast_support && !prune_uniforms && !dedup_samplers)) {
        return create(device, pCreateInfo, pAllocator, pShaderModule);
    }

    std::vector<uint32_t> code(pCreateInfo->codeSize / sizeof(uint32_t));
    memcpy(code.data(), pCreateInfo->pCode, pCreateInfo->codeSize);

    bool broadcast_changed = false;
    if (auto support = broadcast_support) {
        std::string error;
        auto result = vkfix::lower_subgroup_broadcasts(code, support.max_size, support.shuffle, error);
        broadcast_changed = result == vkfix::BroadcastLowering::Changed;
        if (result == vkfix::BroadcastLowering::Invalid)
            LOGW("Subgroup broadcast lowering rejected: %s", error.c_str());
    }
    const bool version_changed = ShaderRewriter::get().rewrite(code, targetVersion);
    bool uniforms_changed = false;
    if (prune_uniforms) {
        std::string error;
        const auto result = vkfix::prune_unused_uniform_members(code, targetVersion, error);
        uniforms_changed = result == vkfix::UniformPruneResult::Changed;
        if (result == vkfix::UniformPruneResult::Invalid)
            LOGW("Measured uniform repair rejected: %s", error.c_str());
        if (uniforms_changed) {
            static std::atomic<uint64_t> pruned_modules{0};
            const auto count = pruned_modules.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 16 == 0)
                LOGI("Measured uniform repair: pruned unused UBO members in %llu shader module(s)",
                     static_cast<unsigned long long>(count));
        }
    }
    bool samplers_changed = false;
    if (dedup_samplers) {
        std::string error;
        const auto result = vkfix::dedup_sampler_aliases(code, targetVersion, error);
        samplers_changed = result == vkfix::SamplerAliasResult::Changed;
        if (samplers_changed)
            LOGI("Measured sampler repair: deduplicated shader sampler aliases");
    }
    if (!version_changed && !broadcast_changed && !uniforms_changed && !samplers_changed) {
        return create(device, pCreateInfo, pAllocator, pShaderModule);
    }

    VkShaderModuleCreateInfo modified = *pCreateInfo;
    modified.codeSize = code.size() * sizeof(uint32_t);
    modified.pCode = code.data();
    return create(device, &modified, pAllocator, pShaderModule);
}
// VK_KHR_separate_depth_stencil_layouts emulation: rewrite the separate
// depth/stencil layouts to the combined layouts understood by Vulkan 1.0.
VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateRenderPass(
    VkDevice device,
    const VkRenderPassCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass
) {
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkCreateRenderPass real_fn = dt.CreateRenderPass;
    if (!real_fn) {
        real_fn = (PFN_vkCreateRenderPass)
            get_real_proc(get_last_instance(), device, "vkCreateRenderPass");
    }
    if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

    auto create = [&](const VkRenderPassCreateInfo* info) {
        VkResult result = real_fn(device, info, pAllocator, pRenderPass);
        if (result == VK_SUCCESS && pRenderPass)
            LayerManager::get().track_render_pass(device, info, *pRenderPass);
        return result;
    };
    if (!pCreateInfo || !LayoutCompat::get().any_emulated()) {
        return create(pCreateInfo);
    }

    std::vector<VkAttachmentDescription> attachments(pCreateInfo->attachmentCount);
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
        attachments[i] = pCreateInfo->pAttachments[i];
        attachments[i].initialLayout = LayoutCompat::sanitize(attachments[i].initialLayout);
        attachments[i].finalLayout = LayoutCompat::sanitize(attachments[i].finalLayout);
    }

    std::vector<std::vector<VkAttachmentReference>> ref_storage(
        static_cast<size_t>(pCreateInfo->subpassCount) * 4);
    std::vector<VkSubpassDescription> subpasses(pCreateInfo->subpassCount);

    auto sanitize_refs = [&](uint32_t subpass, int slot,
                             const VkAttachmentReference* src,
                             uint32_t count) -> const VkAttachmentReference* {
        if (src == nullptr || count == 0) return src;
        auto& storage = ref_storage[static_cast<size_t>(subpass) * 4 + slot];
        storage.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            storage[i] = src[i];
            storage[i].layout = LayoutCompat::sanitize(storage[i].layout);
        }
        return storage.data();
    };

    for (uint32_t s = 0; s < pCreateInfo->subpassCount; ++s) {
        const VkSubpassDescription& in = pCreateInfo->pSubpasses[s];
        VkSubpassDescription& out = subpasses[s];
        out = in;
        out.pInputAttachments = sanitize_refs(s, 0, in.pInputAttachments, in.inputAttachmentCount);
        out.pColorAttachments = sanitize_refs(s, 1, in.pColorAttachments, in.colorAttachmentCount);
        out.pResolveAttachments = sanitize_refs(s, 2, in.pResolveAttachments, in.colorAttachmentCount);
        if (in.pDepthStencilAttachment != nullptr) {
            auto& storage = ref_storage[static_cast<size_t>(s) * 4 + 3];
            storage.resize(1);
            storage[0] = *in.pDepthStencilAttachment;
            storage[0].layout = LayoutCompat::sanitize(storage[0].layout);
            out.pDepthStencilAttachment = storage.data();
        }
    }

    VkRenderPassCreateInfo modified = *pCreateInfo;
    modified.pAttachments = attachments.empty() ? nullptr : attachments.data();
    modified.pSubpasses = subpasses.empty() ? nullptr : subpasses.data();
    return create(&modified);
}
VK_LAYER_EXPORT void VKAPI_CALL vkDestroyRenderPass(
    VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator) {
    LayerManager::get().forget_render_pass(device, renderPass);
    auto real = (PFN_vkDestroyRenderPass)get_real_proc(
        get_last_instance(), device, "vkDestroyRenderPass");
    if (real) real(device, renderPass, pAllocator);
}

// VK_KHR_imageless_framebuffer emulation: a framebuffer created with
// VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT gets a synthetic handle; the real
// framebuffer is created lazily at vkCmdBeginRenderPass when the application
// supplies the attachment views.
VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateFramebuffer(
    VkDevice device,
    const VkFramebufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFramebuffer* pFramebuffer
) {
    if (pCreateInfo != nullptr && pFramebuffer != nullptr &&
        (pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0 &&
        ImagelessFramebufferManager::get().is_emulated(device)) {
        return ImagelessFramebufferManager::get().create(device, pCreateInfo, pFramebuffer);
    }

    static PFN_vkCreateFramebuffer s_real_fn = nullptr;
    if (!s_real_fn) {
        s_real_fn = (PFN_vkCreateFramebuffer)
            get_real_proc(get_last_instance(), device, "vkCreateFramebuffer");
    }
    if (!s_real_fn) return VK_ERROR_INITIALIZATION_FAILED;
    return s_real_fn(device, pCreateInfo, pAllocator, pFramebuffer);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyFramebuffer(
    VkDevice device,
    VkFramebuffer framebuffer,
    const VkAllocationCallbacks* pAllocator
) {
    if (ImagelessFramebufferManager::get().destroy(device, framebuffer)) {
        return;
    }

    static PFN_vkDestroyFramebuffer s_real_fn = nullptr;
    if (!s_real_fn) {
        s_real_fn = (PFN_vkDestroyFramebuffer)
            get_real_proc(get_last_instance(), device, "vkDestroyFramebuffer");
    }
    if (s_real_fn) {
        s_real_fn(device, framebuffer, pAllocator);
    }
}
VK_LAYER_EXPORT void VKAPI_CALL vkDestroyPipeline(
    VkDevice device,
    VkPipeline pipeline,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_pipeline(device, pipeline, pAllocator);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBindPipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline
) {
    LayerManager::get().dispatch_cmd_bind_pipeline(commandBuffer, pipelineBindPoint, pipeline);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBindVertexBuffers(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets
) {
    LayerManager::get().dispatch_cmd_bind_vertex_buffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBindVertexBuffers2(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets,
    const VkDeviceSize* pSizes,
    const VkDeviceSize* pStrides
) {
    LayerManager::get().dispatch_cmd_bind_vertex_buffers2(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBindVertexBuffers2EXT(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets,
    const VkDeviceSize* pSizes,
    const VkDeviceSize* pStrides
) {
    LayerManager::get().dispatch_cmd_bind_vertex_buffers2(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDraw(
    VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance
) {
    LayerManager::get().dispatch_cmd_draw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance
) {
    LayerManager::get().dispatch_cmd_draw_indexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDrawIndirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride
) {
    LayerManager::get().dispatch_cmd_draw_indirect(commandBuffer, buffer, offset, drawCount, stride);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDrawIndexedIndirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride
) {
    LayerManager::get().dispatch_cmd_draw_indexed_indirect(commandBuffer, buffer, offset, drawCount, stride);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBeginRenderPass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    VkSubpassContents contents
) {
    LayerManager::get().dispatch_cmd_begin_render_pass(commandBuffer, pRenderPassBegin, contents);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdNextSubpass(
    VkCommandBuffer commandBuffer,
    VkSubpassContents contents
) {
    LayerManager::get().dispatch_cmd_next_subpass(commandBuffer, contents);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdEndRenderPass(
    VkCommandBuffer commandBuffer
) {
    LayerManager::get().dispatch_cmd_end_render_pass(commandBuffer);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDispatch(
    VkCommandBuffer commandBuffer,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ
) {
    LayerManager::get().dispatch_cmd_dispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDispatchIndirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset
) {
    LayerManager::get().dispatch_cmd_dispatch_indirect(commandBuffer, buffer, offset);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdCopyBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkBuffer dstBuffer,
    uint32_t regionCount,
    const VkBufferCopy* pRegions
) {
    LayerManager::get().dispatch_cmd_copy_buffer(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdCopyImage(
    VkCommandBuffer commandBuffer,
    VkImage srcImage,
    VkImageLayout srcImageLayout,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkImageCopy* pRegions
) {
    LayerManager::get().dispatch_cmd_copy_image(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBlitImage(
    VkCommandBuffer commandBuffer,
    VkImage srcImage,
    VkImageLayout srcImageLayout,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkImageBlit* pRegions,
    VkFilter filter
) {
    LayerManager::get().dispatch_cmd_blit_image(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdCopyBufferToImage(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkBufferImageCopy* pRegions
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.CmdCopyBufferToImage) {
        const VkImageLayout layout = LayoutCompat::get().is_emulated(device)
            ? LayoutCompat::sanitize(dstImageLayout) : dstImageLayout;
        dt.CmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage,
                                layout, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdCopyImageToBuffer(
    VkCommandBuffer commandBuffer,
    VkImage srcImage,
    VkImageLayout srcImageLayout,
    VkBuffer dstBuffer,
    uint32_t regionCount,
    const VkBufferImageCopy* pRegions
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.CmdCopyImageToBuffer) {
        const VkImageLayout layout = LayoutCompat::get().is_emulated(device)
            ? LayoutCompat::sanitize(srcImageLayout) : srcImageLayout;
        dt.CmdCopyImageToBuffer(commandBuffer, srcImage,
                                layout, dstBuffer, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdFillBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer dstBuffer,
    VkDeviceSize dstOffset,
    VkDeviceSize size,
    uint32_t data
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    PFN_vkCmdFillBuffer real_fn = (PFN_vkCmdFillBuffer)
        get_real_proc(get_last_instance(), device, "vkCmdFillBuffer");
    if (real_fn) {
        real_fn(commandBuffer, dstBuffer, dstOffset, size, data);
    }
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdUpdateBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer dstBuffer,
    VkDeviceSize dstOffset,
    VkDeviceSize dataSize,
    const void* pData
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    PFN_vkCmdUpdateBuffer real_fn = (PFN_vkCmdUpdateBuffer)
        get_real_proc(get_last_instance(), device, "vkCmdUpdateBuffer");
    if (real_fn) {
        real_fn(commandBuffer, dstBuffer, dstOffset, dataSize, pData);
    }
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdResolveImage(
    VkCommandBuffer commandBuffer,
    VkImage srcImage,
    VkImageLayout srcImageLayout,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkImageResolve* pRegions
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    PFN_vkCmdResolveImage real_fn = (PFN_vkCmdResolveImage)
        get_real_proc(get_last_instance(), device, "vkCmdResolveImage");
    if (real_fn) {
        const bool emulated = LayoutCompat::get().is_emulated(device);
        real_fn(commandBuffer, srcImage,
                emulated ? LayoutCompat::sanitize(srcImageLayout) : srcImageLayout,
                dstImage,
                emulated ? LayoutCompat::sanitize(dstImageLayout) : dstImageLayout,
                regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdCopyQueryPoolResults(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount,
    VkBuffer dstBuffer,
    VkDeviceSize dstOffset,
    VkDeviceSize stride,
    VkQueryResultFlags flags
) {
    TBDRBarrierOptimizer::get().on_cmd_action(commandBuffer);
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    PFN_vkCmdCopyQueryPoolResults real_fn = (PFN_vkCmdCopyQueryPoolResults)
        get_real_proc(get_last_instance(), device, "vkCmdCopyQueryPoolResults");
    if (real_fn) {
        real_fn(commandBuffer, queryPool, firstQuery, queryCount,
                dstBuffer, dstOffset, stride, flags);
    }
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdClearAttachments(
    VkCommandBuffer commandBuffer,
    uint32_t attachmentCount,
    const VkClearAttachment* pAttachments,
    uint32_t rectCount,
    const VkClearRect* pRects
) {
    LayerManager::get().dispatch_cmd_clear_attachments(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdClearColorImage(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout imageLayout,
    const VkClearColorValue* pColor,
    uint32_t rangeCount,
    const VkImageSubresourceRange* pRanges
) {
    LayerManager::get().dispatch_cmd_clear_color_image(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdClearDepthStencilImage(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout imageLayout,
    const VkClearDepthStencilValue* pDepthStencil,
    uint32_t rangeCount,
    const VkImageSubresourceRange* pRanges
) {
    LayerManager::get().dispatch_cmd_clear_depth_stencil_image(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdExecuteCommands(
    VkCommandBuffer commandBuffer,
    uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers
) {
    LayerManager::get().dispatch_cmd_execute_commands(commandBuffer, commandBufferCount, pCommandBuffers);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdSetEvent2(
    VkCommandBuffer commandBuffer,
    VkEvent event,
    const VkDependencyInfo* pDependencyInfo
) {
    LayerManager::get().dispatch_cmd_set_event2(commandBuffer, event, pDependencyInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdSetEvent2KHR(
    VkCommandBuffer commandBuffer,
    VkEvent event,
    const VkDependencyInfo* pDependencyInfo
) {
    LayerManager::get().dispatch_cmd_set_event2(commandBuffer, event, pDependencyInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdResetEvent2(
    VkCommandBuffer commandBuffer,
    VkEvent event,
    VkPipelineStageFlags2 stageMask
) {
    LayerManager::get().dispatch_cmd_reset_event2(commandBuffer, event, stageMask);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdResetEvent2KHR(
    VkCommandBuffer commandBuffer,
    VkEvent event,
    VkPipelineStageFlags2 stageMask
) {
    LayerManager::get().dispatch_cmd_reset_event2(commandBuffer, event, stageMask);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdWaitEvents2(
    VkCommandBuffer commandBuffer,
    uint32_t eventCount,
    const VkEvent* pEvents,
    const VkDependencyInfo* pDependencyInfos
) {
    LayerManager::get().dispatch_cmd_wait_events2(commandBuffer, eventCount, pEvents, pDependencyInfos);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdWaitEvents2KHR(
    VkCommandBuffer commandBuffer,
    uint32_t eventCount,
    const VkEvent* pEvents,
    const VkDependencyInfo* pDependencyInfos
) {
    LayerManager::get().dispatch_cmd_wait_events2(commandBuffer, eventCount, pEvents, pDependencyInfos);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount,
    const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers
) {
    LayerManager::get().dispatch_cmd_pipeline_barrier(
        commandBuffer,
        srcStageMask,
        dstStageMask,
        dependencyFlags,
        memoryBarrierCount,
        pMemoryBarriers,
        bufferMemoryBarrierCount,
        pBufferMemoryBarriers,
        imageMemoryBarrierCount,
        pImageMemoryBarriers);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdPipelineBarrier2(
    VkCommandBuffer commandBuffer,
    const VkDependencyInfo* pDependencyInfo
) {
    LayerManager::get().dispatch_cmd_pipeline_barrier2(commandBuffer, pDependencyInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdPipelineBarrier2KHR(
    VkCommandBuffer commandBuffer,
    const VkDependencyInfo* pDependencyInfo
) {
    LayerManager::get().dispatch_cmd_pipeline_barrier2(commandBuffer, pDependencyInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdWriteTimestamp2(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags2 stage,
    VkQueryPool queryPool,
    uint32_t query
) {
    LayerManager::get().dispatch_cmd_write_timestamp2(commandBuffer, stage, queryPool, query);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdWriteTimestamp2KHR(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags2 stage,
    VkQueryPool queryPool,
    uint32_t query
) {
    LayerManager::get().dispatch_cmd_write_timestamp2(commandBuffer, stage, queryPool, query);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkQueueSubmit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence
) {
    return LayerManager::get().dispatch_queue_submit2(queue, submitCount, pSubmits, fence);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkQueueSubmit2KHR(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence
) {
    return LayerManager::get().dispatch_queue_submit2(queue, submitCount, pSubmits, fence);
}

// ----------------------------------------------------------------------------
// Vulkan 1.2 Core Functions & Promoted Extensions
// ----------------------------------------------------------------------------

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateSemaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSemaphore* pSemaphore
) {
    return LayerManager::get().dispatch_create_semaphore(device, pCreateInfo, pAllocator, pSemaphore);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroySemaphore(
    VkDevice device,
    VkSemaphore semaphore,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_semaphore(device, semaphore, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetSemaphoreCounterValue(
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t* pValue
) {
    return LayerManager::get().dispatch_get_semaphore_counter_value(device, semaphore, pValue);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetSemaphoreCounterValueKHR(
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t* pValue
) {
    return LayerManager::get().dispatch_get_semaphore_counter_value(device, semaphore, pValue);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkWaitSemaphores(
    VkDevice device,
    const VkSemaphoreWaitInfo* pWaitInfo,
    uint64_t timeout
) {
    return LayerManager::get().dispatch_wait_semaphores(device, pWaitInfo, timeout);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkWaitSemaphoresKHR(
    VkDevice device,
    const VkSemaphoreWaitInfo* pWaitInfo,
    uint64_t timeout
) {
    return LayerManager::get().dispatch_wait_semaphores(device, pWaitInfo, timeout);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkSignalSemaphore(
    VkDevice device,
    const VkSemaphoreSignalInfo* pSignalInfo
) {
    return LayerManager::get().dispatch_signal_semaphore(device, pSignalInfo);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkSignalSemaphoreKHR(
    VkDevice device,
    const VkSemaphoreSignalInfo* pSignalInfo
) {
    return LayerManager::get().dispatch_signal_semaphore(device, pSignalInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkResetQueryPool(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount
) {
    LayerManager::get().dispatch_reset_query_pool(device, queryPool, firstQuery, queryCount);
}

VK_LAYER_EXPORT void VKAPI_CALL vkResetQueryPoolEXT(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount
) {
    LayerManager::get().dispatch_reset_query_pool(device, queryPool, firstQuery, queryCount);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateRenderPass2(
    VkDevice device,
    const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass
) {
    return LayerManager::get().dispatch_create_render_pass2(device, pCreateInfo, pAllocator, pRenderPass);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateRenderPass2KHR(
    VkDevice device,
    const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass
) {
    return LayerManager::get().dispatch_create_render_pass2(device, pCreateInfo, pAllocator, pRenderPass);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBeginRenderPass2(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo
) {
    LayerManager::get().dispatch_cmd_begin_render_pass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdBeginRenderPass2KHR(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo
) {
    LayerManager::get().dispatch_cmd_begin_render_pass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdNextSubpass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassBeginInfo* pSubpassBeginInfo,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    LayerManager::get().dispatch_cmd_next_subpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdNextSubpass2KHR(
    VkCommandBuffer commandBuffer,
    const VkSubpassBeginInfo* pSubpassBeginInfo,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    LayerManager::get().dispatch_cmd_next_subpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdEndRenderPass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    LayerManager::get().dispatch_cmd_end_render_pass2(commandBuffer, pSubpassEndInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdEndRenderPass2KHR(
    VkCommandBuffer commandBuffer,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    LayerManager::get().dispatch_cmd_end_render_pass2(commandBuffer, pSubpassEndInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDrawIndirectCount(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    LayerManager::get().dispatch_cmd_draw_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDrawIndirectCountKHR(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    LayerManager::get().dispatch_cmd_draw_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDrawIndirectCountAMD(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    LayerManager::get().dispatch_cmd_draw_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDrawIndexedIndirectCount(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    LayerManager::get().dispatch_cmd_draw_indexed_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDrawIndexedIndirectCountKHR(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    LayerManager::get().dispatch_cmd_draw_indexed_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdDrawIndexedIndirectCountAMD(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    LayerManager::get().dispatch_cmd_draw_indexed_indirect_count(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VK_LAYER_EXPORT VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(
    VkDevice device,
    const VkBufferDeviceAddressInfo* pInfo
) {
    return LayerManager::get().dispatch_get_buffer_device_address(device, pInfo);
}

VK_LAYER_EXPORT VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddressKHR(
    VkDevice device,
    const VkBufferDeviceAddressInfo* pInfo
) {
    return LayerManager::get().dispatch_get_buffer_device_address(device, pInfo);
}

VK_LAYER_EXPORT VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddressEXT(
    VkDevice device,
    const VkBufferDeviceAddressInfo* pInfo
) {
    return LayerManager::get().dispatch_get_buffer_device_address(device, pInfo);
}

VK_LAYER_EXPORT uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddress(
    VkDevice device,
    const VkBufferDeviceAddressInfo* pInfo
) {
    return LayerManager::get().dispatch_get_buffer_opaque_capture_address(device, pInfo);
}

VK_LAYER_EXPORT uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddressKHR(
    VkDevice device,
    const VkBufferDeviceAddressInfo* pInfo
) {
    return LayerManager::get().dispatch_get_buffer_opaque_capture_address(device, pInfo);
}

VK_LAYER_EXPORT uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddress(
    VkDevice device,
    const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo
) {
    return LayerManager::get().dispatch_get_device_memory_opaque_capture_address(device, pInfo);
}

VK_LAYER_EXPORT uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddressKHR(
    VkDevice device,
    const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo
) {
    return LayerManager::get().dispatch_get_device_memory_opaque_capture_address(device, pInfo);
}

// ============================================================================
// Vulkan 1.1 Exported Entry Points
// ============================================================================

// Memory Binding 2
VK_LAYER_EXPORT VkResult VKAPI_CALL vkBindBufferMemory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindBufferMemoryInfo* pBindInfos
) {
    return LayerManager::get().dispatch_bind_buffer_memory2(device, bindInfoCount, pBindInfos);
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkBindBufferMemory2KHR(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindBufferMemoryInfo* pBindInfos
) {
    return LayerManager::get().dispatch_bind_buffer_memory2(device, bindInfoCount, pBindInfos);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkBindImageMemory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindImageMemoryInfo* pBindInfos
) {
    return LayerManager::get().dispatch_bind_image_memory2(device, bindInfoCount, pBindInfos);
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkBindImageMemory2KHR(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindImageMemoryInfo* pBindInfos
) {
    return LayerManager::get().dispatch_bind_image_memory2(device, bindInfoCount, pBindInfos);
}

// Memory Requirements 2
VK_LAYER_EXPORT void VKAPI_CALL vkGetBufferMemoryRequirements2(
    VkDevice device,
    const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    LayerManager::get().dispatch_get_buffer_memory_requirements2(device, pInfo, pMemoryRequirements);
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetBufferMemoryRequirements2KHR(
    VkDevice device,
    const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    LayerManager::get().dispatch_get_buffer_memory_requirements2(device, pInfo, pMemoryRequirements);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetImageMemoryRequirements2(
    VkDevice device,
    const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    LayerManager::get().dispatch_get_image_memory_requirements2(device, pInfo, pMemoryRequirements);
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetImageMemoryRequirements2KHR(
    VkDevice device,
    const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    LayerManager::get().dispatch_get_image_memory_requirements2(device, pInfo, pMemoryRequirements);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetImageSparseMemoryRequirements2(
    VkDevice device,
    const VkImageSparseMemoryRequirementsInfo2* pInfo,
    uint32_t* pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2* pSparseMemoryRequirements
) {
    LayerManager::get().dispatch_get_image_sparse_memory_requirements2(
        device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetImageSparseMemoryRequirements2KHR(
    VkDevice device,
    const VkImageSparseMemoryRequirementsInfo2* pInfo,
    uint32_t* pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2* pSparseMemoryRequirements
) {
    LayerManager::get().dispatch_get_image_sparse_memory_requirements2(
        device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

// Maintenance 1 & 3
VK_LAYER_EXPORT void VKAPI_CALL vkTrimCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolTrimFlags flags
) {
    LayerManager::get().dispatch_trim_command_pool(device, commandPool, flags);
}
VK_LAYER_EXPORT void VKAPI_CALL vkTrimCommandPoolKHR(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolTrimFlags flags
) {
    LayerManager::get().dispatch_trim_command_pool(device, commandPool, flags);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetDescriptorSetLayoutSupport(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    VkDescriptorSetLayoutSupport* pSupport
) {
    LayerManager::get().dispatch_get_descriptor_set_layout_support(device, pCreateInfo, pSupport);
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetDescriptorSetLayoutSupportKHR(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    VkDescriptorSetLayoutSupport* pSupport
) {
    LayerManager::get().dispatch_get_descriptor_set_layout_support(device, pCreateInfo, pSupport);
}

// Device Groups & Dispatch Base
VK_LAYER_EXPORT void VKAPI_CALL vkCmdDispatchBase(
    VkCommandBuffer commandBuffer,
    uint32_t baseGroupX,
    uint32_t baseGroupY,
    uint32_t baseGroupZ,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ
) {
    LayerManager::get().dispatch_cmd_dispatch_base(
        commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
}
VK_LAYER_EXPORT void VKAPI_CALL vkCmdDispatchBaseKHR(
    VkCommandBuffer commandBuffer,
    uint32_t baseGroupX,
    uint32_t baseGroupY,
    uint32_t baseGroupZ,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ
) {
    LayerManager::get().dispatch_cmd_dispatch_base(
        commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
}

VK_LAYER_EXPORT void VKAPI_CALL vkCmdSetDeviceMask(
    VkCommandBuffer commandBuffer,
    uint32_t deviceMask
) {
    LayerManager::get().dispatch_cmd_set_device_mask(commandBuffer, deviceMask);
}
VK_LAYER_EXPORT void VKAPI_CALL vkCmdSetDeviceMaskKHR(
    VkCommandBuffer commandBuffer,
    uint32_t deviceMask
) {
    LayerManager::get().dispatch_cmd_set_device_mask(commandBuffer, deviceMask);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetDeviceGroupPeerMemoryFeatures(
    VkDevice device,
    uint32_t heapIndex,
    uint32_t localDeviceIndex,
    uint32_t remoteDeviceIndex,
    VkPeerMemoryFeatureFlags* pPeerMemoryFeatures
) {
    LayerManager::get().dispatch_get_device_group_peer_memory_features(
        device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetDeviceGroupPeerMemoryFeaturesKHR(
    VkDevice device,
    uint32_t heapIndex,
    uint32_t localDeviceIndex,
    uint32_t remoteDeviceIndex,
    VkPeerMemoryFeatureFlags* pPeerMemoryFeatures
) {
    LayerManager::get().dispatch_get_device_group_peer_memory_features(
        device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties
) {
    auto result = LayerManager::get().dispatch_enumerate_physical_device_groups(
        instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    if (pPhysicalDeviceGroupProperties && pPhysicalDeviceGroupCount &&
        (result == VK_SUCCESS || result == VK_INCOMPLETE))
        for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; ++i)
            vkfix::remember_physical_devices(instance, pPhysicalDeviceGroupProperties[i].physicalDeviceCount,
                                            pPhysicalDeviceGroupProperties[i].physicalDevices);
    return result;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroupsKHR(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties
) {
    return vkEnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
}

// Physical device queries must use the instance that enumerated the handle.
// A later instance may have different versions, extensions and dispatch routes.
static void* physical_device_proc(VkPhysicalDevice physicalDevice, const char* name) {
    auto instance = vkfix::native_instance_for_physical(physicalDevice);
    if (!instance) instance = get_last_instance();
    return get_real_proc(instance, VK_NULL_HANDLE, name);
}

// Physical Device Properties 2
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkFormatProperties2* pFormatProperties
) {
    PFN_vkGetPhysicalDeviceFormatProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceFormatProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceFormatProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceFormatProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceFormatProperties2KHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, format, pFormatProperties);
        if (pFormatProperties) {
            auto real_fp = (PFN_vkGetPhysicalDeviceFormatProperties)
                physical_device_proc(physicalDevice, "vkGetPhysicalDeviceFormatProperties");
            if (real_fp) {
                VkFormatProperties reference{};
                real_fp(physicalDevice, format, &reference);
                const auto &reported = pFormatProperties->formatProperties;
                if (reported.linearTilingFeatures != reference.linearTilingFeatures ||
                    reported.optimalTilingFeatures != reference.optimalTilingFeatures ||
                    reported.bufferFeatures != reference.bufferFeatures) {
                    LOGW("vkGetPhysicalDeviceFormatProperties2 mismatch repaired from legacy query");
                    pFormatProperties->formatProperties = reference;
                }
            }
        }
        return;
    }
    if (pFormatProperties) {
        PFN_vkGetPhysicalDeviceFormatProperties real_fp =
            (PFN_vkGetPhysicalDeviceFormatProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceFormatProperties");
        if (real_fp) {
            real_fp(physicalDevice, format, &pFormatProperties->formatProperties);
        }
    }
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2KHR(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkFormatProperties2* pFormatProperties
) {
    vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, pFormatProperties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
    VkImageFormatProperties2* pImageFormatProperties
) {
    PFN_vkGetPhysicalDeviceImageFormatProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceImageFormatProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    }
    if (real_fn) {
        const VkResult result = real_fn(physicalDevice, pImageFormatInfo, pImageFormatProperties);
        if (result == VK_SUCCESS && pImageFormatInfo && pImageFormatProperties &&
            pImageFormatInfo->pNext == nullptr) {
            auto real_ifp = (PFN_vkGetPhysicalDeviceImageFormatProperties)
                physical_device_proc(physicalDevice, "vkGetPhysicalDeviceImageFormatProperties");
            if (real_ifp) {
                VkImageFormatProperties reference{};
                if (real_ifp(physicalDevice, pImageFormatInfo->format, pImageFormatInfo->type,
                             pImageFormatInfo->tiling, pImageFormatInfo->usage,
                             pImageFormatInfo->flags, &reference) == VK_SUCCESS) {
                    const auto &reported = pImageFormatProperties->imageFormatProperties;
                    if (reported.maxExtent.width != reference.maxExtent.width ||
                        reported.maxExtent.height != reference.maxExtent.height ||
                        reported.maxExtent.depth != reference.maxExtent.depth ||
                        reported.maxMipLevels != reference.maxMipLevels ||
                        reported.maxArrayLayers != reference.maxArrayLayers ||
                        reported.sampleCounts != reference.sampleCounts ||
                        reported.maxResourceSize != reference.maxResourceSize) {
                        LOGW("vkGetPhysicalDeviceImageFormatProperties2 mismatch repaired from legacy query");
                        pImageFormatProperties->imageFormatProperties = reference;
                    }
                }
            }
        }
        return result;
    }
    if (!pImageFormatInfo || !pImageFormatProperties) return VK_ERROR_INITIALIZATION_FAILED;

    // A 1.0 driver cannot report the 1.1 pNext properties; initialise them so
    // the application never reads uninitialised memory.
    if (auto* external = vku::find_pnext_mut<VkExternalImageFormatProperties>(
            pImageFormatProperties->pNext, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)) {
        memset(&external->externalMemoryProperties, 0, sizeof(external->externalMemoryProperties));
    }
    if (auto* ycbcr = vku::find_pnext_mut<VkSamplerYcbcrConversionImageFormatProperties>(
            pImageFormatProperties->pNext, VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES)) {
        ycbcr->combinedImageSamplerDescriptorCount = 1;
    }
    if (auto* cubic = vku::find_pnext_mut<VkFilterCubicImageViewImageFormatPropertiesEXT>(
            pImageFormatProperties->pNext, VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT)) {
        cubic->filterCubic = VK_FALSE;
        cubic->filterCubicMinmax = VK_FALSE;
    }

    PFN_vkGetPhysicalDeviceImageFormatProperties real_ifp =
        (PFN_vkGetPhysicalDeviceImageFormatProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceImageFormatProperties");
    if (real_ifp) {
        return real_ifp(
            physicalDevice,
            pImageFormatInfo->format,
            pImageFormatInfo->type,
            pImageFormatInfo->tiling,
            pImageFormatInfo->usage,
            pImageFormatInfo->flags,
            &pImageFormatProperties->imageFormatProperties
        );
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
    VkImageFormatProperties2* pImageFormatProperties
) {
    return vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice physicalDevice,
    uint32_t* pQueueFamilyPropertyCount,
    VkQueueFamilyProperties2* pQueueFamilyProperties
) {
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceQueueFamilyProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
        if (pQueueFamilyPropertyCount && pQueueFamilyProperties &&
            *pQueueFamilyPropertyCount > 0) {
            auto real_qfp = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
                physical_device_proc(physicalDevice, "vkGetPhysicalDeviceQueueFamilyProperties");
            if (real_qfp) {
                uint32_t reference_count = *pQueueFamilyPropertyCount;
                std::vector<VkQueueFamilyProperties> reference(reference_count);
                real_qfp(physicalDevice, &reference_count, reference.data());
                if (reference_count == *pQueueFamilyPropertyCount) {
                    bool same = true;
                    for (uint32_t i = 0; same && i < reference_count; ++i) {
                        const auto &reported = pQueueFamilyProperties[i].queueFamilyProperties;
                        const auto &expected = reference[i];
                        same = reported.queueFlags == expected.queueFlags &&
                               reported.queueCount == expected.queueCount &&
                               reported.timestampValidBits == expected.timestampValidBits &&
                               reported.minImageTransferGranularity.width == expected.minImageTransferGranularity.width &&
                               reported.minImageTransferGranularity.height == expected.minImageTransferGranularity.height &&
                               reported.minImageTransferGranularity.depth == expected.minImageTransferGranularity.depth;
                    }
                    if (!same) {
                        LOGW("vkGetPhysicalDeviceQueueFamilyProperties2 mismatch repaired from legacy query");
                        for (uint32_t i = 0; i < reference_count; ++i)
                            pQueueFamilyProperties[i].queueFamilyProperties = reference[i];
                    }
                }
            }
        }
        return;
    }

    if (!pQueueFamilyPropertyCount) return;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties real_qfp =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!real_qfp) return;

    if (!pQueueFamilyProperties) {
        real_qfp(physicalDevice, pQueueFamilyPropertyCount, nullptr);
        return;
    }

    vkfix::query_legacy_array<VkQueueFamilyProperties>(
        pQueueFamilyPropertyCount, pQueueFamilyProperties,
        [&](uint32_t* count, VkQueueFamilyProperties* values) {
            real_qfp(physicalDevice, count, values);
        },
        [](VkQueueFamilyProperties2& output, const VkQueueFamilyProperties& payload) {
            output.queueFamilyProperties = payload;
        });
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2KHR(
    VkPhysicalDevice physicalDevice,
    uint32_t* pQueueFamilyPropertyCount,
    VkQueueFamilyProperties2* pQueueFamilyProperties
) {
    vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties
) {
    PFN_vkGetPhysicalDeviceMemoryProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceMemoryProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceMemoryProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceMemoryProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceMemoryProperties2KHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, pMemoryProperties);
        if (pMemoryProperties) {
            auto real_mp = (PFN_vkGetPhysicalDeviceMemoryProperties)
                physical_device_proc(physicalDevice, "vkGetPhysicalDeviceMemoryProperties");
            if (real_mp) {
                VkPhysicalDeviceMemoryProperties reference{};
                real_mp(physicalDevice, &reference);
                const auto &reported = pMemoryProperties->memoryProperties;
                bool same = reported.memoryTypeCount == reference.memoryTypeCount &&
                            reported.memoryHeapCount == reference.memoryHeapCount;
                for (uint32_t i = 0; same && i < reported.memoryTypeCount; ++i)
                    same = reported.memoryTypes[i].propertyFlags == reference.memoryTypes[i].propertyFlags &&
                           reported.memoryTypes[i].heapIndex == reference.memoryTypes[i].heapIndex;
                for (uint32_t i = 0; same && i < reported.memoryHeapCount; ++i)
                    same = reported.memoryHeaps[i].size == reference.memoryHeaps[i].size &&
                           reported.memoryHeaps[i].flags == reference.memoryHeaps[i].flags;
                if (!same) {
                    LOGW("vkGetPhysicalDeviceMemoryProperties2 mismatch repaired from legacy query");
                    pMemoryProperties->memoryProperties = reference;
                }
            }
        }
        return;
    }
    if (pMemoryProperties) {
        PFN_vkGetPhysicalDeviceMemoryProperties real_mp =
            (PFN_vkGetPhysicalDeviceMemoryProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceMemoryProperties");
        if (real_mp) {
            real_mp(physicalDevice, &pMemoryProperties->memoryProperties);
        }
    }
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2KHR(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties
) {
    vkGetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSparseImageFormatInfo2* pFormatInfo,
    uint32_t* pPropertyCount,
    VkSparseImageFormatProperties2* pProperties
) {
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceSparseImageFormatProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR");
    }
    if (real_fn) {
        const uint32_t capacity = pPropertyCount ? *pPropertyCount : 0;
        real_fn(physicalDevice, pFormatInfo, pPropertyCount, pProperties);
        if (!pFormatInfo || !pPropertyCount) return;
        auto real_sfp = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)
            physical_device_proc(physicalDevice, "vkGetPhysicalDeviceSparseImageFormatProperties");
        if (!real_sfp) return;
        if (!pProperties) {
            uint32_t reference_count = 0;
            real_sfp(physicalDevice, pFormatInfo->format, pFormatInfo->type, pFormatInfo->samples,
                     pFormatInfo->usage, pFormatInfo->tiling, &reference_count, nullptr);
            if (reference_count != *pPropertyCount) {
                LOGW("vkGetPhysicalDeviceSparseImageFormatProperties2 count repaired from legacy query");
                *pPropertyCount = reference_count;
            }
            return;
        }
        uint32_t reference_count = capacity;
        if (reference_count == 0) return;
        std::vector<VkSparseImageFormatProperties> reference(reference_count);
        real_sfp(physicalDevice, pFormatInfo->format, pFormatInfo->type, pFormatInfo->samples,
                 pFormatInfo->usage, pFormatInfo->tiling, &reference_count, reference.data());
        const uint32_t reported_count = std::min(*pPropertyCount, capacity);
        if (reference_count != reported_count) return;
        bool same = true;
        for (uint32_t i = 0; same && i < reference_count; ++i) {
            const auto &reported = pProperties[i].properties;
            const auto &expected = reference[i];
            same = reported.aspectMask == expected.aspectMask &&
                   reported.flags == expected.flags &&
                   reported.imageGranularity.width == expected.imageGranularity.width &&
                   reported.imageGranularity.height == expected.imageGranularity.height &&
                   reported.imageGranularity.depth == expected.imageGranularity.depth;
        }
        if (!same) {
            LOGW("vkGetPhysicalDeviceSparseImageFormatProperties2 mismatch repaired from legacy query");
            for (uint32_t i = 0; i < reference_count; ++i)
                pProperties[i].properties = reference[i];
        }
        return;
    }
    if (!pFormatInfo || !pPropertyCount) return;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties real_sfp =
        (PFN_vkGetPhysicalDeviceSparseImageFormatProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceSparseImageFormatProperties");
    if (!real_sfp) {
        *pPropertyCount = 0;
        return;
    }
    if (!pProperties) {
        real_sfp(physicalDevice, pFormatInfo->format, pFormatInfo->type, pFormatInfo->samples, pFormatInfo->usage, pFormatInfo->tiling, pPropertyCount, nullptr);
        return;
    }
    vkfix::query_legacy_array<VkSparseImageFormatProperties>(
        pPropertyCount, pProperties,
        [&](uint32_t* count, VkSparseImageFormatProperties* values) {
            real_sfp(physicalDevice, pFormatInfo->format, pFormatInfo->type,
                     pFormatInfo->samples, pFormatInfo->usage, pFormatInfo->tiling,
                     count, values);
        },
        [](VkSparseImageFormatProperties2& output, const VkSparseImageFormatProperties& payload) {
            output.properties = payload;
        });
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSparseImageFormatInfo2* pFormatInfo,
    uint32_t* pPropertyCount,
    VkSparseImageFormatProperties2* pProperties
) {
    vkGetPhysicalDeviceSparseImageFormatProperties2(physicalDevice, pFormatInfo, pPropertyCount, pProperties);
}

// External Properties
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo,
    VkExternalBufferProperties* pExternalBufferProperties
) {
    PFN_vkGetPhysicalDeviceExternalBufferProperties real_fn =
        (PFN_vkGetPhysicalDeviceExternalBufferProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceExternalBufferProperties");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceExternalBufferProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
        return;
    }
    if (pExternalBufferProperties) {
        pExternalBufferProperties->externalMemoryProperties.externalMemoryFeatures = 0;
        pExternalBufferProperties->externalMemoryProperties.exportFromImportedHandleTypes = 0;
        pExternalBufferProperties->externalMemoryProperties.compatibleHandleTypes = 0;
    }
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceExternalBufferPropertiesKHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo,
    VkExternalBufferProperties* pExternalBufferProperties
) {
    vkGetPhysicalDeviceExternalBufferProperties(physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo,
    VkExternalFenceProperties* pExternalFenceProperties
) {
    PFN_vkGetPhysicalDeviceExternalFenceProperties real_fn =
        (PFN_vkGetPhysicalDeviceExternalFenceProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceExternalFenceProperties");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceExternalFenceProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceExternalFencePropertiesKHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
        return;
    }
    if (pExternalFenceProperties) {
        pExternalFenceProperties->exportFromImportedHandleTypes = 0;
        pExternalFenceProperties->compatibleHandleTypes = 0;
        pExternalFenceProperties->externalFenceFeatures = 0;
    }
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceExternalFencePropertiesKHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo,
    VkExternalFenceProperties* pExternalFenceProperties
) {
    vkGetPhysicalDeviceExternalFenceProperties(physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
    VkExternalSemaphoreProperties* pExternalSemaphoreProperties
) {
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties real_fn =
        (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties) physical_device_proc(physicalDevice, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
        return;
    }
    if (pExternalSemaphoreProperties) {
        pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
        pExternalSemaphoreProperties->compatibleHandleTypes = 0;
        pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
    }
}
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
    VkExternalSemaphoreProperties* pExternalSemaphoreProperties
) {
    vkGetPhysicalDeviceExternalSemaphoreProperties(physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
}

// Sampler YCbCr Conversion
VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateSamplerYcbcrConversion(
    VkDevice device,
    const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSamplerYcbcrConversion* pYcbcrConversion
) {
    PFN_vkCreateSamplerYcbcrConversion real_fn =
        (PFN_vkCreateSamplerYcbcrConversion) get_real_proc(get_last_instance(), device, "vkCreateSamplerYcbcrConversion");
    if (!real_fn) {
        real_fn = (PFN_vkCreateSamplerYcbcrConversion) get_real_proc(get_last_instance(), device, "vkCreateSamplerYcbcrConversionKHR");
    }
    if (real_fn) {
        return real_fn(device, pCreateInfo, pAllocator, pYcbcrConversion);
    }
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateSamplerYcbcrConversionKHR(
    VkDevice device,
    const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSamplerYcbcrConversion* pYcbcrConversion
) {
    return vkCreateSamplerYcbcrConversion(device, pCreateInfo, pAllocator, pYcbcrConversion);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroySamplerYcbcrConversion(
    VkDevice device,
    VkSamplerYcbcrConversion ycbcrConversion,
    const VkAllocationCallbacks* pAllocator
) {
    PFN_vkDestroySamplerYcbcrConversion real_fn =
        (PFN_vkDestroySamplerYcbcrConversion) get_real_proc(get_last_instance(), device, "vkDestroySamplerYcbcrConversion");
    if (!real_fn) {
        real_fn = (PFN_vkDestroySamplerYcbcrConversion) get_real_proc(get_last_instance(), device, "vkDestroySamplerYcbcrConversionKHR");
    }
    if (real_fn) {
        real_fn(device, ycbcrConversion, pAllocator);
    }
}
VK_LAYER_EXPORT void VKAPI_CALL vkDestroySamplerYcbcrConversionKHR(
    VkDevice device,
    VkSamplerYcbcrConversion ycbcrConversion,
    const VkAllocationCallbacks* pAllocator
) {
    vkDestroySamplerYcbcrConversion(device, ycbcrConversion, pAllocator);
}

// ============================================================================
// Swapchain & Surface (VK_KHR_swapchain, VK_KHR_surface, VK_KHR_android_surface)
// ============================================================================

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain
) {
    return LayerManager::get().dispatch_create_swapchain(device, pCreateInfo, pAllocator, pSwapchain);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_swapchain(device, swapchain, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages
) {
    return LayerManager::get().dispatch_get_swapchain_images(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex
) {
    return LayerManager::get().dispatch_acquire_next_image(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkAcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* pAcquireInfo,
    uint32_t* pImageIndex
) {
    return LayerManager::get().dispatch_acquire_next_image2(device, pAcquireInfo, pImageIndex);
}

// Vulkan 1.1 device-group present queries. A 1.0 driver has neither entry
// point; expose the single-device (local) topology instead.
VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetDeviceGroupPresentCapabilitiesKHR(
    VkDevice device,
    VkDeviceGroupPresentCapabilitiesKHR* pDeviceGroupPresentCapabilities
) {
    PFN_vkGetDeviceGroupPresentCapabilitiesKHR real_fn =
        (PFN_vkGetDeviceGroupPresentCapabilitiesKHR)
            get_real_proc(get_last_instance(), device, "vkGetDeviceGroupPresentCapabilitiesKHR");
    if (!real_fn) {
        real_fn = (PFN_vkGetDeviceGroupPresentCapabilitiesKHR)
            get_real_proc(get_last_instance(), device, "vkGetDeviceGroupPresentCapabilities");
    }
    if (real_fn) {
        return real_fn(device, pDeviceGroupPresentCapabilities);
    }
    if (!pDeviceGroupPresentCapabilities) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pDeviceGroupPresentCapabilities->presentMask, 0,
           sizeof(pDeviceGroupPresentCapabilities->presentMask));
    pDeviceGroupPresentCapabilities->presentMask[0] = 1;
    pDeviceGroupPresentCapabilities->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetDeviceGroupSurfacePresentModesKHR(
    VkDevice device,
    VkSurfaceKHR surface,
    VkDeviceGroupPresentModeFlagsKHR* pModes
) {
    PFN_vkGetDeviceGroupSurfacePresentModesKHR real_fn =
        (PFN_vkGetDeviceGroupSurfacePresentModesKHR)
            get_real_proc(get_last_instance(), device, "vkGetDeviceGroupSurfacePresentModesKHR");
    if (!real_fn) {
        real_fn = (PFN_vkGetDeviceGroupSurfacePresentModesKHR)
            get_real_proc(get_last_instance(), device, "vkGetDeviceGroupSurfacePresentModes");
    }
    if (real_fn) {
        return real_fn(device, surface, pModes);
    }
    if (!pModes) return VK_ERROR_INITIALIZATION_FAILED;
    *pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo
) {
    return LayerManager::get().dispatch_queue_present(queue, pPresentInfo);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported
) {
    return LayerManager::get().dispatch_get_physical_device_surface_support(physicalDevice, queueFamilyIndex, surface, pSupported);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities
) {
    return LayerManager::get().dispatch_get_physical_device_surface_capabilities(physicalDevice, surface, pSurfaceCapabilities);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities
) {
    return LayerManager::get().dispatch_get_physical_device_surface_capabilities2(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats
) {
    return LayerManager::get().dispatch_get_physical_device_surface_formats(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormat2KHR* pSurfaceFormats
) {
    return LayerManager::get().dispatch_get_physical_device_surface_formats2(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes
) {
    return LayerManager::get().dispatch_get_physical_device_surface_present_modes(physicalDevice, surface, pPresentModeCount, pPresentModes);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(
    VkInstance instance,
    const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface
) {
    return LayerManager::get().dispatch_create_android_surface(instance, pCreateInfo, pAllocator, pSurface);
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator
) {
    LayerManager::get().dispatch_destroy_surface(instance, surface, pAllocator);
}

} // extern "C"

// Stateful entry points needed by the opt-in vertex compute/replay fallback.
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *ci,
               const VkAllocationCallbacks *allocator, VkBuffer *out) {
    auto real = (PFN_vkCreateBuffer)get_real_proc(get_last_instance(), device, "vkCreateBuffer");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto modified = ci ? VertexStoreEmulator::get().buffer_info(device, *ci) : VkBufferCreateInfo{};
    const auto result = real(device, ci ? &modified : ci, allocator, out);
    if (result == VK_SUCCESS && out)
        VertexStoreEmulator::get().buffer(device, *out, ci ? &modified : ci);
    return result;
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                                                           const VkAllocationCallbacks *allocator) {
    VertexStoreEmulator::get().erase_buffer(device, buffer);
    auto real = (PFN_vkDestroyBuffer)get_real_proc(get_last_instance(), device, "vkDestroyBuffer");
    if (real)
        real(device, buffer, allocator);
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
    VkDevice device, VkShaderModule module, const VkAllocationCallbacks *allocator) {
    VertexStoreEmulator::get().erase_shader(device, module);
    vkfix::ShaderModuleCache::get().forget(device, module);
    auto real = (PFN_vkDestroyShaderModule)get_real_proc(get_last_instance(), device,
                                                         "vkDestroyShaderModule");
    if (real)
        real(device, module, allocator);
}
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *ci,
                    const VkAllocationCallbacks *allocator, VkCommandPool *out) {
    auto real =
        (PFN_vkCreateCommandPool)get_real_proc(get_last_instance(), device, "vkCreateCommandPool");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto result = real(device, ci, allocator, out);
    if (result == VK_SUCCESS && out && ci)
        VertexStoreEmulator::get().pool(device, *out, ci->queueFamilyIndex);
    return result;
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *allocator) {
    LayerManager::get().track_command_pool_reset(device, pool, true);
    VertexStoreEmulator::get().destroy_pool(device, pool);
    auto real = (PFN_vkDestroyCommandPool)get_real_proc(get_last_instance(), device,
                                                        "vkDestroyCommandPool");
    if (real)
        real(device, pool, allocator);
}
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device,
                                                                  VkCommandPool pool,
                                                                  VkCommandPoolResetFlags flags) {
    auto real =
        (PFN_vkResetCommandPool)get_real_proc(get_last_instance(), device, "vkResetCommandPool");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto result = real(device, pool, flags);
    if (result == VK_SUCCESS) {
        VertexStoreEmulator::get().clear_pool(device, pool);
        LayerManager::get().track_command_pool_reset(device, pool, false);
    }
    return result;
}
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cmd) {
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    auto real =
        (PFN_vkEndCommandBuffer)get_real_proc(get_last_instance(), device, "vkEndCommandBuffer");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto result = real(cmd);
    if (result != VK_SUCCESS)
        return result;
    auto descriptor_result = LayerManager::get().command_recording_error(cmd);
    return descriptor_result != VK_SUCCESS ? descriptor_result :
        VertexStoreEmulator::get().recording_error(cmd);
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(
    VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
    uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *offsets) {
    LayerManager::get().track_descriptor_binding(cmd, point, layout, first, count);
    VertexStoreEmulator::get().bind_sets(cmd, point, layout, first, count, sets, dynamic_count,
                                         offsets);
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    auto real = (PFN_vkCmdBindDescriptorSets)get_real_proc(get_last_instance(), device,
                                                           "vkCmdBindDescriptorSets");
    if (real)
        real(cmd, point, layout, first, count, sets, dynamic_count, offsets);
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer cmd,
                                                                VkBuffer buffer,
                                                                VkDeviceSize offset,
                                                                VkIndexType type) {
    VertexStoreEmulator::get().bind_index(cmd, buffer, offset, type);
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    auto real = (PFN_vkCmdBindIndexBuffer)get_real_proc(get_last_instance(), device,
                                                        "vkCmdBindIndexBuffer");
    if (real)
        real(cmd, buffer, offset, type);
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cmd,
                                                              VkPipelineLayout layout,
                                                              VkShaderStageFlags stages,
                                                              uint32_t offset, uint32_t size,
                                                              const void *data) {
    VertexStoreEmulator::get().push(cmd, layout, stages, offset, size, data);
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    auto real =
        (PFN_vkCmdPushConstants)get_real_proc(get_last_instance(), device, "vkCmdPushConstants");
    if (real)
        real(cmd, layout, stages, offset, size, data);
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer cmd, VkQueryPool pool,
                                                           uint32_t query,
                                                           VkQueryControlFlags flags) {
    VertexStoreEmulator::get().query(cmd, true);
    LayerManager::get().block_tiler_split(cmd, true);
    TBDRBarrierOptimizer::get().on_cmd_action(cmd);
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    auto real = (PFN_vkCmdBeginQuery)get_real_proc(get_last_instance(), device, "vkCmdBeginQuery");
    if (real)
        real(cmd, pool, query, flags);
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer cmd, VkQueryPool pool,
                                                         uint32_t query) {
    VertexStoreEmulator::get().query(cmd, false);
    LayerManager::get().block_tiler_split(cmd, false);
    TBDRBarrierOptimizer::get().on_cmd_action(cmd);
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    auto real = (PFN_vkCmdEndQuery)get_real_proc(get_last_instance(), device, "vkCmdEndQuery");
    if (real)
        real(cmd, pool, query);
}

// These scopes cannot cross a physical render-pass boundary. Keep the tiler
// budget pending until all nested query/feedback/conditional scopes have ended.
#define TILER_SCOPE_COMMAND(name, beginning, params, arguments) \
    VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL name params { \
        auto& manager = LayerManager::get(); \
        TBDRBarrierOptimizer::get().on_cmd_action(cmd); \
        if (beginning) manager.block_tiler_split(cmd, true); \
        auto real = (PFN_##name)get_real_proc(get_last_instance(), \
            manager.get_device_for_cmd(cmd), #name); \
        if (real) real arguments; \
        if (!beginning) manager.block_tiler_split(cmd, false); \
    }
TILER_SCOPE_COMMAND(vkCmdBeginQueryIndexedEXT, true,
    (VkCommandBuffer cmd, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags, uint32_t index),
    (cmd, pool, query, flags, index))
TILER_SCOPE_COMMAND(vkCmdEndQueryIndexedEXT, false,
    (VkCommandBuffer cmd, VkQueryPool pool, uint32_t query, uint32_t index),
    (cmd, pool, query, index))
TILER_SCOPE_COMMAND(vkCmdBeginTransformFeedbackEXT, true,
    (VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets),
    (cmd, first, count, buffers, offsets))
TILER_SCOPE_COMMAND(vkCmdEndTransformFeedbackEXT, false,
    (VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets),
    (cmd, first, count, buffers, offsets))
#undef TILER_SCOPE_COMMAND

namespace {
void track_sets2(VkCommandBuffer cmd, const VkBindDescriptorSetsInfo *info) {
    if (!info)
        return;
    auto &emulator = VertexStoreEmulator::get();
    if (info->pNext)
        emulator.unsupported(cmd, "descriptor binding extensions are unsupported");
    for (auto point : {VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE}) {
        const auto mask = point == VK_PIPELINE_BIND_POINT_GRAPHICS ? VK_SHADER_STAGE_ALL_GRAPHICS
                                                                   : VK_SHADER_STAGE_COMPUTE_BIT;
        if (info->stageFlags & mask) {
            LayerManager::get().track_descriptor_binding(cmd, point, info->layout,
                info->firstSet, info->descriptorSetCount);
            emulator.bind_sets(cmd, point, info->layout, info->firstSet, info->descriptorSetCount,
                               info->pDescriptorSets, info->dynamicOffsetCount,
                               info->pDynamicOffsets);
        }
    }
}
void track_push2(VkCommandBuffer cmd, const VkPushConstantsInfo *info) {
    if (!info)
        return;
    if (info->pNext)
        VertexStoreEmulator::get().unsupported(cmd, "push constant extensions are unsupported");
    VertexStoreEmulator::get().push(cmd, info->layout, info->stageFlags, info->offset, info->size,
                                    info->pValues);
}
} // namespace
#define VERTEX_STORE_BIND2(suffix)                                                                 \
    VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2##suffix(                   \
        VkCommandBuffer cmd, const VkBindDescriptorSetsInfo *info) {                               \
        track_sets2(cmd, info);                                                                    \
        auto device = LayerManager::get().get_device_for_cmd(cmd);                                 \
        auto real = (PFN_vkCmdBindDescriptorSets2)get_real_proc(                                   \
            get_last_instance(), device, "vkCmdBindDescriptorSets2" #suffix);                      \
        if (real)                                                                                  \
            real(cmd, info);                                                                       \
    }                                                                                              \
    VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2##suffix(                        \
        VkCommandBuffer cmd, const VkPushConstantsInfo *info) {                                    \
        track_push2(cmd, info);                                                                    \
        auto device = LayerManager::get().get_device_for_cmd(cmd);                                 \
        auto real = (PFN_vkCmdPushConstants2)get_real_proc(get_last_instance(), device,            \
                                                           "vkCmdPushConstants2" #suffix);         \
        if (real)                                                                                  \
            real(cmd, info);                                                                       \
    }                                                                                              \
    VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer2##suffix(                      \
        VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,              \
        VkIndexType type) {                                                                        \
        VertexStoreEmulator::get().bind_index(cmd, buffer, offset, type);                          \
        auto device = LayerManager::get().get_device_for_cmd(cmd);                                 \
        auto real = (PFN_vkCmdBindIndexBuffer2)get_real_proc(get_last_instance(), device,          \
                                                             "vkCmdBindIndexBuffer2" #suffix);     \
        if (real)                                                                                  \
            real(cmd, buffer, offset, size, type);                                                 \
    }
VERTEX_STORE_BIND2()
VERTEX_STORE_BIND2(KHR)
#undef VERTEX_STORE_BIND2
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginConditionalRenderingEXT(
    VkCommandBuffer cmd, const VkConditionalRenderingBeginInfoEXT *info) {
    VertexStoreEmulator::get().query(cmd, true);
    LayerManager::get().block_tiler_split(cmd, true);
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    auto real = (PFN_vkCmdBeginConditionalRenderingEXT)get_real_proc(
        get_last_instance(), device, "vkCmdBeginConditionalRenderingEXT");
    if (real)
        real(cmd, info);
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndConditionalRenderingEXT(VkCommandBuffer cmd) {
    VertexStoreEmulator::get().query(cmd, false);
    LayerManager::get().block_tiler_split(cmd, false);
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    auto real = (PFN_vkCmdEndConditionalRenderingEXT)get_real_proc(
        get_last_instance(), device, "vkCmdEndConditionalRenderingEXT");
    if (real)
        real(cmd);
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawMultiEXT(VkCommandBuffer cmd, uint32_t count,
                                                             const VkMultiDrawInfoEXT *info,
                                                             uint32_t instances,
                                                             uint32_t first_instance,
                                                             uint32_t stride) {
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    if (VertexStoreEmulator::get().enabled(device)) {
        VertexStoreEmulator::get().unsupported(
            cmd, "VK_EXT_multi_draw is not implemented by vertex compute/replay");
        return;
    }
    auto real =
        (PFN_vkCmdDrawMultiEXT)get_real_proc(get_last_instance(), device, "vkCmdDrawMultiEXT");
    if (real)
        real(cmd, count, info, instances, first_instance, stride);
}
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawMultiIndexedEXT(
    VkCommandBuffer cmd, uint32_t count, const VkMultiDrawIndexedInfoEXT *info, uint32_t instances,
    uint32_t first_instance, uint32_t stride, const int32_t *vertex_offset) {
    auto device = LayerManager::get().get_device_for_cmd(cmd);
    if (VertexStoreEmulator::get().enabled(device)) {
        VertexStoreEmulator::get().unsupported(
            cmd, "VK_EXT_multi_draw is not implemented by vertex compute/replay");
        return;
    }
    auto real = (PFN_vkCmdDrawMultiIndexedEXT)get_real_proc(get_last_instance(), device,
                                                            "vkCmdDrawMultiIndexedEXT");
    if (real)
        real(cmd, count, info, instances, first_instance, stride, vertex_offset);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShadersEXT(VkDevice device, uint32_t count, const VkShaderCreateInfoEXT *infos,
                   const VkAllocationCallbacks *allocator, VkShaderEXT *shaders) {
    if (VertexStoreEmulator::get().enabled(device)) {
        if (shaders)
            for (uint32_t i = 0; i < count; ++i)
                shaders[i] = VK_NULL_HANDLE;
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    auto real =
        (PFN_vkCreateShadersEXT)get_real_proc(get_last_instance(), device, "vkCreateShadersEXT");
    return real ? real(device, count, infos, allocator, shaders) : VK_ERROR_FEATURE_NOT_PRESENT;
}

#include "vk_common.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include <string.h>
#include <string>
#include <dirent.h>
#include <sys/stat.h>

static void repair_options_file(const std::string& filepath) {
    FILE* f = fopen(filepath.c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return; }

    std::string content(sz, '\0');
    if (fread(&content[0], 1, sz, f) != (size_t)sz) { fclose(f); return; }
    fclose(f);

    bool modified = false;

    // 1. Fix graphicsApiPreference
    size_t pos = content.find("graphicsApiPreference:");
    if (pos != std::string::npos) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.length();
        std::string line = content.substr(pos, eol - pos);
        if (line.find("prefer_vulkan") == std::string::npos) {
            content.replace(pos, eol - pos, "graphicsApiPreference:prefer_vulkan");
            modified = true;
            LOGI("Repaired %s: set graphicsApiPreference:prefer_vulkan", filepath.c_str());
        }
    } else {
        if (!content.empty() && content.back() != '\n') content += '\n';
        content += "graphicsApiPreference:prefer_vulkan\n";
        modified = true;
        LOGI("Appended to %s: graphicsApiPreference:prefer_vulkan", filepath.c_str());
    }

    // 2. Fix graphicsApi
    pos = content.find("graphicsApi:");
    if (pos != std::string::npos) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.length();
        std::string line = content.substr(pos, eol - pos);
        if (line.find("vulkan") == std::string::npos) {
            content.replace(pos, eol - pos, "graphicsApi:vulkan");
            modified = true;
            LOGI("Repaired %s: set graphicsApi:vulkan", filepath.c_str());
        }
    }

    if (modified) {
        f = fopen(filepath.c_str(), "wb");
        if (f) {
            fwrite(content.data(), 1, content.size(), f);
            fclose(f);
            LOGI("Successfully saved repaired options to %s", filepath.c_str());
        }
    }
}

static void check_and_repair_options() {
    repair_options_file("options.txt");
    repair_options_file("../options.txt");

    DIR* vdir = opendir("versions");
    if (vdir) {
        struct dirent* entry;
        while ((entry = readdir(vdir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            std::string path = std::string("versions/") + entry->d_name + "/options.txt";
            repair_options_file(path);
        }
        closedir(vdir);
    }

    const char* home = getenv("HOME");
    if (home) {
        std::string mc_dir = std::string(home) + "/.minecraft";
        repair_options_file(mc_dir + "/options.txt");

        std::string versions_dir = mc_dir + "/versions";
        DIR* dir = opendir(versions_dir.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                std::string path = versions_dir + "/" + entry->d_name + "/options.txt";
                repair_options_file(path);
            }
            closedir(dir);
        }
    }
}

__attribute__((constructor))
static void init_vulkan_layer() {
    LOGI("Initializing Vulkan Layer (Modular Extension & Feature Supplement)...");
    init_real_vulkan();

    auto& manager = LayerManager::get();
    manager.init_registered_modules();

    register_vulkan_ptr();
    check_and_repair_options();
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

    VkResult res = real_fn(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS && pInstance && *pInstance != VK_NULL_HANDLE) {
        set_last_instance(*pInstance);
        LOGI("Created VkInstance %p", *pInstance);
    }
    return res;
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator
) {
    PFN_vkDestroyInstance real_fn =
        (PFN_vkDestroyInstance) get_real_proc(instance, VK_NULL_HANDLE, "vkDestroyInstance");
    if (real_fn) {
        real_fn(instance, pAllocator);
    }
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
// Dispatchers: vkGetInstanceProcAddr & vkGetDeviceProcAddr
// ============================================================================

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName
) {
    if (!pName) return NULL;

    PFN_vkVoidFunction custom_proc = LayerManager::get().get_custom_proc(pName);
    if (custom_proc) return custom_proc;

    #define MATCH_FUNC(name) if (strcmp(pName, #name) == 0) return (PFN_vkVoidFunction) name

    MATCH_FUNC(vkGetInstanceProcAddr);
    MATCH_FUNC(vkGetDeviceProcAddr);
    MATCH_FUNC(vkCreateInstance);
    MATCH_FUNC(vkDestroyInstance);
    MATCH_FUNC(vkCreateDevice);
    MATCH_FUNC(vkDestroyDevice);
    MATCH_FUNC(vkEnumerateDeviceExtensionProperties);
    MATCH_FUNC(vkGetPhysicalDeviceFeatures);
    MATCH_FUNC(vkGetPhysicalDeviceFeatures2);
    MATCH_FUNC(vkGetPhysicalDeviceFeatures2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceProperties2KHR);
    MATCH_FUNC(vkCreateGraphicsPipelines);
    MATCH_FUNC(vkCreateDescriptorSetLayout);
    MATCH_FUNC(vkDestroyDescriptorSetLayout);
    MATCH_FUNC(vkCreatePipelineLayout);
    MATCH_FUNC(vkDestroyPipelineLayout);
    MATCH_FUNC(vkAllocateCommandBuffers);
    MATCH_FUNC(vkFreeCommandBuffers);
    MATCH_FUNC(vkBeginCommandBuffer);
    MATCH_FUNC(vkResetCommandBuffer);
    MATCH_FUNC(vkCreateDescriptorUpdateTemplate);
    MATCH_FUNC(vkCreateDescriptorUpdateTemplateKHR);
    MATCH_FUNC(vkDestroyDescriptorUpdateTemplate);
    MATCH_FUNC(vkDestroyDescriptorUpdateTemplateKHR);
    MATCH_FUNC(vkCmdPushDescriptorSetKHR);
    MATCH_FUNC(vkCmdPushDescriptorSetWithTemplateKHR);
    MATCH_FUNC(vkCreateImage);
    MATCH_FUNC(vkDestroyImage);
    MATCH_FUNC(vkCreateImageView);
    MATCH_FUNC(vkDestroyImageView);
    MATCH_FUNC(vkCmdBeginRendering);
    MATCH_FUNC(vkCmdBeginRenderingKHR);
    MATCH_FUNC(vkCmdEndRendering);
    MATCH_FUNC(vkCmdEndRenderingKHR);
    MATCH_FUNC(vkDestroyPipeline);
    MATCH_FUNC(vkCmdBindPipeline);
    MATCH_FUNC(vkCmdBindVertexBuffers);
    MATCH_FUNC(vkCmdBindVertexBuffers2);
    MATCH_FUNC(vkCmdBindVertexBuffers2EXT);
    MATCH_FUNC(vkCmdDraw);
    MATCH_FUNC(vkCmdDrawIndexed);
    MATCH_FUNC(vkGetDeviceQueue);
    MATCH_FUNC(vkGetDeviceQueue2);
    MATCH_FUNC(vkCmdSetEvent2);
    MATCH_FUNC(vkCmdSetEvent2KHR);
    MATCH_FUNC(vkCmdResetEvent2);
    MATCH_FUNC(vkCmdResetEvent2KHR);
    MATCH_FUNC(vkCmdWaitEvents2);
    MATCH_FUNC(vkCmdWaitEvents2KHR);
    MATCH_FUNC(vkCmdPipelineBarrier2);
    MATCH_FUNC(vkCmdPipelineBarrier2KHR);
    MATCH_FUNC(vkCmdWriteTimestamp2);
    MATCH_FUNC(vkCmdWriteTimestamp2KHR);
    MATCH_FUNC(vkQueueSubmit2);
    MATCH_FUNC(vkQueueSubmit2KHR);
    MATCH_FUNC(vkEnumerateInstanceVersion);
    MATCH_FUNC(vkGetPhysicalDeviceProperties);
    MATCH_FUNC(vkQueueSubmit);
    MATCH_FUNC(vkQueueWaitIdle);
    MATCH_FUNC(vkDeviceWaitIdle);
    MATCH_FUNC(vkCreateSemaphore);
    MATCH_FUNC(vkDestroySemaphore);
    MATCH_FUNC(vkGetSemaphoreCounterValue);
    MATCH_FUNC(vkGetSemaphoreCounterValueKHR);
    MATCH_FUNC(vkWaitSemaphores);
    MATCH_FUNC(vkWaitSemaphoresKHR);
    MATCH_FUNC(vkSignalSemaphore);
    MATCH_FUNC(vkSignalSemaphoreKHR);
    MATCH_FUNC(vkResetQueryPool);
    MATCH_FUNC(vkResetQueryPoolEXT);
    MATCH_FUNC(vkCreateRenderPass2);
    MATCH_FUNC(vkCreateRenderPass2KHR);
    MATCH_FUNC(vkCmdBeginRenderPass2);
    MATCH_FUNC(vkCmdBeginRenderPass2KHR);
    MATCH_FUNC(vkCmdNextSubpass2);
    MATCH_FUNC(vkCmdNextSubpass2KHR);
    MATCH_FUNC(vkCmdEndRenderPass2);
    MATCH_FUNC(vkCmdEndRenderPass2KHR);
    MATCH_FUNC(vkCmdDrawIndirectCount);
    MATCH_FUNC(vkCmdDrawIndirectCountKHR);
    MATCH_FUNC(vkCmdDrawIndirectCountAMD);
    MATCH_FUNC(vkCmdDrawIndexedIndirectCount);
    MATCH_FUNC(vkCmdDrawIndexedIndirectCountKHR);
    MATCH_FUNC(vkCmdDrawIndexedIndirectCountAMD);
    MATCH_FUNC(vkGetBufferDeviceAddress);
    MATCH_FUNC(vkGetBufferDeviceAddressKHR);
    MATCH_FUNC(vkGetBufferDeviceAddressEXT);
    MATCH_FUNC(vkGetBufferOpaqueCaptureAddress);
    MATCH_FUNC(vkGetBufferOpaqueCaptureAddressKHR);
    MATCH_FUNC(vkGetDeviceMemoryOpaqueCaptureAddress);
    MATCH_FUNC(vkGetDeviceMemoryOpaqueCaptureAddressKHR);

    // Vulkan 1.1 Core / Promoted Entry Points
    MATCH_FUNC(vkBindBufferMemory2);
    MATCH_FUNC(vkBindBufferMemory2KHR);
    MATCH_FUNC(vkBindImageMemory2);
    MATCH_FUNC(vkBindImageMemory2KHR);
    MATCH_FUNC(vkGetBufferMemoryRequirements2);
    MATCH_FUNC(vkGetBufferMemoryRequirements2KHR);
    MATCH_FUNC(vkGetImageMemoryRequirements2);
    MATCH_FUNC(vkGetImageMemoryRequirements2KHR);
    MATCH_FUNC(vkGetImageSparseMemoryRequirements2);
    MATCH_FUNC(vkGetImageSparseMemoryRequirements2KHR);
    MATCH_FUNC(vkUpdateDescriptorSetWithTemplate);
    MATCH_FUNC(vkUpdateDescriptorSetWithTemplateKHR);
    MATCH_FUNC(vkGetDescriptorSetLayoutSupport);
    MATCH_FUNC(vkGetDescriptorSetLayoutSupportKHR);
    MATCH_FUNC(vkTrimCommandPool);
    MATCH_FUNC(vkTrimCommandPoolKHR);
    MATCH_FUNC(vkCmdDispatchBase);
    MATCH_FUNC(vkCmdDispatchBaseKHR);
    MATCH_FUNC(vkCmdSetDeviceMask);
    MATCH_FUNC(vkCmdSetDeviceMaskKHR);
    MATCH_FUNC(vkGetDeviceGroupPeerMemoryFeatures);
    MATCH_FUNC(vkGetDeviceGroupPeerMemoryFeaturesKHR);
    MATCH_FUNC(vkEnumeratePhysicalDeviceGroups);
    MATCH_FUNC(vkEnumeratePhysicalDeviceGroupsKHR);
    MATCH_FUNC(vkGetPhysicalDeviceFormatProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceFormatProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceImageFormatProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceImageFormatProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceQueueFamilyProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceQueueFamilyProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceMemoryProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceMemoryProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceSparseImageFormatProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceSparseImageFormatProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceExternalBufferProperties);
    MATCH_FUNC(vkGetPhysicalDeviceExternalBufferPropertiesKHR);
    MATCH_FUNC(vkGetPhysicalDeviceExternalFenceProperties);
    MATCH_FUNC(vkGetPhysicalDeviceExternalFencePropertiesKHR);
    MATCH_FUNC(vkGetPhysicalDeviceExternalSemaphoreProperties);
    MATCH_FUNC(vkGetPhysicalDeviceExternalSemaphorePropertiesKHR);
    MATCH_FUNC(vkCreateSamplerYcbcrConversion);
    MATCH_FUNC(vkCreateSamplerYcbcrConversionKHR);
    MATCH_FUNC(vkDestroySamplerYcbcrConversion);
    MATCH_FUNC(vkDestroySamplerYcbcrConversionKHR);

    #undef MATCH_FUNC

    init_real_vulkan();
    PFN_vkGetInstanceProcAddr real_gipa = get_real_instance_proc_addr();
    if (real_gipa) {
        return real_gipa(instance, pName);
    }
    return NULL;
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName
) {
    if (!pName) return NULL;

    PFN_vkVoidFunction custom_proc = LayerManager::get().get_custom_proc(pName);
    if (custom_proc) return custom_proc;

    #define MATCH_FUNC(name) if (strcmp(pName, #name) == 0) return (PFN_vkVoidFunction) name

    MATCH_FUNC(vkGetDeviceProcAddr);
    MATCH_FUNC(vkDestroyDevice);
    MATCH_FUNC(vkCreateGraphicsPipelines);
    MATCH_FUNC(vkCreateDescriptorSetLayout);
    MATCH_FUNC(vkDestroyDescriptorSetLayout);
    MATCH_FUNC(vkCreatePipelineLayout);
    MATCH_FUNC(vkDestroyPipelineLayout);
    MATCH_FUNC(vkAllocateCommandBuffers);
    MATCH_FUNC(vkFreeCommandBuffers);
    MATCH_FUNC(vkBeginCommandBuffer);
    MATCH_FUNC(vkResetCommandBuffer);
    MATCH_FUNC(vkCreateDescriptorUpdateTemplate);
    MATCH_FUNC(vkCreateDescriptorUpdateTemplateKHR);
    MATCH_FUNC(vkDestroyDescriptorUpdateTemplate);
    MATCH_FUNC(vkDestroyDescriptorUpdateTemplateKHR);
    MATCH_FUNC(vkCmdPushDescriptorSetKHR);
    MATCH_FUNC(vkCmdPushDescriptorSetWithTemplateKHR);
    MATCH_FUNC(vkCreateImage);
    MATCH_FUNC(vkDestroyImage);
    MATCH_FUNC(vkCreateImageView);
    MATCH_FUNC(vkDestroyImageView);
    MATCH_FUNC(vkCmdBeginRendering);
    MATCH_FUNC(vkCmdBeginRenderingKHR);
    MATCH_FUNC(vkCmdEndRendering);
    MATCH_FUNC(vkCmdEndRenderingKHR);
    MATCH_FUNC(vkDestroyPipeline);
    MATCH_FUNC(vkCmdBindPipeline);
    MATCH_FUNC(vkCmdBindVertexBuffers);
    MATCH_FUNC(vkCmdBindVertexBuffers2);
    MATCH_FUNC(vkCmdBindVertexBuffers2EXT);
    MATCH_FUNC(vkCmdDraw);
    MATCH_FUNC(vkCmdDrawIndexed);
    MATCH_FUNC(vkGetDeviceQueue);
    MATCH_FUNC(vkGetDeviceQueue2);
    MATCH_FUNC(vkCmdSetEvent2);
    MATCH_FUNC(vkCmdSetEvent2KHR);
    MATCH_FUNC(vkCmdResetEvent2);
    MATCH_FUNC(vkCmdResetEvent2KHR);
    MATCH_FUNC(vkCmdWaitEvents2);
    MATCH_FUNC(vkCmdWaitEvents2KHR);
    MATCH_FUNC(vkCmdPipelineBarrier2);
    MATCH_FUNC(vkCmdPipelineBarrier2KHR);
    MATCH_FUNC(vkCmdWriteTimestamp2);
    MATCH_FUNC(vkCmdWriteTimestamp2KHR);
    MATCH_FUNC(vkQueueSubmit2);
    MATCH_FUNC(vkQueueSubmit2KHR);
    MATCH_FUNC(vkQueueSubmit);
    MATCH_FUNC(vkQueueWaitIdle);
    MATCH_FUNC(vkDeviceWaitIdle);
    MATCH_FUNC(vkCreateSemaphore);
    MATCH_FUNC(vkDestroySemaphore);
    MATCH_FUNC(vkGetSemaphoreCounterValue);
    MATCH_FUNC(vkGetSemaphoreCounterValueKHR);
    MATCH_FUNC(vkWaitSemaphores);
    MATCH_FUNC(vkWaitSemaphoresKHR);
    MATCH_FUNC(vkSignalSemaphore);
    MATCH_FUNC(vkSignalSemaphoreKHR);
    MATCH_FUNC(vkResetQueryPool);
    MATCH_FUNC(vkResetQueryPoolEXT);
    MATCH_FUNC(vkCreateRenderPass2);
    MATCH_FUNC(vkCreateRenderPass2KHR);
    MATCH_FUNC(vkCmdBeginRenderPass2);
    MATCH_FUNC(vkCmdBeginRenderPass2KHR);
    MATCH_FUNC(vkCmdNextSubpass2);
    MATCH_FUNC(vkCmdNextSubpass2KHR);
    MATCH_FUNC(vkCmdEndRenderPass2);
    MATCH_FUNC(vkCmdEndRenderPass2KHR);
    MATCH_FUNC(vkCmdDrawIndirectCount);
    MATCH_FUNC(vkCmdDrawIndirectCountKHR);
    MATCH_FUNC(vkCmdDrawIndirectCountAMD);
    MATCH_FUNC(vkCmdDrawIndexedIndirectCount);
    MATCH_FUNC(vkCmdDrawIndexedIndirectCountKHR);
    MATCH_FUNC(vkCmdDrawIndexedIndirectCountAMD);
    MATCH_FUNC(vkGetBufferDeviceAddress);
    MATCH_FUNC(vkGetBufferDeviceAddressKHR);
    MATCH_FUNC(vkGetBufferDeviceAddressEXT);
    MATCH_FUNC(vkGetBufferOpaqueCaptureAddress);
    MATCH_FUNC(vkGetBufferOpaqueCaptureAddressKHR);
    MATCH_FUNC(vkGetDeviceMemoryOpaqueCaptureAddress);
    MATCH_FUNC(vkGetDeviceMemoryOpaqueCaptureAddressKHR);

    // Vulkan 1.1 Core / Promoted Entry Points
    MATCH_FUNC(vkBindBufferMemory2);
    MATCH_FUNC(vkBindBufferMemory2KHR);
    MATCH_FUNC(vkBindImageMemory2);
    MATCH_FUNC(vkBindImageMemory2KHR);
    MATCH_FUNC(vkGetBufferMemoryRequirements2);
    MATCH_FUNC(vkGetBufferMemoryRequirements2KHR);
    MATCH_FUNC(vkGetImageMemoryRequirements2);
    MATCH_FUNC(vkGetImageMemoryRequirements2KHR);
    MATCH_FUNC(vkGetImageSparseMemoryRequirements2);
    MATCH_FUNC(vkGetImageSparseMemoryRequirements2KHR);
    MATCH_FUNC(vkUpdateDescriptorSetWithTemplate);
    MATCH_FUNC(vkUpdateDescriptorSetWithTemplateKHR);
    MATCH_FUNC(vkGetDescriptorSetLayoutSupport);
    MATCH_FUNC(vkGetDescriptorSetLayoutSupportKHR);
    MATCH_FUNC(vkTrimCommandPool);
    MATCH_FUNC(vkTrimCommandPoolKHR);
    MATCH_FUNC(vkCmdDispatchBase);
    MATCH_FUNC(vkCmdDispatchBaseKHR);
    MATCH_FUNC(vkCmdSetDeviceMask);
    MATCH_FUNC(vkCmdSetDeviceMaskKHR);
    MATCH_FUNC(vkGetDeviceGroupPeerMemoryFeatures);
    MATCH_FUNC(vkGetDeviceGroupPeerMemoryFeaturesKHR);
    MATCH_FUNC(vkEnumeratePhysicalDeviceGroups);
    MATCH_FUNC(vkEnumeratePhysicalDeviceGroupsKHR);
    MATCH_FUNC(vkGetPhysicalDeviceFormatProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceFormatProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceImageFormatProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceImageFormatProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceQueueFamilyProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceQueueFamilyProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceMemoryProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceMemoryProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceSparseImageFormatProperties2);
    MATCH_FUNC(vkGetPhysicalDeviceSparseImageFormatProperties2KHR);
    MATCH_FUNC(vkGetPhysicalDeviceExternalBufferProperties);
    MATCH_FUNC(vkGetPhysicalDeviceExternalBufferPropertiesKHR);
    MATCH_FUNC(vkGetPhysicalDeviceExternalFenceProperties);
    MATCH_FUNC(vkGetPhysicalDeviceExternalFencePropertiesKHR);
    MATCH_FUNC(vkGetPhysicalDeviceExternalSemaphoreProperties);
    MATCH_FUNC(vkGetPhysicalDeviceExternalSemaphorePropertiesKHR);
    MATCH_FUNC(vkCreateSamplerYcbcrConversion);
    MATCH_FUNC(vkCreateSamplerYcbcrConversionKHR);
    MATCH_FUNC(vkDestroySamplerYcbcrConversion);
    MATCH_FUNC(vkDestroySamplerYcbcrConversionKHR);

    #undef MATCH_FUNC

    init_real_vulkan();
    PFN_vkGetDeviceProcAddr real_gdpa = get_real_device_proc_addr();
    if (real_gdpa && device != VK_NULL_HANDLE) {
        return real_gdpa(device, pName);
    }

    PFN_vkGetInstanceProcAddr real_gipa = get_real_instance_proc_addr();
    if (real_gipa) {
        return real_gipa(get_last_instance(), pName);
    }
    return NULL;
}

// ============================================================================
// Standard Vulkan 1.0 Forwarding Stubs
// ============================================================================

#define FORWARD_INST(ret, name, args, params) \
    VK_LAYER_EXPORT ret VKAPI_CALL name args { \
        typedef ret (VKAPI_PTR *fn_t) args; \
        fn_t fn = (fn_t) get_real_proc(get_last_instance(), VK_NULL_HANDLE, #name); \
        if (fn) return fn params; \
        return (ret)0; \
    }

#define FORWARD_DEV(ret, name, dev_arg, args, params) \
    VK_LAYER_EXPORT ret VKAPI_CALL name args { \
        typedef ret (VKAPI_PTR *fn_t) args; \
        fn_t fn = (fn_t) get_real_proc(get_last_instance(), dev_arg, #name); \
        if (fn) return fn params; \
        return (ret)0; \
    }

#define FORWARD_DEV_VOID(name, dev_arg, args, params) \
    VK_LAYER_EXPORT void VKAPI_CALL name args { \
        typedef void (VKAPI_PTR *fn_t) args; \
        fn_t fn = (fn_t) get_real_proc(get_last_instance(), dev_arg, #name); \
        if (fn) fn params; \
    }

#define FORWARD_QUEUE(ret, name, args, params) \
    VK_LAYER_EXPORT ret VKAPI_CALL name args { \
        typedef ret (VKAPI_PTR *fn_t) args; \
        fn_t fn = (fn_t) get_real_proc(get_last_instance(), VK_NULL_HANDLE, #name); \
        if (fn) return fn params; \
        return (ret)0; \
    }

FORWARD_INST(VkResult, vkEnumeratePhysicalDevices, (VkInstance instance, uint32_t* pCount, VkPhysicalDevice* pDevices), (instance, pCount, pDevices))
FORWARD_INST(VkResult, vkEnumerateInstanceExtensionProperties, (const char* pLayer, uint32_t* pCount, VkExtensionProperties* pProps), (pLayer, pCount, pProps))
FORWARD_INST(VkResult, vkEnumerateInstanceLayerProperties, (uint32_t* pCount, VkLayerProperties* pProps), (pCount, pProps))
VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumerateInstanceVersion(
    uint32_t* pApiVersion
) {
    if (!pApiVersion) return VK_ERROR_INITIALIZATION_FAILED;
    init_real_vulkan();
    typedef VkResult (VKAPI_PTR *PFN_vkEnumerateInstanceVersion)(uint32_t*);
    PFN_vkEnumerateInstanceVersion real_fn =
        (PFN_vkEnumerateInstanceVersion) get_real_proc(VK_NULL_HANDLE, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (real_fn) {
        VkResult res = real_fn(pApiVersion);
        if (res == VK_SUCCESS && *pApiVersion < VK_API_VERSION_1_2) {
            *pApiVersion = VK_API_VERSION_1_2;
        }
        return res;
    }
    *pApiVersion = VK_API_VERSION_1_2;
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
FORWARD_DEV(VkResult, vkMapMemory, device, (VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** ppData), (device, memory, offset, size, flags, ppData))
FORWARD_DEV_VOID(vkUnmapMemory, device, (VkDevice device, VkDeviceMemory memory), (device, memory))
FORWARD_DEV(VkResult, vkCreateBuffer, device, (VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer), (device, pCreateInfo, pAllocator, pBuffer))
FORWARD_DEV_VOID(vkDestroyBuffer, device, (VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator), (device, buffer, pAllocator))
FORWARD_DEV(VkResult, vkCreateShaderModule, device, (VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule), (device, pCreateInfo, pAllocator, pShaderModule))
FORWARD_DEV_VOID(vkDestroyShaderModule, device, (VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks* pAllocator), (device, shaderModule, pAllocator))
FORWARD_DEV(VkResult, vkCreateRenderPass, device, (VkDevice device, const VkRenderPassCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass), (device, pCreateInfo, pAllocator, pRenderPass))
FORWARD_DEV_VOID(vkDestroyRenderPass, device, (VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator), (device, renderPass, pAllocator))
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
    return LayerManager::get().dispatch_enumerate_physical_device_groups(
        instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
}
VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroupsKHR(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties
) {
    return LayerManager::get().dispatch_enumerate_physical_device_groups(
        instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
}

// Physical Device Properties 2
VK_LAYER_EXPORT void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkFormatProperties2* pFormatProperties
) {
    PFN_vkGetPhysicalDeviceFormatProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceFormatProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFormatProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceFormatProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFormatProperties2KHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, format, pFormatProperties);
        return;
    }
    if (pFormatProperties) {
        PFN_vkGetPhysicalDeviceFormatProperties real_fp =
            (PFN_vkGetPhysicalDeviceFormatProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFormatProperties");
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
        (PFN_vkGetPhysicalDeviceImageFormatProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceImageFormatProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    }
    if (real_fn) {
        return real_fn(physicalDevice, pImageFormatInfo, pImageFormatProperties);
    }
    if (!pImageFormatInfo || !pImageFormatProperties) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetPhysicalDeviceImageFormatProperties real_ifp =
        (PFN_vkGetPhysicalDeviceImageFormatProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceImageFormatProperties");
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
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceQueueFamilyProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
        return;
    }

    if (!pQueueFamilyPropertyCount) return;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties real_qfp =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!real_qfp) return;

    if (!pQueueFamilyProperties) {
        real_qfp(physicalDevice, pQueueFamilyPropertyCount, nullptr);
        return;
    }

    uint32_t count = *pQueueFamilyPropertyCount;
    std::vector<VkQueueFamilyProperties> nativeProps(count);
    real_qfp(physicalDevice, &count, nativeProps.data());
    for (uint32_t i = 0; i < count; ++i) {
        pQueueFamilyProperties[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        pQueueFamilyProperties[i].pNext = nullptr;
        pQueueFamilyProperties[i].queueFamilyProperties = nativeProps[i];
    }
    *pQueueFamilyPropertyCount = count;
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
        (PFN_vkGetPhysicalDeviceMemoryProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceMemoryProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties2KHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, pMemoryProperties);
        return;
    }
    if (pMemoryProperties) {
        PFN_vkGetPhysicalDeviceMemoryProperties real_mp =
            (PFN_vkGetPhysicalDeviceMemoryProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties");
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
        (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSparseImageFormatProperties2");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR");
    }
    if (real_fn) {
        real_fn(physicalDevice, pFormatInfo, pPropertyCount, pProperties);
        return;
    }
    if (!pFormatInfo || !pPropertyCount) return;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties real_sfp =
        (PFN_vkGetPhysicalDeviceSparseImageFormatProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSparseImageFormatProperties");
    if (!real_sfp) {
        *pPropertyCount = 0;
        return;
    }
    if (!pProperties) {
        real_sfp(physicalDevice, pFormatInfo->format, pFormatInfo->type, pFormatInfo->samples, pFormatInfo->usage, pFormatInfo->tiling, pPropertyCount, nullptr);
        return;
    }
    uint32_t count = *pPropertyCount;
    std::vector<VkSparseImageFormatProperties> nativeProps(count);
    real_sfp(physicalDevice, pFormatInfo->format, pFormatInfo->type, pFormatInfo->samples, pFormatInfo->usage, pFormatInfo->tiling, &count, nativeProps.data());
    for (uint32_t i = 0; i < count; ++i) {
        pProperties[i].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2;
        pProperties[i].pNext = nullptr;
        pProperties[i].properties = nativeProps[i];
    }
    *pPropertyCount = count;
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
        (PFN_vkGetPhysicalDeviceExternalBufferProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceExternalBufferProperties");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceExternalBufferProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
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
        (PFN_vkGetPhysicalDeviceExternalFenceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceExternalFenceProperties");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceExternalFenceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceExternalFencePropertiesKHR");
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
        (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if (!real_fn) {
        real_fn = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
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

} // extern "C"

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
FORWARD_INST(void, vkGetPhysicalDeviceProperties, (VkPhysicalDevice physDev, VkPhysicalDeviceProperties* pProps), (physDev, pProps))
FORWARD_INST(void, vkGetPhysicalDeviceFormatProperties, (VkPhysicalDevice physDev, VkFormat format, VkFormatProperties* pProps), (physDev, format, pProps))
FORWARD_INST(VkResult, vkGetPhysicalDeviceImageFormatProperties, (VkPhysicalDevice physDev, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* pProps), (physDev, format, type, tiling, usage, flags, pProps))
FORWARD_INST(void, vkGetPhysicalDeviceQueueFamilyProperties, (VkPhysicalDevice physDev, uint32_t* pCount, VkQueueFamilyProperties* pProps), (physDev, pCount, pProps))
FORWARD_INST(void, vkGetPhysicalDeviceMemoryProperties, (VkPhysicalDevice physDev, VkPhysicalDeviceMemoryProperties* pProps), (physDev, pProps))
FORWARD_INST(VkResult, vkEnumerateDeviceLayerProperties, (VkPhysicalDevice physDev, uint32_t* pCount, VkLayerProperties* pProps), (physDev, pCount, pProps))

FORWARD_DEV_VOID(vkGetDeviceQueue, device, (VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue), (device, queueFamilyIndex, queueIndex, pQueue))
FORWARD_QUEUE(VkResult, vkQueueSubmit, (VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence), (queue, submitCount, pSubmits, fence))
FORWARD_QUEUE(VkResult, vkQueueWaitIdle, (VkQueue queue), (queue))
FORWARD_DEV(VkResult, vkDeviceWaitIdle, device, (VkDevice device), (device))
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
FORWARD_DEV_VOID(vkDestroyPipeline, device, (VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator), (device, pipeline, pAllocator))

} // extern "C"

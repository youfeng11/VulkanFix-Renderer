// Negative controls for POST readbacks. Test-only; never package this library.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#include "../core/driver_trust_shaders.h"

namespace {
PFN_vkGetInstanceProcAddr backend() {
    static auto gipa = [] {
        const char *path = std::getenv("VKFIX_POST_TEST_DRIVER");
        void *library = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : nullptr;
        return library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr")) : nullptr;
    }();
    return gipa;
}
bool drop(const char *mode) {
    const char *value = std::getenv("VKFIX_POST_TEST_DROP");
    return value && std::strcmp(value, mode) == 0;
}
VKAPI_ATTR void VKAPI_CALL copy_buffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy *) {}
VKAPI_ATTR void VKAPI_CALL copy_image(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy *) {}
VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL indirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL fill(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL reset_query(VkDevice, VkQueryPool, uint32_t, uint32_t) {}
PFN_vkCreateGraphicsPipelines create_graphics = nullptr;
PFN_vkCreateShaderModule create_shader = nullptr;
PFN_vkGetPhysicalDeviceProperties physical_properties = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL version10(uint32_t *version) { *version=VK_API_VERSION_1_0; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL properties10(VkPhysicalDevice physical,VkPhysicalDeviceProperties *properties) {
    physical_properties(physical,properties); properties->apiVersion=VK_API_VERSION_1_0;
}
VKAPI_ATTR void VKAPI_CALL properties_other(VkPhysicalDevice physical,VkPhysicalDeviceProperties *properties) {
    physical_properties(physical,properties);
    properties->vendorID=0x1234; properties->deviceID=0x5678; properties->driverVersion=99;
    properties->pipelineCacheUUID[0]^=0x5a;
}
VKAPI_ATTR VkResult VKAPI_CALL healthy_shader(VkDevice device,const VkShaderModuleCreateInfo *info,const VkAllocationCallbacks *allocator,VkShaderModule *out) {
    auto modified=*info;
    if (info->codeSize==sizeof vkfix::nativeprobe::sampler_subject &&
        !std::memcmp(info->pCode,vkfix::nativeprobe::sampler_subject,info->codeSize)) {
        modified.pCode=vkfix::nativeprobe::sampler_control; modified.codeSize=sizeof vkfix::nativeprobe::sampler_control;
    }
    if (info->codeSize==sizeof vkfix::nativeprobe::uniform_subject &&
        !std::memcmp(info->pCode,vkfix::nativeprobe::uniform_subject,info->codeSize)) {
        modified.pCode=vkfix::nativeprobe::uniform_control; modified.codeSize=sizeof vkfix::nativeprobe::uniform_control;
    }
    return create_shader(device,&modified,allocator,out);
}
VKAPI_ATTR VkResult VKAPI_CALL no_divisor(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    std::vector<VkGraphicsPipelineCreateInfo> modified(infos,infos+count);
    std::vector<VkPipelineVertexInputStateCreateInfo> inputs(count);
    for (uint32_t i=0; i<count; ++i) if (infos[i].pVertexInputState) {
        inputs[i]=*infos[i].pVertexInputState;
        auto head=static_cast<const VkBaseInStructure *>(inputs[i].pNext);
        if (head && head->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT)
            inputs[i].pNext=head->pNext;
        modified[i].pVertexInputState=&inputs[i];
    }
    return create_graphics(device,cache,count,modified.data(),allocator,pipelines);
}
PFN_vkGetDeviceProcAddr device_proc = nullptr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device(VkDevice device, const char *name) {
    if (!name || !device_proc) return nullptr;
    if (drop("trust-healthy") && !std::strcmp(name,"vkCreateShaderModule")) {
        create_shader=reinterpret_cast<PFN_vkCreateShaderModule>(device_proc(device,name));
        return reinterpret_cast<PFN_vkVoidFunction>(healthy_shader);
    }
    if (drop("fill") && std::strcmp(name,"vkCmdFillBuffer")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(fill);
    if (drop("reset") && (std::strcmp(name,"vkResetQueryPool")==0 || std::strcmp(name,"vkResetQueryPoolEXT")==0))
        return reinterpret_cast<PFN_vkVoidFunction>(reset_query);
    if (drop("copy") && std::strcmp(name, "vkCmdCopyBuffer") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(copy_buffer);
    if (drop("render") && std::strcmp(name, "vkCmdCopyImageToBuffer") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(copy_image);
    if ((drop("push") || drop("trust-control")) && std::strcmp(name, "vkCmdDispatch") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(dispatch);
    if (drop("indirect") && std::strcmp(name, "vkCmdDrawIndirect") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(indirect);
    if (drop("draw") && std::strcmp(name, "vkCmdDraw") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(draw);
    if (drop("divisor") && std::strcmp(name, "vkCreateGraphicsPipelines") == 0) {
        create_graphics=reinterpret_cast<PFN_vkCreateGraphicsPipelines>(device_proc(device,name));
        return reinterpret_cast<PFN_vkVoidFunction>(no_divisor);
    }
    return device_proc(device, name);
}
}
extern "C" __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    auto gipa = backend();
    if (!gipa || !name) return nullptr;
    if (drop("api10") && std::strcmp(name,"vkEnumerateInstanceVersion")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(version10);
    if (drop("api10") && std::strcmp(name,"vkGetPhysicalDeviceProperties")==0) {
        physical_properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance,name));
        return reinterpret_cast<PFN_vkVoidFunction>(properties10);
    }
    if (drop("trust-other-identity") && std::strcmp(name,"vkGetPhysicalDeviceProperties")==0) {
        physical_properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance,name));
        return reinterpret_cast<PFN_vkVoidFunction>(properties_other);
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, name));
        return device_proc ? reinterpret_cast<PFN_vkVoidFunction>(get_device) : nullptr;
    }
    return gipa(instance, name);
}

// Test-only strict API1.0 boundary, forwarding valid work to the real GPU.
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#define EXPORT extern "C" __attribute__((visibility("default")))
namespace {
PFN_vkGetInstanceProcAddr gipa{}; PFN_vkGetDeviceProcAddr gdpa{};
VkInstance instance{}; std::once_flag once;
std::atomic<unsigned> rejected{0}, seen{0};
std::atomic<unsigned> bind2_calls{0};
std::atomic<unsigned> group_enumerations{0}, group_creations{0};
bool legacy_bindings() { const auto* mode = std::getenv("VULKANFIX_TEST_BIND1_ONLY"); return mode && !strcmp(mode,"1"); }
void init() {
    std::call_once(once,[] {
        auto library = dlopen("libvulkan.so",RTLD_NOW | RTLD_LOCAL);
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(library,"vkGetInstanceProcAddr");
        gdpa = (PFN_vkGetDeviceProcAddr)dlsym(library,"vkGetDeviceProcAddr");
    });
}
bool has(const void* chain,VkStructureType type) {
    for (auto p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext) if (p->sType == type) return true;
    return false;
}
void VKAPI_CALL properties(VkPhysicalDevice physical,VkPhysicalDeviceProperties* out) {
    ((PFN_vkGetPhysicalDeviceProperties)gipa(instance,"vkGetPhysicalDeviceProperties"))(physical,out); out->apiVersion = VK_API_VERSION_1_0;
}
VkResult VKAPI_CALL extensions(VkPhysicalDevice physical,const char* layer,uint32_t* count,VkExtensionProperties* out) {
    auto fn = (PFN_vkEnumerateDeviceExtensionProperties)gipa(instance,"vkEnumerateDeviceExtensionProperties");
    uint32_t size = 0; auto status = fn(physical,layer,&size,nullptr); if (status != VK_SUCCESS) return status;
    std::vector<VkExtensionProperties> list(size); status = fn(physical,layer,&size,list.data()); if (status != VK_SUCCESS) return status;
    uint32_t available = 0, written = 0;
    for (uint32_t i = 0; i < size; ++i) {
        if (!strcmp(list[i].extensionName,VK_KHR_DEVICE_GROUP_EXTENSION_NAME)) continue;
        if (legacy_bindings() && !strcmp(list[i].extensionName,VK_KHR_BIND_MEMORY_2_EXTENSION_NAME)) continue;
        if (out && written < *count) out[written++] = list[i]; ++available;
    }
    if (!out) { *count = available; return VK_SUCCESS; }
    *count = written; return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}
VkResult VKAPI_CALL memory(VkDevice device,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* allocator,VkDeviceMemory* out) {
    if (has(info->pNext,VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO)) { ++rejected; return VK_ERROR_FEATURE_NOT_PRESENT; }
    seen.fetch_or(1); return ((PFN_vkAllocateMemory)gdpa(device,"vkAllocateMemory"))(device,info,allocator,out);
}
VkResult VKAPI_CALL begin(VkCommandBuffer cmd,const VkCommandBufferBeginInfo* info) {
    if (has(info->pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_COMMAND_BUFFER_BEGIN_INFO)) { ++rejected; return VK_ERROR_FEATURE_NOT_PRESENT; }
    seen.fetch_or(2); return ((PFN_vkBeginCommandBuffer)gipa(instance,"vkBeginCommandBuffer"))(cmd,info);
}
VkResult VKAPI_CALL pass(VkDevice device,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks* allocator,VkRenderPass* out) {
    for (uint32_t i = 0; i < info->dependencyCount; ++i)
        if (info->pDependencies[i].dependencyFlags & VK_DEPENDENCY_DEVICE_GROUP_BIT) { ++rejected; return VK_ERROR_FEATURE_NOT_PRESENT; }
    seen.fetch_or(4); return ((PFN_vkCreateRenderPass)gdpa(device,"vkCreateRenderPass"))(device,info,allocator,out);
}
void VKAPI_CALL pass_begin(VkCommandBuffer cmd,const VkRenderPassBeginInfo* info,VkSubpassContents contents) {
    if (has(info->pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO)) { ++rejected; return; }
    seen.fetch_or(8); ((PFN_vkCmdBeginRenderPass)gipa(instance,"vkCmdBeginRenderPass"))(cmd,info,contents);
}
void VKAPI_CALL barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    uint32_t mc,const VkMemoryBarrier* m,uint32_t bc,const VkBufferMemoryBarrier* b,uint32_t ic,const VkImageMemoryBarrier* i) {
    if (flags & VK_DEPENDENCY_DEVICE_GROUP_BIT) { ++rejected; return; }
    seen.fetch_or(16); ((PFN_vkCmdPipelineBarrier)gipa(instance,"vkCmdPipelineBarrier"))(cmd,src,dst,flags,mc,m,bc,b,ic,i);
}
VkResult VKAPI_CALL submit(VkQueue queue,uint32_t count,const VkSubmitInfo* infos,VkFence fence) {
    for (uint32_t i = 0; i < count; ++i)
        if (has(infos[i].pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO)) { ++rejected; return VK_ERROR_FEATURE_NOT_PRESENT; }
    seen.fetch_or(32); return ((PFN_vkQueueSubmit)gipa(instance,"vkQueueSubmit"))(queue,count,infos,fence);
}
void VKAPI_CALL mask(VkCommandBuffer,uint32_t) { ++rejected; }
VkResult VKAPI_CALL bind_buffer(VkDevice device,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize offset) {
    seen.fetch_or(64); return ((PFN_vkBindBufferMemory)gdpa(device,"vkBindBufferMemory"))(device,buffer,memory,offset);
}
VkResult VKAPI_CALL bind_image(VkDevice device,VkImage image,VkDeviceMemory memory,VkDeviceSize offset) {
    seen.fetch_or(128); return ((PFN_vkBindImageMemory)gdpa(device,"vkBindImageMemory"))(device,image,memory,offset);
}
VkResult VKAPI_CALL bind_buffer2(VkDevice device,uint32_t count,const VkBindBufferMemoryInfo* infos) {
    for (uint32_t i = 0; i < count; ++i)
        if (has(infos[i].pNext,VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO)) { ++rejected; return VK_ERROR_FEATURE_NOT_PRESENT; }
    ++bind2_calls; seen.fetch_or(64);
    return ((PFN_vkBindBufferMemory2KHR)gdpa(device,"vkBindBufferMemory2KHR"))(device,count,infos);
}
VkResult VKAPI_CALL bind_image2(VkDevice device,uint32_t count,const VkBindImageMemoryInfo* infos) {
    for (uint32_t i = 0; i < count; ++i)
        if (has(infos[i].pNext,VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO)) { ++rejected; return VK_ERROR_FEATURE_NOT_PRESENT; }
    ++bind2_calls; seen.fetch_or(128);
    return ((PFN_vkBindImageMemory2KHR)gdpa(device,"vkBindImageMemory2KHR"))(device,count,infos);
}
VkResult VKAPI_CALL image(VkDevice device,const VkImageCreateInfo* info,const VkAllocationCallbacks* allocator,VkImage* out) {
    if (info->flags & VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) { ++rejected; return VK_ERROR_FEATURE_NOT_PRESENT; }
    seen.fetch_or(256); return ((PFN_vkCreateImage)gdpa(device,"vkCreateImage"))(device,info,allocator,out);
}
VkResult VKAPI_CALL image_format(VkPhysicalDevice physical,VkFormat format,VkImageType type,VkImageTiling tiling,
    VkImageUsageFlags usage,VkImageCreateFlags flags,VkImageFormatProperties* out) {
    if (flags & VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) { ++rejected; return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    seen.fetch_or(512); return ((PFN_vkGetPhysicalDeviceImageFormatProperties)gipa(instance,"vkGetPhysicalDeviceImageFormatProperties"))(physical,format,type,tiling,usage,flags,out);
}
VkResult VKAPI_CALL image_format2(VkPhysicalDevice physical,const VkPhysicalDeviceImageFormatInfo2* info,VkImageFormatProperties2* out) {
    if (info->flags & VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) { ++rejected; return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    seen.fetch_or(512); return ((PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(instance,"vkGetPhysicalDeviceImageFormatProperties2"))(physical,info,out);
}
VkResult VKAPI_CALL enumerate_groups(VkInstance current,uint32_t* count,VkPhysicalDeviceGroupProperties* output) {
    ++group_enumerations;
    return ((PFN_vkEnumeratePhysicalDeviceGroups)gipa(current,"vkEnumeratePhysicalDeviceGroups"))(current,count,output);
}
VkResult VKAPI_CALL unenabled_groups(VkInstance,uint32_t*,VkPhysicalDeviceGroupProperties*) { ++rejected; return VK_ERROR_EXTENSION_NOT_PRESENT; }
VkResult VKAPI_CALL create_device(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* allocator,VkDevice* out) {
    for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO) {
            const auto* group = reinterpret_cast<const VkDeviceGroupDeviceCreateInfo*>(node);
            if (group->physicalDeviceCount != 1 || group->pPhysicalDevices[0] != physical) { ++rejected; return VK_ERROR_FEATURE_NOT_PRESENT; }
            ++group_creations;
        }
    return ((PFN_vkCreateDevice)gipa(instance,"vkCreateDevice"))(physical,info,allocator,out);
}
bool blocked_binding(const char* name) {
    return !strcmp(name,"vkBindBufferMemory2") || !strcmp(name,"vkBindImageMemory2") ||
        (legacy_bindings() && (!strcmp(name,"vkBindBufferMemory2KHR") || !strcmp(name,"vkBindImageMemory2KHR")));
}
PFN_vkVoidFunction lookup(const char* name) {
#define PROC(n,f) if (!strcmp(name,n)) return (PFN_vkVoidFunction)f
    PROC("vkAllocateMemory",memory); PROC("vkBeginCommandBuffer",begin); PROC("vkCreateRenderPass",pass);
    PROC("vkCmdBeginRenderPass",pass_begin); PROC("vkCmdPipelineBarrier",barrier); PROC("vkQueueSubmit",submit);
    PROC("vkCmdSetDeviceMask",mask); PROC("vkCmdSetDeviceMaskKHR",mask);
    PROC("vkBindBufferMemory",bind_buffer); PROC("vkBindImageMemory",bind_image);
    PROC("vkBindBufferMemory2KHR",bind_buffer2); PROC("vkBindImageMemory2KHR",bind_image2);
    PROC("vkCreateImage",image); PROC("vkGetPhysicalDeviceImageFormatProperties",image_format);
    PROC("vkGetPhysicalDeviceImageFormatProperties2",image_format2); PROC("vkGetPhysicalDeviceImageFormatProperties2KHR",image_format2);
    PROC("vkEnumeratePhysicalDeviceGroups",enumerate_groups); PROC("vkEnumeratePhysicalDeviceGroupsKHR",unenabled_groups);
    PROC("vkCreateDevice",create_device);
#undef PROC
    return nullptr;
}
}
EXPORT unsigned singleton_group_test_rejected() { return rejected.load(); }
EXPORT unsigned singleton_group_test_seen() { return seen.load(); }
EXPORT unsigned singleton_group_test_bind2_calls() { return bind2_calls.load(); }
EXPORT unsigned singleton_group_test_group_enumerations() { return group_enumerations.load(); }
EXPORT unsigned singleton_group_test_group_creations() { return group_creations.load(); }
EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device,const char* name) {
    init(); if (!strcmp(name,"vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (blocked_binding(name)) return nullptr;
    if (auto fn = lookup(name)) return fn; return gdpa(device,name);
}
EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance current,const char* name) {
    init(); if (current) instance = current;
    if (!strcmp(name,"vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name,"vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name,"vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)properties;
    if (!strcmp(name,"vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)extensions;
    if (blocked_binding(name)) return nullptr;
    if (auto fn = lookup(name)) return fn; return gipa(current,name);
}

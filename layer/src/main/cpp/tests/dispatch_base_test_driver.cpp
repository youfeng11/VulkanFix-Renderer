// Forward real GPU work while removing native dispatch-base entry points.
// Only an explicit test target builds this provider; it is never packaged.
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <dlfcn.h>
#include <mutex>
#include <vulkan/vulkan.h>
#define EXPORT extern "C" __attribute__((visibility("default")))
namespace {
PFN_vkGetInstanceProcAddr native_gipa;
PFN_vkGetDeviceProcAddr native_gdpa;
std::once_flag initialized;
std::atomic<unsigned> kernel_pipelines{0};
std::atomic<unsigned> bad_commands{0}, native_dispatches{0}, native_masks{0};
std::atomic<unsigned> queue2_probes{0};
std::atomic<unsigned> resource_bad{0}, requirements_calls[2]{}, template_create_calls[2]{}, template_update_calls[2]{}, template_destroy_calls[2]{};
std::atomic<unsigned> legacy_updates{0};
VkInstance native_instance{};
bool khr_mode() {
    const auto* value = std::getenv("VULKANFIX_TEST_NATIVE_GROUP_KHR");
    return value && !strcmp(value,"1");
}
bool khr_resources() { auto value = getenv("VULKANFIX_TEST_KHR_RESOURCES"); return value && !strcmp(value, "1"); }
bool resource_allowed(unsigned route) { return !khr_mode() || (route == 1 && khr_resources()); }
void report_template_emulation();
void init() {
    std::call_once(initialized, [] {
        std::atexit(report_template_emulation);
        auto library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (library) {
            native_gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
            native_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(library, "vkGetDeviceProcAddr");
        }
    });
}
bool hidden(const char* name) {
    // Exercise the complete emulated template lifecycle with unmodified CTS
    // workloads on the real GPU, independently of its physical API version.
    const auto* templates = std::getenv("VULKANFIX_TEST_HIDE_TEMPLATES");
    if (templates && !strcmp(templates, "1") &&
        (!strcmp(name, "vkCreateDescriptorUpdateTemplate") || !strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") ||
         !strcmp(name, "vkUpdateDescriptorSetWithTemplate") || !strcmp(name, "vkUpdateDescriptorSetWithTemplateKHR") ||
         !strcmp(name, "vkDestroyDescriptorUpdateTemplate") || !strcmp(name, "vkDestroyDescriptorUpdateTemplateKHR"))) return true;
    return !khr_mode() && (!strcmp(name, "vkCmdDispatchBase") || !strcmp(name, "vkCmdDispatchBaseKHR"));
}
void VKAPI_CALL update_descriptors(VkDevice device, uint32_t count, const VkWriteDescriptorSet* writes,
    uint32_t copyCount, const VkCopyDescriptorSet* copies) {
    ++legacy_updates;
    ((PFN_vkUpdateDescriptorSets)native_gdpa(device, "vkUpdateDescriptorSets"))(device, count, writes, copyCount, copies);
}
void report_template_emulation() {
    const auto* templates = std::getenv("VULKANFIX_TEST_HIDE_TEMPLATES");
    if (templates && !strcmp(templates, "1"))
        std::fprintf(stderr, "Template-emulation provider: native C/U/D=%u/%u/%u; legacy updates=%u; bad resource calls=%u\n",
            template_create_calls[0].load() + template_create_calls[1].load(),
            template_update_calls[0].load() + template_update_calls[1].load(),
            template_destroy_calls[0].load() + template_destroy_calls[1].load(),
            legacy_updates.load(), resource_bad.load());
}
void VKAPI_CALL queue2_stub(VkDevice,const VkDeviceQueueInfo2*,VkQueue* out) {
    ++queue2_probes; *out = VK_NULL_HANDLE;
}
void VKAPI_CALL properties(VkPhysicalDevice physical,VkPhysicalDeviceProperties* out) {
    ((PFN_vkGetPhysicalDeviceProperties)native_gipa(native_instance,"vkGetPhysicalDeviceProperties"))(physical,out);
    out->apiVersion = VK_API_VERSION_1_0;
}
VkResult VKAPI_CALL extensions(VkPhysicalDevice,const char*,uint32_t* count,VkExtensionProperties* out) {
    const char* names[] = {VK_KHR_DEVICE_GROUP_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME};
    const uint32_t size = khr_resources() ? 3 : 1;
    if (!out) { *count = size; return VK_SUCCESS; }
    auto written = std::min(*count, size);
    for (uint32_t i = 0; i < written; ++i) { out[i] = {}; strcpy(out[i].extensionName, names[i]); out[i].specVersion = 1; }
    *count = written; return written < size ? VK_INCOMPLETE : VK_SUCCESS;
}
template<unsigned Route> VKAPI_ATTR void VKAPI_CALL requirements(VkDevice d, const VkBufferMemoryRequirementsInfo2* info, VkMemoryRequirements2* out) {
    if (!resource_allowed(Route)) { ++resource_bad; out->memoryRequirements = {}; return; }
    ++requirements_calls[Route];
    ((PFN_vkGetBufferMemoryRequirements2)native_gdpa(d, "vkGetBufferMemoryRequirements2"))(d, info, out);
}
template<unsigned Route> VKAPI_ATTR VkResult VKAPI_CALL template_create(VkDevice d, const VkDescriptorUpdateTemplateCreateInfo* info, const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* out) {
    if (!resource_allowed(Route)) { ++resource_bad; *out = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED; }
    ++template_create_calls[Route];
    return ((PFN_vkCreateDescriptorUpdateTemplate)native_gdpa(d, "vkCreateDescriptorUpdateTemplate"))(d, info, alloc, out);
}
template<unsigned Route> VKAPI_ATTR void VKAPI_CALL template_update(VkDevice d, VkDescriptorSet set, VkDescriptorUpdateTemplate handle, const void* data) {
    if (!resource_allowed(Route)) { ++resource_bad; return; }
    ++template_update_calls[Route];
    ((PFN_vkUpdateDescriptorSetWithTemplate)native_gdpa(d, "vkUpdateDescriptorSetWithTemplate"))(d, set, handle, data);
}
template<unsigned Route> VKAPI_ATTR void VKAPI_CALL template_destroy(VkDevice d, VkDescriptorUpdateTemplate handle, const VkAllocationCallbacks* alloc) {
    if (!resource_allowed(Route)) { ++resource_bad; return; }
    ++template_destroy_calls[Route];
    ((PFN_vkDestroyDescriptorUpdateTemplate)native_gdpa(d, "vkDestroyDescriptorUpdateTemplate"))(d, handle, alloc);
}
PFN_vkVoidFunction resource_proc(const char* name) {
#define RESOURCE(command, fn) if (!strcmp(name, #command)) return (PFN_vkVoidFunction)fn<0>; if (!strcmp(name, #command "KHR")) return (PFN_vkVoidFunction)fn<1>
    RESOURCE(vkGetBufferMemoryRequirements2, requirements);
    RESOURCE(vkCreateDescriptorUpdateTemplate, template_create);
    RESOURCE(vkUpdateDescriptorSetWithTemplate, template_update);
    RESOURCE(vkDestroyDescriptorUpdateTemplate, template_destroy);
#undef RESOURCE
    return nullptr;
}
VkResult VKAPI_CALL create_device(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,
                                  const VkAllocationCallbacks* allocator,VkDevice* out) {
    // The provider exposes a KHR route backed by the real GPU's Core1.1 route.
    // Its sole advertised extension is consumed here; native commands below
    // execute through the real API1.1 instance, without changing their work.
    auto native = *info; native.enabledExtensionCount = 0; native.ppEnabledExtensionNames = nullptr;
    return ((PFN_vkCreateDevice)native_gipa(native_instance,"vkCreateDevice"))(physical,&native,allocator,out);
}
void VKAPI_CALL bad_mask(VkCommandBuffer,uint32_t) { ++bad_commands; }
void VKAPI_CALL bad_dispatch(VkCommandBuffer,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) { ++bad_commands; }
void VKAPI_CALL bad_peer(VkDevice,uint32_t,uint32_t,uint32_t,VkPeerMemoryFeatureFlags* out) { ++bad_commands; *out = 0; }
void VKAPI_CALL mask_khr(VkCommandBuffer cmd,uint32_t mask) {
    ++native_masks;
    ((PFN_vkCmdSetDeviceMask)native_gipa(native_instance,"vkCmdSetDeviceMask"))(cmd,mask);
}
void VKAPI_CALL dispatch_khr(VkCommandBuffer cmd,uint32_t x,uint32_t y,uint32_t z,uint32_t nx,uint32_t ny,uint32_t nz) {
    ++native_dispatches;
    ((PFN_vkCmdDispatchBase)native_gipa(native_instance,"vkCmdDispatchBase"))(cmd,x,y,z,nx,ny,nz);
}
void VKAPI_CALL peer_khr(VkDevice device,uint32_t heap,uint32_t local,uint32_t remote,VkPeerMemoryFeatureFlags* out) {
    ((PFN_vkGetDeviceGroupPeerMemoryFeatures)native_gdpa(device,"vkGetDeviceGroupPeerMemoryFeatures"))(device,heap,local,remote,out);
}
PFN_vkVoidFunction group_proc(const char* name) {
    if (!khr_mode()) return nullptr;
    if (!strcmp(name,"vkCmdSetDeviceMask")) return (PFN_vkVoidFunction)bad_mask;
    if (!strcmp(name,"vkCmdDispatchBase")) return (PFN_vkVoidFunction)bad_dispatch;
    if (!strcmp(name,"vkGetDeviceGroupPeerMemoryFeatures")) return (PFN_vkVoidFunction)bad_peer;
    if (!strcmp(name,"vkCmdSetDeviceMaskKHR")) return (PFN_vkVoidFunction)mask_khr;
    if (!strcmp(name,"vkCmdDispatchBaseKHR")) return (PFN_vkVoidFunction)dispatch_khr;
    if (!strcmp(name,"vkGetDeviceGroupPeerMemoryFeaturesKHR")) return (PFN_vkVoidFunction)peer_khr;
    return nullptr;
}
VkResult VKAPI_CALL create_compute(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    for (uint32_t i = 0; i < count; ++i)
        if (infos[i].stage.pName && !strcmp(infos[i].stage.pName, "kernel")) ++kernel_pipelines;
    auto native = (PFN_vkCreateComputePipelines)native_gdpa(device, "vkCreateComputePipelines");
    return native ? native(device, cache, count, infos, allocator, pipelines) : VK_ERROR_INITIALIZATION_FAILED;
}
} // namespace
EXPORT unsigned dispatch_base_test_kernel_pipelines() { return kernel_pipelines.load(); }
EXPORT unsigned dispatch_base_test_bad_commands() { return bad_commands.load(); }
EXPORT unsigned dispatch_base_test_native_dispatches() { return native_dispatches.load(); }
EXPORT unsigned dispatch_base_test_native_masks() { return native_masks.load(); }
EXPORT unsigned dispatch_base_test_queue2_probes() { return queue2_probes.load(); }
EXPORT unsigned dispatch_base_test_bad_resources() { return resource_bad.load(); }
EXPORT unsigned dispatch_base_test_requirements(unsigned route) { return requirements_calls[route].load(); }
EXPORT unsigned dispatch_base_test_template_creates(unsigned route) { return template_create_calls[route].load(); }
EXPORT unsigned dispatch_base_test_template_updates(unsigned route) { return template_update_calls[route].load(); }
EXPORT unsigned dispatch_base_test_template_destroys(unsigned route) { return template_destroy_calls[route].load(); }
EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char* name) {
    init();
    if (hidden(name)) return nullptr;
    if (!strcmp(name, "vkUpdateDescriptorSets")) return (PFN_vkVoidFunction)update_descriptors;
    if (!strcmp(name,"vkGetDeviceQueue2")) return (PFN_vkVoidFunction)queue2_stub;
    if (auto fn = group_proc(name)) return fn;
    if (auto fn = resource_proc(name)) return fn;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkCreateComputePipelines")) return (PFN_vkVoidFunction)create_compute;
    return native_gdpa ? native_gdpa(device, name) : nullptr;
}
EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    init();
    if (hidden(name)) return nullptr;
    if (!strcmp(name, "vkUpdateDescriptorSets")) return (PFN_vkVoidFunction)update_descriptors;
    if (!strcmp(name,"vkGetDeviceQueue2")) return (PFN_vkVoidFunction)queue2_stub;
    if (instance) native_instance = instance;
    if (auto fn = group_proc(name)) return fn;
    if (auto fn = resource_proc(name)) return fn;
    if (khr_mode()) {
        if (!strcmp(name,"vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)properties;
        if (!strcmp(name,"vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)extensions;
        if (!strcmp(name,"vkCreateDevice")) return (PFN_vkVoidFunction)create_device;
    }
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkCreateComputePipelines")) return (PFN_vkVoidFunction)create_compute;
    return native_gipa ? native_gipa(instance, name) : nullptr;
}

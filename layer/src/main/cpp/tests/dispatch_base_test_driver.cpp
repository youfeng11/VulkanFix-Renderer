// Forward real GPU work while removing native dispatch-base entry points.
// Only an explicit test target builds this provider; it is never packaged.
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <vulkan/vulkan.h>
#define EXPORT extern "C" __attribute__((visibility("default")))
namespace {
PFN_vkGetInstanceProcAddr native_gipa;
PFN_vkGetDeviceProcAddr native_gdpa;
std::once_flag initialized;
std::atomic<unsigned> kernel_pipelines{0};
void init() {
    std::call_once(initialized, [] {
        auto library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (library) {
            native_gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
            native_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(library, "vkGetDeviceProcAddr");
        }
    });
}
bool hidden(const char* name) {
    return !strcmp(name, "vkCmdDispatchBase") || !strcmp(name, "vkCmdDispatchBaseKHR");
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
EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char* name) {
    init();
    if (hidden(name)) return nullptr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkCreateComputePipelines")) return (PFN_vkVoidFunction)create_compute;
    return native_gdpa ? native_gdpa(device, name) : nullptr;
}
EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    init();
    if (hidden(name)) return nullptr;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkCreateComputePipelines")) return (PFN_vkVoidFunction)create_compute;
    return native_gipa ? native_gipa(instance, name) : nullptr;
}

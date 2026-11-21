// Real GPU provider with a strict Vulkan 1.0 shader admission boundary.
// This test-only library is never packaged in the plugin.
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <dlfcn.h>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#define EXPORT extern "C" __attribute__((visibility("default")))
namespace {
PFN_vkGetInstanceProcAddr gipa;
PFN_vkGetDeviceProcAddr gdpa;
std::once_flag once;
std::atomic<unsigned> accepted{0}, rejected{0};
std::atomic<bool> reported_api{false};
void init() {
    std::call_once(once, [] {
        auto library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
        gdpa = (PFN_vkGetDeviceProcAddr)dlsym(library, "vkGetDeviceProcAddr");
    });
}
VkInstance current_instance{};
void VKAPI_CALL properties_for_instance(VkPhysicalDevice physical, VkPhysicalDeviceProperties* out) {
    auto fn = (PFN_vkGetPhysicalDeviceProperties)gipa(current_instance, "vkGetPhysicalDeviceProperties");
    fn(physical, out);
    const auto* keep_api = std::getenv("VULKANFIX_TEST_KEEP_PHYSICAL_API");
    if (!keep_api || strcmp(keep_api, "1")) out->apiVersion = VK_API_VERSION_1_0;
    else if (!reported_api.exchange(true))
        std::printf("Strict Vulkan1.0 shader boundary with physical API 0x%x\n", out->apiVersion);
}
VkResult VKAPI_CALL extensions(VkPhysicalDevice physical, const char* layer, uint32_t* count,
                              VkExtensionProperties* out) {
    auto fn = (PFN_vkEnumerateDeviceExtensionProperties)gipa(current_instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t size = 0;
    auto status = fn(physical, layer, &size, nullptr);
    if (status != VK_SUCCESS) return status;
    std::vector<VkExtensionProperties> list(size);
    status = fn(physical, layer, &size, list.data());
    if (status != VK_SUCCESS) return status;
    uint32_t available = 0, written = 0;
    for (uint32_t i = 0; i < size; ++i) {
        if (!strcmp(list[i].extensionName, "VK_KHR_storage_buffer_storage_class")) continue;
        if (out && written < *count) out[written++] = list[i];
        ++available;
    }
    if (!out) { *count = available; return VK_SUCCESS; }
    *count = written;
    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}
bool modern_storage(const uint32_t* words, size_t size) {
    for (size_t at = 5; at < size;) {
        const uint32_t wc = words[at] >> 16, op = words[at] & 0xffff;
        if (!wc || at + wc > size) return true;
        if (op == 32 && wc == 4 && words[at + 2] == 12) return true;
        if (op == 10 && wc > 1 && !strcmp(reinterpret_cast<const char*>(words + at + 1),
                                        "SPV_KHR_storage_buffer_storage_class")) return true;
        at += wc;
    }
    return false;
}
VkResult VKAPI_CALL shader(VkDevice device, const VkShaderModuleCreateInfo* ci,
    const VkAllocationCallbacks* allocator, VkShaderModule* out) {
    if (!ci || !ci->pCode || ci->codeSize < 20 || ci->pCode[1] > 0x10000 ||
        modern_storage(ci->pCode, ci->codeSize / 4)) {
        ++rejected; if (out) *out = {}; return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    ++accepted;
    return ((PFN_vkCreateShaderModule)gdpa(device, "vkCreateShaderModule"))(device, ci, allocator, out);
}
}
EXPORT unsigned storage_buffer_test_accepted() { return accepted.load(); }
EXPORT unsigned storage_buffer_test_rejected() { return rejected.load(); }
EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char* name) {
    init();
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)shader;
    return gdpa(device, name);
}
EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    init();
    if (instance) current_instance = instance;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)properties_for_instance;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)extensions;
    if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)shader;
    return gipa(instance, name);
}

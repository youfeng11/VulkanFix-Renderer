#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <vector>
#include <vulkan/vulkan.h>

namespace {
unsigned failures = 0;
void check(bool ok, const char* text) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("VULKAN_FIX_DRIVER_PATH", "libvulkan.so", 1);
    unsetenv("VULKAN_FIX_VALIDATION_DRIVER"); unsetenv("VULKAN_FIX_RENDERDOC_DRIVER");
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    setenv("VULKAN_FIX_LOG_PATH", "/dev/null", 1);
    auto lib = dlopen(argc > 1 ? argv[1] : "./libvulkanfix.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("dlopen: %s\n", dlerror()); return 1; }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) return 1;
    check(gipa({}, "vkGetInstanceProcAddr") && gipa({}, "vkEnumerateInstanceVersion") &&
          gipa({}, "vkEnumerateInstanceExtensionProperties") && gipa({}, "vkEnumerateInstanceLayerProperties"), "global entry points remain available");
    check(!gipa({}, "vkCreateDevice") && !gipa({}, "vkDestroyDevice") && !gipa({}, "vkGetPhysicalDeviceFeatures2"), "null-instance queries reject non-global entry points");
    auto create = (PFN_vkCreateInstance)gipa({}, "vkCreateInstance");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    const char* unsupported = "VK_UNSUPPORTED_EXTENSION";
    ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = &unsupported;
    VkInstance rejected = (VkInstance)(uintptr_t)0x1234;
    const auto rejected_result = create(&ici, nullptr, &rejected);
    check(rejected_result == VK_ERROR_EXTENSION_NOT_PRESENT && !rejected, "unsupported instance extension fails before publishing an instance");
    if (rejected_result == VK_SUCCESS)
        ((PFN_vkDestroyInstance)gipa(rejected, "vkDestroyInstance"))(rejected, nullptr);
    const char* debug = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = &debug;
    VkInstance a{}, b{};
    if (create(&ici, nullptr, &a) != VK_SUCCESS) return 1;
    ici.enabledExtensionCount = 0;
    if (create(&ici, nullptr, &b) != VK_SUCCESS) return 1;
    check(gipa(a, "vkCmdBeginDebugUtilsLabelEXT") && !gipa(b, "vkCmdBeginDebugUtilsLabelEXT"), "GIPA respects instance extension enablement across two instances");
    check(!gipa(b, "vkGetPhysicalDeviceFeatures2KHR") && gipa(b, "vkGetPhysicalDeviceFeatures2"), "native Core 1.1 query support does not enable public KHR aliases");
    auto enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(a, "vkEnumeratePhysicalDevices");
    uint32_t count = 0;
    if (enumerate(a, &count, nullptr) != VK_SUCCESS || !count) return 1;
    std::vector<VkPhysicalDevice> physical(count);
    if (enumerate(a, &count, physical.data()) != VK_SUCCESS) return 1;
    auto families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(a, "vkGetPhysicalDeviceQueueFamilyProperties");
    families(physical[0], &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count); families(physical[0], &count, queues.data());
    uint32_t family = 0; while (family < count && !queues[family].queueCount) ++family;
    if (family == count) return 1;
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    auto create_device = (PFN_vkCreateDevice)gipa(a, "vkCreateDevice");
    VkDevice da{}, db{};
    if (create_device(physical[0], &dci, nullptr, &da) != VK_SUCCESS) return 1;
    const char* extensions[] = {VK_KHR_DEVICE_GROUP_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME};
    dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = extensions;
    if (create_device(physical[0], &dci, nullptr, &db) != VK_SUCCESS) return 1;
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(a, "vkGetDeviceProcAddr");
    check(!gdpa(da, "vkGetPhysicalDeviceImageFormatProperties") && !gdpa(da, "vkEnumeratePhysicalDeviceGroups") &&
          !gdpa(da, "vkCreateDevice"), "device query rejects instance and physical entry points before native bypass");
    check(gdpa(da, "vkCmdDispatchBase") && !gdpa(da, "vkCmdDispatchBaseKHR") &&
          !gdpa(da, "vkBindBufferMemory2KHR"), "core functions remain visible while unenabled promoted aliases are null");
    check(gdpa(db, "vkCmdDispatchBaseKHR") && gdpa(db, "vkBindBufferMemory2KHR"), "explicitly enabled promoted aliases remain available");
    check(gipa(b, "vkCmdDispatchBaseKHR") != nullptr, "GIPA exposes available device extension commands without requiring a device");
    check(gdpa(da, "vkCmdBeginDebugUtilsLabelEXT") && gdpa(db, "vkCmdBeginDebugUtilsLabelEXT"), "GDPA inherits enabled instance extension commands");
    check(!gdpa(da, "vkQueuePresentKHR") && !gdpa(da, "vkCmdBeginRenderingKHR"), "unrequested native and emulated device extension commands are null");
    VkQueue queue{}; auto get_queue = (PFN_vkGetDeviceQueue)gdpa(db, "vkGetDeviceQueue"); get_queue(db, family, 0, &queue);
    check(queue != VK_NULL_HANDLE, "authorized native device command is callable");
    auto destroy_device = (PFN_vkDestroyDevice)gdpa(da, "vkDestroyDevice"); destroy_device(db, nullptr); destroy_device(da, nullptr);
    auto destroy_instance = (PFN_vkDestroyInstance)gipa(a, "vkDestroyInstance"); destroy_instance(b, nullptr); destroy_instance(a, nullptr);
    std::printf("Proc address integration failures: %u\n", failures);
    return failures ? 1 : 0;
}

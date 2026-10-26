// Opt-in in-process Vulkan layer chain for Android launchers. No system layer
// properties or application debuggable flags are changed.
#include <algorithm>
#include <android/log.h>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#include <vulkan/vk_layer.h>

namespace {
std::once_flag once;
std::mutex mutex;
PFN_vkGetInstanceProcAddr native_gipa{}, capture_gipa{};
PFN_vkGetDeviceProcAddr native_gdpa{}, capture_gdpa{};
int (*capture_api)(int, void **) = nullptr;
std::vector<VkExtensionProperties> native_extensions, capture_extensions, visible_extensions;
std::map<VkPhysicalDevice, VkInstance> parents;
bool ready = false;
constexpr const char *layer_name = "VK_LAYER_RENDERDOC_Capture";
bool has(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const auto &e) { return !strcmp(name, e.extensionName); });
}

// Android's loader may only implement debug_report. RenderDoc provides names
// and labels itself, but forwards messenger management to the next layer.
struct DebugCallback {
    VkInstance instance{};
    VkDebugUtilsMessengerCreateInfoEXT info{};
    VkDebugReportCallbackEXT report{};
};
std::map<VkDebugUtilsMessengerEXT, std::shared_ptr<DebugCallback>> debug_callbacks;
VKAPI_ATTR VkBool32 VKAPI_CALL report_message(VkDebugReportFlagsEXT flags,
                                              VkDebugReportObjectTypeEXT, uint64_t, size_t,
                                              int32_t code, const char *layer, const char *message,
                                              void *user) {
    const auto &info = static_cast<DebugCallback *>(user)->info;
    auto severity =
        flags & VK_DEBUG_REPORT_ERROR_BIT_EXT ? VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
        : flags & (VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT)
            ? VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        : flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT
            ? VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
            : VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
    VkDebugUtilsMessageTypeFlagsEXT types =
        flags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT
            ? VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT
            : VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    if (!(info.messageSeverity & severity) || !(info.messageType & types))
        return VK_FALSE;
    VkDebugUtilsMessengerCallbackDataEXT data{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    data.pMessageIdName = layer;
    data.messageIdNumber = code;
    data.pMessage = message;
    return info.pfnUserCallback(severity, types, &data, info.pUserData);
}
void destroy_report(const std::shared_ptr<DebugCallback> &callback) {
    if (callback->report) {
        auto destroy = (PFN_vkDestroyDebugReportCallbackEXT)native_gipa(
            callback->instance, "vkDestroyDebugReportCallbackEXT");
        if (destroy)
            destroy(callback->instance, callback->report, nullptr);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL
create_debug_messenger(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *info,
                       const VkAllocationCallbacks *, VkDebugUtilsMessengerEXT *out) {
    if (!instance || !info || !info->pfnUserCallback || !out)
        return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    std::shared_ptr<DebugCallback> callback;
    try {
        callback = std::make_shared<DebugCallback>();
        callback->instance = instance;
        callback->info = *info;
        callback->info.pNext = nullptr;
        auto create = (PFN_vkCreateDebugReportCallbackEXT)native_gipa(
            instance, "vkCreateDebugReportCallbackEXT");
        if (create) {
            VkDebugReportCallbackCreateInfoEXT report{
                VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};
            if (info->messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                report.flags |= VK_DEBUG_REPORT_ERROR_BIT_EXT;
            if (info->messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                report.flags |=
                    VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
            if (info->messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
                report.flags |= VK_DEBUG_REPORT_INFORMATION_BIT_EXT;
            if (info->messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
                report.flags |= VK_DEBUG_REPORT_DEBUG_BIT_EXT;
            report.pfnCallback = report_message;
            report.pUserData = callback.get();
            auto result = create(instance, &report, nullptr, &callback->report);
            if (result != VK_SUCCESS)
                return result;
        }
        auto handle = (VkDebugUtilsMessengerEXT)(uintptr_t)callback.get();
        std::lock_guard<std::mutex> lock(mutex);
        debug_callbacks.emplace(handle, callback);
        *out = handle;
        return VK_SUCCESS;
    } catch (const std::bad_alloc &) {
        if (callback)
            destroy_report(callback);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VKAPI_ATTR void VKAPI_CALL destroy_debug_messenger(VkInstance instance,
                                                   VkDebugUtilsMessengerEXT messenger,
                                                   const VkAllocationCallbacks *) {
    std::shared_ptr<DebugCallback> callback;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = debug_callbacks.find(messenger);
        if (it == debug_callbacks.end() || it->second->instance != instance)
            return;
        callback = it->second;
        debug_callbacks.erase(it);
    }
    destroy_report(callback);
}
VKAPI_ATTR void VKAPI_CALL submit_debug_message(VkInstance instance,
                                                VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT types,
                                                const VkDebugUtilsMessengerCallbackDataEXT *data) {
    std::vector<std::shared_ptr<DebugCallback>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto &entry : debug_callbacks)
            if (entry.second->instance == instance)
                callbacks.push_back(entry.second);
    }
    for (const auto &callback : callbacks)
        if ((callback->info.messageSeverity & severity) && (callback->info.messageType & types))
            callback->info.pfnUserCallback(severity, types, data, callback->info.pUserData);
}
VKAPI_ATTR void VKAPI_CALL bottom_destroy_instance(VkInstance instance,
                                                   const VkAllocationCallbacks *allocator) {
    for (;;) {
        VkDebugUtilsMessengerEXT orphan{};
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto &entry : debug_callbacks)
                if (entry.second->instance == instance) {
                    orphan = entry.first;
                    break;
                }
        }
        if (!orphan)
            break;
        destroy_debug_messenger(instance, orphan, nullptr);
    }
    ((PFN_vkDestroyInstance)native_gipa(instance, "vkDestroyInstance"))(instance, allocator);
}
template <class Query> bool enumerate(Query query, std::vector<VkExtensionProperties> &out) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint32_t count = 0;
        if (query(&count, nullptr) != VK_SUCCESS)
            return false;
        out.resize(count);
        auto result = count ? query(&count, out.data()) : VK_SUCCESS;
        if (result == VK_INCOMPLETE)
            continue;
        if (result != VK_SUCCESS)
            return false;
        out.resize(count);
        return true;
    }
    return false;
}
VKAPI_ATTR VkResult VKAPI_CALL bottom_instance(const VkInstanceCreateInfo *info,
                                               const VkAllocationCallbacks *allocator,
                                               VkInstance *out) {
    auto ci = *info;
    std::vector<const char *> extensions, layers;
    for (uint32_t i = 0; i < ci.enabledExtensionCount; ++i) {
        auto name = ci.ppEnabledExtensionNames[i];
        // RenderDoc can add software extensions to its filtered global list
        // (notably debug_utils) without reporting them in its layer-only list.
        if (has(native_extensions, name) ||
            (!has(capture_extensions, name) && !has(visible_extensions, name)))
            extensions.push_back(name);
        else
            __android_log_print(ANDROID_LOG_INFO, "VulkanFixRenderDoc",
                                "Keeping capture-only instance extension above native driver: %s",
                                name);
    }
    for (uint32_t i = 0; i < ci.enabledLayerCount; ++i)
        if (strcmp(ci.ppEnabledLayerNames[i], layer_name))
            layers.push_back(ci.ppEnabledLayerNames[i]);
    ci.enabledExtensionCount = uint32_t(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    auto result =
        ((PFN_vkCreateInstance)native_gipa(nullptr, "vkCreateInstance"))(&ci, allocator, out);
    if (result != VK_SUCCESS)
        __android_log_print(ANDROID_LOG_ERROR, "VulkanFixRenderDoc",
                            "Native instance creation failed: %d", result);
    return result;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL next_gipa(VkInstance instance, const char *name) {
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)bottom_instance;
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)bottom_destroy_instance;
    auto native = native_gipa(instance, name);
    if (native || has(native_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        return native;
    if (!strcmp(name, "vkCreateDebugUtilsMessengerEXT"))
        return (PFN_vkVoidFunction)create_debug_messenger;
    if (!strcmp(name, "vkDestroyDebugUtilsMessengerEXT"))
        return (PFN_vkVoidFunction)destroy_debug_messenger;
    if (!strcmp(name, "vkSubmitDebugUtilsMessageEXT"))
        return (PFN_vkVoidFunction)submit_debug_message;
    return nullptr;
}
VKAPI_ATTR VkResult VKAPI_CALL
native_extensions_link(const VkEnumerateInstanceExtensionPropertiesChain *, const char *layer,
                       uint32_t *count, VkExtensionProperties *out) {
    return ((PFN_vkEnumerateInstanceExtensionProperties)native_gipa(
        nullptr, "vkEnumerateInstanceExtensionProperties"))(layer, count, out);
}
void initialize() {
    std::call_once(once, [] {
        auto path = getenv("VULKAN_FIX_RENDERDOC_NATIVE_DRIVER_PATH");
        auto native = dlopen(path && *path ? path : "libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!native)
            return;
        native_gipa = (PFN_vkGetInstanceProcAddr)dlsym(native, "vkGetInstanceProcAddr");
        native_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(native, "vkGetDeviceProcAddr");
        if (!native_gipa || !native_gdpa)
            return;
        std::string library;
        path = getenv("VULKAN_FIX_RENDERDOC_LIB");
        if (path && *path)
            library = path;
        else {
            Dl_info own{};
            if (!dladdr((void *)initialize, &own) || !own.dli_fname)
                return;
            library = own.dli_fname;
            auto slash = library.find_last_of('/');
            library = slash == std::string::npos ? "./" : library.substr(0, slash + 1);
            library += "libVkLayer_GLES_RenderDoc.so";
        }
        auto layer = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!layer) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanFixRenderDoc",
                                "Cannot load capture layer: %s", dlerror());
            return;
        }
        auto negotiate = (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(
            layer, "VK_LAYER_RENDERDOC_CaptureNegotiateLoaderLayerInterfaceVersion");
        VkNegotiateLayerInterface interface { LAYER_NEGOTIATE_INTERFACE_STRUCT, nullptr, 2 };
        if (!negotiate || negotiate(&interface) != VK_SUCCESS)
            return;
        capture_gipa = interface.pfnGetInstanceProcAddr;
        capture_gdpa = interface.pfnGetDeviceProcAddr;
        capture_api = (int (*)(int, void **))dlsym(layer, "RENDERDOC_GetAPI");
        auto native_enum = (PFN_vkEnumerateInstanceExtensionProperties)native_gipa(
            nullptr, "vkEnumerateInstanceExtensionProperties");
        // This loader-layer export has an extra chain parameter. It is not
        // ABI-compatible with the public three-parameter Vulkan command.
        using LayerEnumerate =
            VkResult(VKAPI_PTR *)(const VkEnumerateInstanceExtensionPropertiesChain *, const char *,
                                  uint32_t *, VkExtensionProperties *);
        auto capture_enum = (LayerEnumerate)dlsym(
            layer, "VK_LAYER_RENDERDOC_CaptureEnumerateInstanceExtensionProperties");
        if (!capture_gipa || !capture_gdpa || !capture_api || !native_enum || !capture_enum)
            return;
        VkEnumerateInstanceExtensionPropertiesChain extension_chain{
            {VK_CHAIN_TYPE_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES, 1,
             sizeof(VkEnumerateInstanceExtensionPropertiesChain)},
            native_extensions_link,
            nullptr};
        if (!enumerate(
                [&](uint32_t *n, VkExtensionProperties *p) { return native_enum(nullptr, n, p); },
                native_extensions) ||
            !enumerate(
                [&](uint32_t *n, VkExtensionProperties *p) {
                    return capture_enum(nullptr, layer_name, n, p);
                },
                capture_extensions) ||
            !enumerate(
                [&](uint32_t *n, VkExtensionProperties *p) {
                    return capture_enum(&extension_chain, nullptr, n, p);
                },
                visible_extensions))
            return;
        ready = true;
        __android_log_print(ANDROID_LOG_INFO, "VulkanFixRenderDoc",
                            "Explicit capture layer chain ready: %s", library.c_str());
    });
}
VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info,
                                               const VkAllocationCallbacks *allocator,
                                               VkInstance *out) {
    if (!info || !out)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto ci = *info;
    VkLayerInstanceLink link{};
    link.pfnNextGetInstanceProcAddr = next_gipa;
    VkLayerInstanceCreateInfo chain{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, ci.pNext,
                                    VK_LAYER_LINK_INFO};
    chain.u.pLayerInfo = &link;
    ci.pNext = &chain;
    return ((PFN_vkCreateInstance)capture_gipa(nullptr, "vkCreateInstance"))(&ci, allocator, out);
}
VKAPI_ATTR VkResult VKAPI_CALL physical_devices(VkInstance instance, uint32_t *count,
                                                VkPhysicalDevice *out) {
    auto result = ((PFN_vkEnumeratePhysicalDevices)capture_gipa(
        instance, "vkEnumeratePhysicalDevices"))(instance, count, out);
    if (out && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
        std::lock_guard<std::mutex> lock(mutex);
        for (uint32_t i = 0; i < *count; ++i)
            parents[out[i]] = instance;
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL groups(VkInstance instance, uint32_t *count,
                                      VkPhysicalDeviceGroupProperties *out) {
    auto fn = (PFN_vkEnumeratePhysicalDeviceGroups)capture_gipa(instance,
                                                                "vkEnumeratePhysicalDeviceGroups");
    if (!fn)
        fn = (PFN_vkEnumeratePhysicalDeviceGroups)capture_gipa(
            instance, "vkEnumeratePhysicalDeviceGroupsKHR");
    if (!fn)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    auto result = fn(instance, count, out);
    if (out && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
        std::lock_guard<std::mutex> lock(mutex);
        for (uint32_t i = 0; i < *count; ++i)
            for (uint32_t j = 0; j < out[i].physicalDeviceCount; ++j)
                parents[out[i].physicalDevices[j]] = instance;
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical,
                                             const VkDeviceCreateInfo *info,
                                             const VkAllocationCallbacks *allocator,
                                             VkDevice *out) {
    VkInstance instance{};
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = parents.find(physical);
        if (it != parents.end())
            instance = it->second;
    }
    if (!instance || !info || !out)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto ci = *info;
    VkLayerDeviceLink link{};
    link.pfnNextGetInstanceProcAddr = next_gipa;
    link.pfnNextGetDeviceProcAddr = native_gdpa;
    VkLayerDeviceCreateInfo chain{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, ci.pNext,
                                  VK_LAYER_LINK_INFO};
    chain.u.pLayerInfo = &link;
    ci.pNext = &chain;
    return ((PFN_vkCreateDevice)capture_gipa(instance, "vkCreateDevice"))(physical, &ci, allocator,
                                                                          out);
}
VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance instance,
                                            const VkAllocationCallbacks *allocator) {
    if (!instance)
        return;
    ((PFN_vkDestroyInstance)capture_gipa(instance, "vkDestroyInstance"))(instance, allocator);
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = parents.begin(); it != parents.end();)
        if (it->second == instance)
            it = parents.erase(it);
        else
            ++it;
}
VKAPI_ATTR VkResult VKAPI_CALL extensions(const char *layer, uint32_t *count,
                                          VkExtensionProperties *out) {
    if (!count)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (layer && strcmp(layer, layer_name))
        return ((PFN_vkEnumerateInstanceExtensionProperties)native_gipa(
            nullptr, "vkEnumerateInstanceExtensionProperties"))(layer, count, out);
    const auto &all = layer ? capture_extensions : visible_extensions;
    if (!out) {
        *count = uint32_t(all.size());
        return VK_SUCCESS;
    }
    auto copied = std::min(*count, uint32_t(all.size()));
    std::copy_n(all.data(), copied, out);
    *count = copied;
    return copied < all.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
} // namespace
extern "C" __attribute__((visibility("default"))) int RENDERDOC_GetAPI(int version, void **api) {
    initialize();
    return ready ? capture_api(version, api) : 0;
}
extern "C" __attribute__((visibility("default"))) PFN_vkVoidFunction
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    initialize();
    if (!ready || !name)
        return nullptr;
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)create_instance;
    if (!strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)create_device;
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)destroy_instance;
    if (!strcmp(name, "vkEnumeratePhysicalDevices"))
        return (PFN_vkVoidFunction)physical_devices;
    if (!strcmp(name, "vkEnumeratePhysicalDeviceGroups") ||
        !strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR"))
        return instance && capture_gipa(instance, name) ? (PFN_vkVoidFunction)groups : nullptr;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction)extensions;
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    return instance ? capture_gipa(instance, name) : native_gipa(instance, name);
}
extern "C" __attribute__((visibility("default"))) PFN_vkVoidFunction
vkGetDeviceProcAddr(VkDevice device, const char *name) {
    initialize();
    return ready && device && name ? capture_gdpa(device, name) : nullptr;
}

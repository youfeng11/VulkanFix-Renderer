#pragma once
#include "../core/driver_probe_process.h"
#include "../core/native_probe_resources.h"

namespace vkfix::nativeprobe {
// Owns the selected native device and its dispatch functions in one isolated
// process. Workloads choose a queue capability and build their own feature chain.
struct NativeContext {
    explicit NativeContext(PFN_vkGetInstanceProcAddr resolver) : gipa(resolver) {}
    NativeContext(const NativeContext &) = delete;
    NativeContext &operator=(const NativeContext &) = delete;
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkDevice device{};
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa{};
    uint32_t api = VK_API_VERSION_1_0;
    uint32_t family = 0;
    uint32_t timestamp_valid_bits = 0;
    bool pending = false, retain_device = false;

    template<class T> T get(const char *name) const {
        auto fn = gipa(instance, name);
        if (!fn) throw std::runtime_error(std::string("native probe missing ") + name);
        return reinterpret_cast<T>(fn);
    }
    template<class T> T dev(const char *name) const {
        auto fn = gdpa ? gdpa(device, name) : nullptr;
        if (!fn) throw std::runtime_error(std::string("native probe missing ") + name);
        return reinterpret_cast<T>(fn);
    }
    ~NativeContext() {
        // Leave the instance to process exit. A device with unresolved GPU
        // work must also stay alive until the worker exits.
        if (device && gdpa && !pending && !retain_device) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(device, "vkDestroyDevice"));
            if (destroy) destroy(device, nullptr);
        }
    }

    void select(const std::string &identity, uint64_t revision, VkQueueFlags queue_flags, uint32_t ceiling,
                bool require_timestamps = false) {
        auto version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion"));
        uint32_t loader_api = VK_API_VERSION_1_0;
        if (version && version(&loader_api) == VK_SUCCESS) api = std::min(loader_api, ceiling);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = api;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        check(get<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "worker instance");

        auto enumerate = get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        check(enumerate(instance, &count, nullptr), "worker devices");
        if (!count || count > 256) throw std::runtime_error("invalid worker physical-device count");
        std::vector<VkPhysicalDevice> devices(count);
        check(enumerate(instance, &count, devices.data()), "worker physicals");
        if (count > devices.size()) throw std::runtime_error("invalid worker physical-device list");
        auto describe = get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
        for (uint32_t index = 0; index < count; ++index) {
            describe(devices[index], &properties);
            if (driver_probe_identity(properties, revision) != identity) continue;
            physical = devices[index];
            break;
        }
        if (!physical) throw std::runtime_error("worker identity/revision mismatch");

        auto queues = get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        count = 0;
        queues(physical, &count, nullptr);
        if (!count || count > 65536) throw std::runtime_error("invalid worker queue-family count");
        std::vector<VkQueueFamilyProperties> families(count);
        queues(physical, &count, families.data());
        if (count > families.size()) throw std::runtime_error("invalid worker queue-family list");
        for (family = 0; family < count; ++family) {
            const auto &candidate = families[family];
            if (!candidate.queueCount || !(candidate.queueFlags & queue_flags) ||
                (require_timestamps && !candidate.timestampValidBits)) continue;
            timestamp_valid_bits = candidate.timestampValidBits;
            return;
        }
        if (require_timestamps) throw std::runtime_error("no graphics or compute queue advertises timestamp support");
        throw std::runtime_error("no queue family supports the probe workload");
    }

    bool has_extension(const char *name) const {
        const auto enumerate = get<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        uint32_t count = 0;
        if (enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS || !count || count > 65536) return false;
        std::vector<VkExtensionProperties> extensions(count);
        if (enumerate(physical, nullptr, &count, extensions.data()) != VK_SUCCESS || count > extensions.size()) return false;
        return std::any_of(extensions.begin(), extensions.begin() + count, [=](const auto &extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
    }

    void create_device(const VkDeviceCreateInfo &info) {
        gdpa = get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        check(get<PFN_vkCreateDevice>("vkCreateDevice")(physical, &info, nullptr, &device), "worker device");
    }
};
} // namespace vkfix::nativeprobe

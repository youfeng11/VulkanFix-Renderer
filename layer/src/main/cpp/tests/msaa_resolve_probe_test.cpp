#include "core/native_msaa_resolve_probe.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
namespace {
struct Context {
    VkInstance instance{}; VkDevice device{};
    PFN_vkGetInstanceProcAddr gipa{}; PFN_vkGetDeviceProcAddr gdpa{};
    bool pending = false, retain_device = false;
    template<class T> T get(const char* name) {
        auto fn = reinterpret_cast<T>(gipa(instance,name));
        if (!fn) throw std::runtime_error(name); return fn;
    }
    template<class T> T dev(const char* name) {
        auto fn = reinterpret_cast<T>(gdpa(device,name));
        if (!fn) throw std::runtime_error(name); return fn;
    }
    ~Context() {
        if (pending) return;
        if (device) dev<PFN_vkDestroyDevice>("vkDestroyDevice")(device,nullptr);
        if (instance) get<PFN_vkDestroyInstance>("vkDestroyInstance")(instance,nullptr);
    }
};
}
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0); Context p;
    try {
        auto library = dlopen("libvulkan.so",RTLD_NOW | RTLD_LOCAL);
        if (!library) throw std::runtime_error(dlerror());
        p.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr"));
        if (!p.gipa) throw std::runtime_error("native Vulkan resolver unavailable");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
        vkfix::nativeprobe::check(p.get<PFN_vkCreateInstance>("vkCreateInstance")(&ci,nullptr,&p.instance),"instance");
        auto enumerate = p.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        uint32_t count = 0; vkfix::nativeprobe::check(enumerate(p.instance,&count,nullptr),"devices");
        std::vector<VkPhysicalDevice> devices(count);
        vkfix::nativeprobe::check(enumerate(p.instance,&count,devices.data()),"devices");
        if (!count) throw std::runtime_error("no native device");
        auto physical = devices[0];
        auto queues = p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        queues(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count); queues(physical,&count,families.data());
        uint32_t family = 0;
        while (family < count && (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))) ++family;
        if (family == count) throw std::runtime_error("no graphics queue");
        float priority = 1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qi;
        vkfix::nativeprobe::check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dc,nullptr,&p.device),"device");
        p.gdpa = p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        const auto result = vkfix::nativeprobe::probe_msaa_array_resolve(p,physical,family);
        const auto verdict = result.verdict == vkfix::DriverProbeVerdict::Healthy ? "healthy" :
            result.verdict == vkfix::DriverProbeVerdict::RepairConfirmed ? "repair-confirmed" : "inconclusive";
        std::printf("Native MSAA array resolve: %s (%s)\n",verdict,result.detail.c_str());
        if (result.verdict == vkfix::DriverProbeVerdict::Inconclusive) return 77;
        if (argc > 1 && !std::strcmp(argv[1],"--expect-repair") && result.verdict != vkfix::DriverProbeVerdict::RepairConfirmed) return 1;
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"MSAA resolve probe: %s\n",error.what()); return 2; }
}

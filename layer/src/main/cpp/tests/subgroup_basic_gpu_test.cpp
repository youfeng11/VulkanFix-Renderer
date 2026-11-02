#include "core/subgroup_basic_probe.h"
#include "subgroup_basic_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <spirv-tools/libspirv.hpp>
using vkfix::nativeprobe::check;
struct Context {
    PFN_vkGetInstanceProcAddr gipa{}; PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{}; VkDevice device{};
    bool pending=false,retain_device=false;
    template<class T> T get(const char* name) {
        auto fn=reinterpret_cast<T>(gipa(instance,name));
        if (!fn) throw std::runtime_error(std::string("missing ")+name); return fn;
    }
    template<class T> T dev(const char* name) {
        auto fn=reinterpret_cast<T>(gdpa(device,name));
        if (!fn) throw std::runtime_error(std::string("missing ")+name); return fn;
    }
};
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    bool native=false,base=false;
    for (int i=2;i<argc;++i) { native|=!strcmp(argv[i],"--native-driver"); base|=!strcmp(argv[i],"--dispatch-base"); }
    setenv("VULKAN_FIX_DRIVER_PATH",native?"libvulkan.so":"./libstorage_buffer_test_driver.so",1);
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE","1",1); setenv("VULKAN_FIX_SHADER_CACHE_PATH","/dev/null",1);
    setenv("VULKAN_FIX_LOG_PATH","./subgroup-basic-gpu-layer.log",1);
    if (base) setenv("VULKAN_FIX_EMULATE_DISPATCH_BASE","1",1);
    Context p;
    try {
        auto library=dlopen(argc>1?argv[1]:"./libvulkanfix.so",RTLD_NOW|RTLD_LOCAL);
        if (!library) throw std::runtime_error(dlerror());
        p.gipa=(PFN_vkGetInstanceProcAddr)dlsym(library,"vkGetInstanceProcAddr");
        if (!p.gipa) throw std::runtime_error("no instance resolver");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo=&app;
        check(p.get<PFN_vkCreateInstance>("vkCreateInstance")(&ii,nullptr,&p.instance),"instance");
        auto enumerate=p.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        uint32_t count=0; check(enumerate(p.instance,&count,nullptr),"physical count");
        std::vector<VkPhysicalDevice> physicals(count); check(enumerate(p.instance,&count,physicals.data()),"physicals");
        if (physicals.empty()) throw std::runtime_error("no physical device");
        const auto physical=physicals[0];
        VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&subgroup};
        p.get<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(physical,&properties);
        if (!(subgroup.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT) || !(subgroup.supportedOperations&VK_SUBGROUP_FEATURE_BASIC_BIT) ||
            !subgroup.subgroupSize || (!native && (subgroup.subgroupSize!=1 || subgroup.supportedOperations!=VK_SUBGROUP_FEATURE_BASIC_BIT)))
            throw std::runtime_error("BASIC compute properties do not match the implementation");
        if (properties.properties.apiVersion>=VK_API_VERSION_1_2) {
            VkPhysicalDeviceVulkan11Properties aggregate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
            VkPhysicalDeviceProperties2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&aggregate};
            p.get<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(physical,&root);
            if (aggregate.subgroupSize!=subgroup.subgroupSize || aggregate.subgroupSupportedStages!=subgroup.supportedStages ||
                aggregate.subgroupSupportedOperations!=subgroup.supportedOperations ||
                aggregate.subgroupQuadOperationsInAllStages!=subgroup.quadOperationsInAllStages)
                throw std::runtime_error("aggregate and individual subgroup properties disagree");
        }
        auto qfp=p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        qfp(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> queues(count); qfp(physical,&count,queues.data());
        uint32_t family=count;
        for (uint32_t i=0;i<count;++i) if (queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT) { family=i; break; }
        if (family==count) throw std::runtime_error("no compute queue");
        const float priority=1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queue.queueFamilyIndex=family;
        queue.queueCount=1; queue.pQueuePriorities=&priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&queue;
        check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical,&di,nullptr,&p.device),"device");
        p.gdpa=p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        for (bool grouped:{false,true}) for (bool existing:{false,true}) for (bool spec:{false,true})
            for (bool divergent:{false,true}) for (bool signed_types:{false,true}) {
            if (native && divergent) continue; // A workgroup-varying condition is subgroup-uniform only at size 1.
            std::vector<uint32_t> code;
            if (!tools.Assemble(subgroup_basic_fixture(grouped,existing,spec,divergent,signed_types),&code) || !tools.Validate(code))
                throw std::runtime_error("invalid subgroup source fixture");
            const auto error=vkfix::nativeprobe::subgroup_basic(p,physical,family,code.data(),code.size(),subgroup,spec,base,!native);
            if (!error.empty()) throw std::runtime_error(error);
            printf("PASS: grouped=%u existing-index=%u specialized-workgroup=%u divergent=%u signed=%u base=%u size=%u\n",
                grouped,existing,spec,divergent,signed_types,base,subgroup.subgroupSize);
        }
        for (bool signed_types:{false,true}) {
            std::vector<uint32_t> code;
            if (!tools.Assemble(subgroup_basic_multi_entry(signed_types),&code) || !tools.Validate(code))
                throw std::runtime_error("invalid multi-entry subgroup fixture");
            for (const char* entry:{"main","second"}) {
                const auto error=vkfix::nativeprobe::subgroup_basic(p,physical,family,code.data(),code.size(),subgroup,
                    false,base,!native,entry,!strcmp(entry,"main")?12u:3u);
                if (!error.empty()) throw std::runtime_error(std::string(entry)+": "+error);
                printf("PASS: shared-module entry=%s signed=%u base=%u size=%u\n",entry,signed_types,base,subgroup.subgroupSize);
            }
        }
        p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr); p.device={};
        p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr); p.instance={};
        puts("Basic subgroup GPU failures: 0"); return 0;
    } catch (const std::exception& failure) {
        printf("FAIL: %s\n",failure.what());
        if (p.device && p.gdpa && !p.retain_device && !p.pending)
            p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr);
        if (p.instance && p.gipa && !p.retain_device && !p.pending)
            p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr);
        return 1;
    }
}

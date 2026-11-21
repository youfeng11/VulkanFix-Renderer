#include "core/storage_buffer_array_probe.h"
#include "storage_buffer_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <spirv-tools/libspirv.hpp>
using vkfix::nativeprobe::check;
struct Context {
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{};
    VkDevice device{};
    bool pending = false, retain_device = false;
    template<class T> T get(const char* name) {
        auto fn = reinterpret_cast<T>(gipa(instance, name));
        if (!fn) throw std::runtime_error(std::string("missing ") + name);
        return fn;
    }
    template<class T> T dev(const char* name) {
        auto fn = reinterpret_cast<T>(gdpa(device, name));
        if (!fn) throw std::runtime_error(std::string("missing ") + name);
        return fn;
    }
};
void run(Context& p, VkPhysicalDevice physical, uint32_t family, bool grouped, bool extension, bool specialized,
         bool scalar_reference, bool memory_access, bool static_access, bool deferred, bool dispatch_base,bool graphics) {
    spvtools::SpirvTools tools(extension ? SPV_ENV_VULKAN_1_0 : SPV_ENV_VULKAN_1_1);
    std::vector<uint32_t> words;
    auto text=storage_buffer_fixture(grouped,extension,specialized,memory_access);
    if (deferred && specialized) {
        const std::string capability="OpCapability StorageBufferArrayDynamicIndexing\n";
        text.erase(text.find(capability),capability.size());
        const std::string runtime="%lastIndex = OpISub %uint %arrayLength %one\n";
        text.erase(text.find(runtime),runtime.size());
        text.insert(text.find("%Array = OpTypeArray"),
                    "%lastIndex = OpSpecConstantOp %uint ISub %arrayLength %one\n");
    } else if (static_access && specialized) {
        for (const auto line:{"OpCapability StorageBufferArrayDynamicIndexing\n","OpDecorate %arrayLength SpecId 7\n"})
            text.erase(text.find(line),std::string(line).size());
        const std::string length="%arrayLength = OpSpecConstant %uint 2";
        text.replace(text.find(length),length.size(),"%arrayLength = OpConstant %uint 3");
        const std::string last="%lastBlock = OpAccessChain %ptrSBlock %array %lastIndex";
        text.replace(text.find(last),last.size(),"%lastBlock = OpAccessChain %ptrSBlock %array %two");
    }
    if (dispatch_base) {
        const std::string entry="OpEntryPoint GLCompute %main \"main\"";
        text.insert(text.find(entry)+entry.size()," %groupId");
        text.insert(text.find("OpMemberDecorate %Block 0"),"OpDecorate %groupId BuiltIn WorkgroupId\n");
        text.insert(text.find("%Block = OpTypeStruct"),"%v3uint = OpTypeVector %uint 3\n%ptrInput = OpTypePointer Input %v3uint\n");
        text.insert(text.find("%main = OpFunction"),"%groupId = OpVariable %ptrInput Input\n");
        // Globals must precede every function, including the helper.
        const std::string variable="%groupId = OpVariable %ptrInput Input\n";
        text.erase(text.find(variable),variable.size()); text.insert(text.find("%helper = OpFunction"),variable);
        text.insert(text.find("OpStore %fieldB %double"),
                    "%group = OpLoad %v3uint %groupId\n%groupX = OpCompositeExtract %uint %group 0\n%offsetDouble = OpIAdd %uint %double %groupX\n");
        text.replace(text.find("OpStore %fieldB %double"),std::string("OpStore %fieldB %double").size(),"OpStore %fieldB %offsetDouble");
    }
    std::vector<uint32_t> vertex;
    if (graphics) {
        const std::string entry="OpEntryPoint GLCompute %main \"main\"";
        text.replace(text.find(entry),entry.size(),"OpEntryPoint Fragment %main \"main\"");
        const std::string mode="OpExecutionMode %main LocalSize 1 1 1";
        text.replace(text.find(mode),mode.size(),"OpExecutionMode %main OriginUpperLeft");
        spvtools::SpirvTools legacy(SPV_ENV_VULKAN_1_0);
        if (!legacy.Assemble(storage_probe_vertex(),&vertex) || !legacy.Validate(vertex))
            throw std::runtime_error("invalid storage probe vertex");
    }
    if (scalar_reference && specialized) {
        auto at=text.find("%whole = OpLoad %Array %array");
        text.erase(at,text.find('\n',at)-at+1);
        at=text.find("OpStore %lastField %zero");
        text.insert(at,"%savedLast = OpLoad %uint %lastField\n");
        at=text.find("OpStore %array %whole");
        text.replace(at,text.find('\n',at)-at,"OpStore %lastField %savedLast");
    }
    if (!tools.Assemble(text,&words) || !tools.Validate(words))
        throw std::runtime_error("invalid storage fixture");
    const auto result=vkfix::nativeprobe::storage_buffer_array(p,physical,family,words.data(),words.size(),specialized,dispatch_base,
        graphics?vertex.data():nullptr,vertex.size());
    if (!result.error.empty()) throw std::runtime_error(result.error);
    printf("PASS: grouped=%u extension-SPIR-V-1.0=%u specialized-array=%u memory-access=%u indexing-disabled=%u dispatch-base=%u graphics=%u UBO unchanged, SSBO copies/array/atomics matched\n",
        grouped,extension,specialized&&(!static_access||deferred),memory_access,static_access,dispatch_base,graphics);
}
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    const auto flag=[&](const char* name) {
        for (int i=2;i<argc;++i) if (!strcmp(argv[i],name)) return true;
        return false;
    };
    const bool deferred=flag("--specialized-no-dynamic-indexing");
    const bool native_driver=flag("--native-driver"), static_access=flag("--no-dynamic-indexing")||deferred;
    const bool dispatch_base=flag("--dispatch-base");
    const bool graphics=flag("--graphics");
    setenv("VULKAN_FIX_DRIVER_PATH",native_driver?"libvulkan.so":"./libstorage_buffer_test_driver.so",1);
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE","1",1);
    setenv("VULKAN_FIX_SHADER_CACHE_PATH","/dev/null",1);
    setenv("VULKAN_FIX_LOG_PATH","./storage-buffer-gpu-layer.log",1);
    if (dispatch_base) setenv("VULKAN_FIX_EMULATE_DISPATCH_BASE","1",1);
    Context p;
    try {
        if (dispatch_base && graphics) throw std::runtime_error("dispatch base requires compute");
        auto provider=dlopen("./libstorage_buffer_test_driver.so",RTLD_NOW|RTLD_LOCAL);
        if (!provider) throw std::runtime_error(dlerror());
        auto accepted=(unsigned(*)())dlsym(provider,"storage_buffer_test_accepted");
        auto library=dlopen(argc>1 ? argv[1] : "./libvulkanfix.so",RTLD_NOW|RTLD_LOCAL);
        if (!library || !accepted) throw std::runtime_error("missing provider or wrapper");
        p.gipa=(PFN_vkGetInstanceProcAddr)dlsym(library,"vkGetInstanceProcAddr");
        if (!p.gipa) throw std::runtime_error("missing instance resolver");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion=native_driver?VK_API_VERSION_1_1:VK_API_VERSION_1_0;
        VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance.pApplicationInfo=&app;
        check(p.get<PFN_vkCreateInstance>("vkCreateInstance")(&instance,nullptr,&p.instance),"instance");
        auto enumerate=p.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        uint32_t count=0; check(enumerate(p.instance,&count,nullptr),"physical count");
        std::vector<VkPhysicalDevice> devices(count); check(enumerate(p.instance,&count,devices.data()),"physical devices");
        if (devices.empty()) throw std::runtime_error("no physical device");
        const auto physical=devices.front();
        VkPhysicalDeviceFeatures available{}, enabled{};
        p.get<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical,&available);
        if (!static_access && !available.shaderStorageBufferArrayDynamicIndexing)
            throw std::runtime_error("specialized-array probe requires storage-buffer array dynamic indexing");
        enabled.shaderStorageBufferArrayDynamicIndexing=static_access?VK_FALSE:VK_TRUE;
        if (graphics && !available.fragmentStoresAndAtomics) throw std::runtime_error("fragment storage unavailable");
        enabled.fragmentStoresAndAtomics=graphics?VK_TRUE:VK_FALSE;
        auto qfp=p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        qfp(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> queues(count); qfp(physical,&count,queues.data());
        uint32_t family=count;
        for (uint32_t i=0; i<count; ++i) if (queues[i].queueFlags & (graphics?VK_QUEUE_GRAPHICS_BIT:VK_QUEUE_COMPUTE_BIT)) { family=i; break; }
        if (family==count) throw std::runtime_error("no compute queue");
        float priority=1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex=family; queue.queueCount=1; queue.pQueuePriorities=&priority;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device.queueCreateInfoCount=1; device.pQueueCreateInfos=&queue;
        device.pEnabledFeatures=&enabled;
        check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical,&device,nullptr,&p.device),"device");
        p.gdpa=p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        const auto before=accepted();
        for (unsigned mode : {0u,1u,2u}) for (bool grouped : {false,true}) for (bool extension : {false,true})
            run(p,physical,family,grouped,extension,mode>0,
                flag("--scalar-reference"),mode==2,static_access,deferred,dispatch_base,graphics);
        if (!native_driver && !(argc>1 && !strcmp(argv[1],"libvulkan.so")) && accepted()<before+12)
            throw std::runtime_error("missing native legacy-shader admissions");
        p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr); p.device={};
        p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr); p.instance={};
        puts(native_driver ? "Storage-buffer GPU failures: 0 (modern native driver path)" :
             argc>1 && !strcmp(argv[1],"libvulkan.so") ? "Storage-buffer GPU failures: 0 (native reference)" :
             "Storage-buffer GPU failures: 0 (native admission rejects modern headers/classes)");
        return 0;
    } catch (const std::exception& e) {
        printf("FAIL: %s\n",e.what());
        if (p.device && p.gdpa && !p.retain_device) {
            auto destroy=(PFN_vkDestroyDevice)p.gdpa(p.device,"vkDestroyDevice");
            if (destroy) destroy(p.device,nullptr);
        }
        if (p.instance && p.gipa && !p.retain_device) {
            auto destroy=(PFN_vkDestroyInstance)p.gipa(p.instance,"vkDestroyInstance");
            if (destroy) destroy(p.instance,nullptr);
        }
        return 1;
    }
}

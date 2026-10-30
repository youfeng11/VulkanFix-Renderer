// Standalone device POST inspired by MobileGL's separate capability/functional
// reports. This implementation uses no MobileGL code or renderer global state.
// Run in a disposable process: a broken ICD must not take down the game.
#define VK_NO_PROTOTYPES
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window_jni.h>
using PostWindow = ANativeWindow;
#else
struct PostWindow;
#endif
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <stdexcept>
#include <string>
#include <vector>
#include "../core/native_probe_resources.h"
#include "../core/driver_trust.h"
#include "../core/driver_trust_shaders.h"
#include "../core/driver_probe_process.h"

namespace {
thread_local std::string *report_sink = nullptr;
struct ProbeMismatch : std::runtime_error { using std::runtime_error::runtime_error; };
std::string quote(const std::string &value) {
    std::string out = "\"";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += char(ch); }
        else if (ch < 32) {
            char escaped[7]; std::snprintf(escaped, sizeof escaped, "\\u%04x", ch); out += escaped;
        } else out += char(ch);
    }
    return out + '"';
}
std::string version(uint32_t v) {
    return std::to_string(VK_VERSION_MAJOR(v)) + "." + std::to_string(VK_VERSION_MINOR(v)) +
           "." + std::to_string(VK_VERSION_PATCH(v));
}
void row(const std::string &name, const char *status, const std::string &detail,
         const char *evidence = "query") {
    const std::string line = "{\"name\":" + quote(name) + ",\"status\":" + quote(status) +
        ",\"evidence\":" + quote(evidence) + ",\"detail\":" + quote(detail) + "}\n";
    if (report_sink) *report_sink += line;
    else std::fputs(line.c_str(), stdout);
}
void require(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
template<class F> void run_case(const char *name, F &&execute) {
    try { execute(); }
    catch (const ProbeMismatch &error) { row(name, "FAIL", error.what(), "functional"); }
}
template<class T, class F> std::vector<T> enumerate(F fn) {
    for (unsigned attempt = 0; attempt != 8; ++attempt) {
        uint32_t count = 0;
        require(fn(&count, nullptr), "enumerate count");
        if (count > 65536) throw std::runtime_error("unreasonable enumeration count");
        std::vector<T> values(count);
        VkResult result = fn(&count, values.data());
        if (result == VK_INCOMPLETE) continue;
        require(result, "enumerate values");
        if (count > values.size()) throw std::runtime_error("enumeration overflow");
        values.resize(count);
        return values;
    }
    throw std::runtime_error("enumeration never stabilized");
}
enum Scope { Global, Instance, Device };
struct Command { uint32_t version; Scope scope; const char *name; };
const Command commands[]{
#define POST_COMMAND(v, scope, name) {v, scope, name},
#include "driver_post_commands.inc"
#undef POST_COMMAND
};

struct Probe {
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance{};
    VkDevice device{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkSurfaceKHR surface{};
    bool retain_device = false;
    bool live_present_resources = false;
    // Do not destroy objects after a fence timeout: the ICD may still use them.
    bool pending = false;
    template<class T> T get(const char *name) const {
        auto result = gipa(instance, name);
        if (!result) throw std::runtime_error(std::string("missing ") + name);
        return reinterpret_cast<T>(result);
    }
    template<class T> T dev(const char *name) const {
        auto result = gdpa(device, name);
        if (!result) throw std::runtime_error(std::string("missing ") + name);
        return reinterpret_cast<T>(result);
    }
    ~Probe() {
        if (pending || live_present_resources) return;
        if (device) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(device, "vkDestroyDevice"));
            if (destroy) destroy(device, nullptr);
        }
        if (instance) {
            if (surface) {
                auto destroy_surface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(gipa(instance,"vkDestroySurfaceKHR"));
                if (destroy_surface) destroy_surface(instance,surface,nullptr);
            }
            auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
            if (destroy) destroy(instance, nullptr);
        }
    }
};

void buffer_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    // All temporary resources belong to this throwaway device. On failure the
    // process exits; do not tear down potentially in-flight objects on timeout.
    struct Resources {
        Probe &p;
        VkBuffer buffer{};
        VkDeviceMemory allocation{};
        VkCommandPool pool{};
        VkFence fence{};
        bool mapped = false;
        ~Resources() {
            if (p.pending) return;
            if (mapped) p.dev<PFN_vkUnmapMemory>("vkUnmapMemory")(p.device, allocation);
            if (fence) p.dev<PFN_vkDestroyFence>("vkDestroyFence")(p.device, fence, nullptr);
            if (pool) p.dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(p.device, pool, nullptr);
            if (buffer) p.dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(p.device, buffer, nullptr);
            if (allocation) p.dev<PFN_vkFreeMemory>("vkFreeMemory")(p.device, allocation, nullptr);
        }
    } resources{p};
    // Resolve teardown before allocating anything; a malformed dispatch table
    // must not throw out of a destructor while handling another failure.
    for (const char *name : {"vkUnmapMemory", "vkDestroyFence", "vkDestroyCommandPool",
                              "vkDestroyBuffer", "vkFreeMemory"})
        p.dev<PFN_vkVoidFunction>(name);
    VkPhysicalDeviceMemoryProperties memory{};
    p.get<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 256; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto &buffer = resources.buffer;
    require(p.dev<PFN_vkCreateBuffer>("vkCreateBuffer")(p.device, &bi, nullptr, &buffer), "create buffer");
    VkMemoryRequirements mr{};
    p.dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(p.device, buffer, &mr);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = i; break; }
    if (type == UINT32_MAX) throw std::runtime_error("no host-visible buffer memory");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = type;
    auto &allocation = resources.allocation;
    require(p.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(p.device, &ai, nullptr, &allocation), "allocate memory");
    require(p.dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(p.device, buffer, allocation, 0), "bind memory");
    void *mapped = nullptr;
    require(p.dev<PFN_vkMapMemory>("vkMapMemory")(p.device, allocation, 0, VK_WHOLE_SIZE, 0, &mapped), "map memory");
    resources.mapped = true;
    std::memset(mapped, 0xa5, 256);
    VkMappedMemoryRange initial{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; initial.memory = allocation; initial.size = VK_WHOLE_SIZE;
    require(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device, 1, &initial), "initialize buffer sentinel");
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = family;
    auto &pool = resources.pool;
    require(p.dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(p.device, &pci, nullptr, &pool), "create command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb{};
    require(p.dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(p.device, &cai, &cb), "allocate commands");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    require(p.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cb, &begin), "begin commands");
    constexpr uint32_t expected = 0x56465850;
    p.dev<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(cb, buffer, 0, 256, expected);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    require(p.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cb), "end commands");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; auto &fence = resources.fence;
    require(p.dev<PFN_vkCreateFence>("vkCreateFence")(p.device, &fci, nullptr, &fence), "create fence");
    VkQueue queue{}; p.dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(p.device, family, 0, &queue);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
    require(p.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, fence), "queue submit");
    p.pending = true;
    require(p.dev<PFN_vkWaitForFences>("vkWaitForFences")(p.device, 1, &fence, VK_TRUE, 3000000000ull), "wait fence (3 seconds)");
    p.pending = false;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = allocation; range.size = VK_WHOLE_SIZE;
    require(p.dev<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(p.device, 1, &range), "invalidate memory");
    bool correct = true;
    for (unsigned i = 0; i < 64; ++i) correct &= static_cast<uint32_t *>(mapped)[i] == expected;
    if (!correct) throw ProbeMismatch("GPU fill readback mismatch");
    row("bufferTransfer", "PASS", "64 GPU-written words matched after fence and host visibility barrier", "functional");
}

void timeline_smoke(Probe &p, bool core) {
    auto destroy = p.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore");
    auto signal = p.dev<PFN_vkSignalSemaphore>(core ? "vkSignalSemaphore" : "vkSignalSemaphoreKHR");
    auto wait = p.dev<PFN_vkWaitSemaphores>(core ? "vkWaitSemaphores" : "vkWaitSemaphoresKHR");
    auto counter = p.dev<PFN_vkGetSemaphoreCounterValue>(core ? "vkGetSemaphoreCounterValue" : "vkGetSemaphoreCounterValueKHR");
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; type.initialValue = 3;
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; ci.pNext = &type;
    VkSemaphore semaphore{};
    require(p.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(p.device, &ci, nullptr, &semaphore), "create timeline semaphore");
    try {
        uint64_t value = 0;
        require(counter(p.device, semaphore, &value), "initial timeline counter");
        if (value != 3) throw ProbeMismatch("incorrect initial timeline counter");
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; si.semaphore = semaphore; si.value = 7;
        require(signal(p.device, &si), "signal timeline");
        uint64_t target = 7;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1; wi.pSemaphores = &semaphore; wi.pValues = &target;
        require(wait(p.device, &wi, 1000000000ull), "wait timeline (1 second)");
        require(counter(p.device, semaphore, &value), "signaled timeline counter");
        if (value != 7) throw ProbeMismatch("incorrect signaled timeline counter");
        target = 8;
        if (wait(p.device, &wi, 0) != VK_TIMEOUT) throw ProbeMismatch("unsignaled timeline wait did not time out");
    } catch (...) { destroy(p.device, semaphore, nullptr); throw; }
    destroy(p.device, semaphore, nullptr);
    row("timelineSemaphore.hostSignalWait", "PASS", "initial counter, host signal/wait, and unsignaled zero-timeout verified", "functional");
}

#include "driver_post_gpu.inc"
#include "driver_post_draw.inc"
#include "driver_post_swapchain.inc"

int run(const char *library, PostWindow *window = nullptr) {
    // Intentionally keep the loader resident until process exit: ICD threads
    // may retain code pointers even after all Vulkan objects are destroyed.
#ifdef _WIN32
    auto handle = LoadLibraryA(library);
    if (!handle) throw std::runtime_error("LoadLibrary failed: " + std::to_string(GetLastError()));
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(handle, "vkGetInstanceProcAddr"));
#else
    void *handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw std::runtime_error(dlerror());
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
#endif
    if (!gipa) throw std::runtime_error("loader lacks vkGetInstanceProcAddr");
#ifndef _WIN32
    // Probe the real device, including when this POST invocation targets the
    // wrapper. VulkanFix forwards instance/physical handles without wrapping.
    static void *native_library = dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    auto native_gipa = native_library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(native_library,"vkGetInstanceProcAddr")) : nullptr;
#else
    auto native_gipa = gipa;
#endif
    uint32_t loader_version = VK_API_VERSION_1_0;
    auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerate_version) require(enumerate_version(&loader_version), "instance version");
    uint32_t requested = std::min(loader_version, uint32_t(VK_API_VERSION_1_2));
    Probe probe{gipa};
    auto enum_instance = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr,"vkEnumerateInstanceExtensionProperties"));
    if (!enum_instance) throw std::runtime_error("missing vkEnumerateInstanceExtensionProperties");
    const auto instance_extensions=enumerate<VkExtensionProperties>([&](uint32_t *n,VkExtensionProperties *v){ return enum_instance(nullptr,n,v); });
    auto has_instance=[&](const char *name) { return std::any_of(instance_extensions.begin(),instance_extensions.end(),
        [&](const VkExtensionProperties &value){ return std::strcmp(value.extensionName,name)==0; }); };
    std::vector<const char *> requested_instance_extensions;
    const bool features2_query=requested>=VK_API_VERSION_1_1 || has_instance("VK_KHR_get_physical_device_properties2");
    if (requested<VK_API_VERSION_1_1 && features2_query) requested_instance_extensions.push_back("VK_KHR_get_physical_device_properties2");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VulkanFix Driver POST"; app.apiVersion = requested;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
#ifdef __ANDROID__
    if (window) {
        requested_instance_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        requested_instance_extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    }
#endif
    ici.enabledExtensionCount=uint32_t(requested_instance_extensions.size());
    ici.ppEnabledExtensionNames=requested_instance_extensions.data();
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) throw std::runtime_error("missing vkCreateInstance");
    require(create(&ici, nullptr, &probe.instance), "create instance");
#ifdef __ANDROID__
    if (window) {
        VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR}; ci.window=window;
        require(probe.get<PFN_vkCreateAndroidSurfaceKHR>("vkCreateAndroidSurfaceKHR")(probe.instance,&ci,nullptr,&probe.surface), "create Android surface");
    }
#endif
    auto epd = probe.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    auto physicals = enumerate<VkPhysicalDevice>([&](uint32_t *n, VkPhysicalDevice *v) { return epd(probe.instance, n, v); });
    if (physicals.empty()) throw std::runtime_error("no physical devices");
    row("loader", "INFO", std::string(library) + "; API " + version(loader_version), "identity");
    for (size_t index = 0; index < physicals.size(); ++index) {
        probe.retain_device = false;
        auto physical = physicals[index];
        VkPhysicalDeviceProperties properties{};
        probe.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical, &properties);
        const uint32_t api = std::min(requested, properties.apiVersion);
        row("device", "INFO", std::to_string(index) + ": " + properties.deviceName +
            "; API " + version(properties.apiVersion) + "; vendor " + std::to_string(properties.vendorID) +
            "; driverVersion " + std::to_string(properties.driverVersion), "identity");
        auto eep = probe.get<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        auto extensions = enumerate<VkExtensionProperties>([&](uint32_t *n, VkExtensionProperties *v) { return eep(physical, nullptr, n, v); });
        auto has = [&](const char *name) { return std::any_of(extensions.begin(), extensions.end(),
            [&](const VkExtensionProperties &e) { return std::strcmp(e.extensionName, name) == 0; }); };
        for (const char *name : {"VK_KHR_dynamic_rendering", "VK_KHR_push_descriptor", "VK_KHR_synchronization2",
                                 "VK_EXT_vertex_attribute_divisor", "VK_KHR_swapchain"})
            row(name, has(name) ? "PASS" : "FAIL", has(name) ? "advertised extension; see separate functional probes" : "extension not advertised");
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceHostQueryResetFeatures reset{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
        VkPhysicalDeviceSynchronization2Features sync{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        VkPhysicalDeviceDynamicRenderingFeatures dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisor{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
        auto append = [&](auto &value) { value.pNext = features.pNext; features.pNext = &value; };
        if (api >= VK_API_VERSION_1_1) append(draw);
        if (api >= VK_API_VERSION_1_2 || has("VK_KHR_timeline_semaphore")) append(timeline);
        if (api >= VK_API_VERSION_1_2 || has("VK_EXT_host_query_reset")) append(reset);
        if (has("VK_KHR_synchronization2")) append(sync);
        if (has("VK_KHR_dynamic_rendering")) append(dynamic);
        if (has("VK_EXT_vertex_attribute_divisor")) append(divisor);
        if (features2_query)
            probe.get<PFN_vkGetPhysicalDeviceFeatures2>(requested>=VK_API_VERSION_1_1 ? "vkGetPhysicalDeviceFeatures2" : "vkGetPhysicalDeviceFeatures2KHR")(physical, &features);
        else probe.get<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical, &features.features);
        // The original 1.0 extension had no opt-in feature bit. Its presence
        // enables draw parameters; the feature struct arrived with core 1.1.
        if (api<VK_API_VERSION_1_1 && has("VK_KHR_shader_draw_parameters")) draw.shaderDrawParameters=VK_TRUE;
        auto feature = [&](const char *name, VkBool32 value) {
            row(name, value ? "PASS" : "FAIL", value ? "feature query true; see separate functional probes" : "feature unavailable");
        };
        feature("multiDrawIndirect", features.features.multiDrawIndirect);
        feature("fillModeNonSolid", features.features.fillModeNonSolid);
        feature("drawIndirectFirstInstance", features.features.drawIndirectFirstInstance);
        feature("samplerAnisotropy", features.features.samplerAnisotropy);
        feature("vertexPipelineStoresAndAtomics", features.features.vertexPipelineStoresAndAtomics);
        feature("shaderDrawParameters", draw.shaderDrawParameters);
        feature("timelineSemaphore", timeline.timelineSemaphore);
        feature("hostQueryReset", reset.hostQueryReset);
        feature("synchronization2", sync.synchronization2);
        feature("dynamicRendering", dynamic.dynamicRendering);
        feature("vertexAttributeInstanceRateDivisor", divisor.vertexAttributeInstanceRateDivisor);
        const auto native_trust = vkfix::native_driver_trust(probe.instance,physical,native_gipa);
        const auto &aliases = native_trust.sampler_aliases;
        row("driverTrust.samplerAliases", aliases.verdict==vkfix::DriverProbeVerdict::Healthy ? "PASS" :
            aliases.verdict==vkfix::DriverProbeVerdict::RepairConfirmed ? "WARN" : "SKIP",
            std::string(vkfix::driver_probe_verdict_name(aliases.verdict))+": "+aliases.detail,
            "functional");
        const auto &uniform = native_trust.large_uniform;
        row("driverTrust.largeUniform", uniform.verdict==vkfix::DriverProbeVerdict::Healthy ? "PASS" :
            uniform.verdict==vkfix::DriverProbeVerdict::RepairConfirmed ? "WARN" : "SKIP",
            std::string(vkfix::driver_probe_verdict_name(uniform.verdict))+": "+uniform.detail,
            "functional");
        const auto &push_trust = native_trust.push_descriptor;
        row("driverTrust.pushDescriptor", push_trust.verdict==vkfix::DriverProbeVerdict::Healthy ? "PASS" :
            push_trust.verdict==vkfix::DriverProbeVerdict::RepairConfirmed ? "WARN" : "SKIP",
            std::string(vkfix::driver_probe_verdict_name(push_trust.verdict))+": "+push_trust.detail,
            "functional");
        const auto &tiler = native_trust.tiler_budget;
        row("driverTrust.tilerPressure", tiler.verdict==vkfix::DriverProbeVerdict::Healthy ? "PASS" :
            tiler.verdict==vkfix::DriverProbeVerdict::RepairConfirmed ? "WARN" : "SKIP",
            std::string(vkfix::driver_probe_verdict_name(tiler.verdict))+": "+tiler.detail,
            "functional");
        if (native_gipa) {
            auto raw_props=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(native_gipa(probe.instance,"vkGetPhysicalDeviceProperties"));
            if (raw_props) {
                VkPhysicalDeviceProperties raw{}; raw_props(physical,&raw);
                auto control=vkfix::run_driver_probe_process("push-control",vkfix::driver_probe_identity(raw,vkfix::nativeprobe::revision));
                using S=vkfix::ProbeProcessStatus;
                row("driverTrust.workerControl",control.status==S::Passed ? "PASS" :
                    control.status==S::Unavailable ? "SKIP" : "FAIL",control.detail,"functional");
            }
        }
        uint32_t count = 0;
        auto qfp = probe.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        qfp(physical, &count, nullptr);
        if (!count || count > 65536) throw std::runtime_error("invalid queue family count");
        std::vector<VkQueueFamilyProperties> families(count);
        qfp(physical, &count, families.data());
        uint32_t family = 0;
        // Prefer graphics so a leading compute-only family does not hide the
        // drawing probes on devices with multiple queues.
        while (family < count && (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))) ++family;
        if (family == count) {
            family=0;
            while (family < count && (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))) ++family;
        }
        if (family == count) throw std::runtime_error("no graphics/compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
        // Enable only features exercised below, not every advertised feature.
        // The inventory device still exposes all mandatory core entry points.
        std::vector<const char *> enabled_extensions;
        VkPhysicalDeviceTimelineSemaphoreFeatures enable_timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceHostQueryResetFeatures enable_reset{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
        VkPhysicalDeviceSynchronization2Features enable_sync{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        VkPhysicalDeviceDynamicRenderingFeatures enable_dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        VkPhysicalDeviceShaderDrawParametersFeatures enable_draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
        VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT enable_divisor{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
        VkPhysicalDeviceFeatures enabled_features{};
        enabled_features.multiDrawIndirect = features.features.multiDrawIndirect;
        enabled_features.drawIndirectFirstInstance = features.features.drawIndirectFirstInstance;
        enabled_features.fillModeNonSolid = features.features.fillModeNonSolid;
        enabled_features.vertexPipelineStoresAndAtomics = features.features.vertexPipelineStoresAndAtomics;
        enabled_features.samplerAnisotropy = features.features.samplerAnisotropy;
        dci.pEnabledFeatures = &enabled_features;
        if (timeline.timelineSemaphore) {
            enable_timeline.timelineSemaphore = VK_TRUE;
            enable_timeline.pNext = const_cast<void *>(dci.pNext); dci.pNext = &enable_timeline;
            if (api < VK_API_VERSION_1_2) enabled_extensions.push_back("VK_KHR_timeline_semaphore");
        }
        if (reset.hostQueryReset) {
            enable_reset.hostQueryReset = VK_TRUE;
            enable_reset.pNext = const_cast<void *>(dci.pNext); dci.pNext = &enable_reset;
            if (api < VK_API_VERSION_1_2) enabled_extensions.push_back("VK_EXT_host_query_reset");
        }
        if (sync.synchronization2) {
            enable_sync.synchronization2 = VK_TRUE;
            enable_sync.pNext = const_cast<void *>(dci.pNext); dci.pNext = &enable_sync;
            enabled_extensions.push_back("VK_KHR_synchronization2");
        }
        const bool dynamic_dependencies = api >= VK_API_VERSION_1_2 ||
            (api >= VK_API_VERSION_1_1 && has("VK_KHR_depth_stencil_resolve") && has("VK_KHR_create_renderpass2"));
        if (dynamic.dynamicRendering && dynamic_dependencies) {
            enable_dynamic.dynamicRendering = VK_TRUE;
            enable_dynamic.pNext = const_cast<void *>(dci.pNext); dci.pNext = &enable_dynamic;
            enabled_extensions.push_back("VK_KHR_dynamic_rendering");
            if (api < VK_API_VERSION_1_2) {
                enabled_extensions.push_back("VK_KHR_depth_stencil_resolve");
                enabled_extensions.push_back("VK_KHR_create_renderpass2");
            }
        }
        if (has("VK_KHR_push_descriptor")) enabled_extensions.push_back("VK_KHR_push_descriptor");
        if (window && has("VK_KHR_swapchain")) enabled_extensions.push_back("VK_KHR_swapchain");
        if (draw.shaderDrawParameters) {
            enable_draw.shaderDrawParameters = VK_TRUE;
            if (api>=VK_API_VERSION_1_1) {
                enable_draw.pNext = const_cast<void *>(dci.pNext); dci.pNext = &enable_draw;
            } else enabled_extensions.push_back("VK_KHR_shader_draw_parameters");
        }
        if (divisor.vertexAttributeInstanceRateDivisor) {
            enable_divisor.vertexAttributeInstanceRateDivisor = VK_TRUE;
            enable_divisor.pNext = const_cast<void *>(dci.pNext); dci.pNext = &enable_divisor;
            enabled_extensions.push_back("VK_EXT_vertex_attribute_divisor");
        }
        dci.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
        dci.ppEnabledExtensionNames = enabled_extensions.data();
        probe.gdpa = probe.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        require(probe.get<PFN_vkCreateDevice>("vkCreateDevice")(physical, &dci, nullptr, &probe.device), "create device");
        for (uint32_t core : {VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2}) {
            unsigned total = 0, missing = 0;
            if (api >= core) for (const auto &command : commands) if (command.version == core) {
                ++total;
                auto address = command.scope == Device ? probe.gdpa(probe.device, command.name) :
                    gipa(command.scope == Global ? VK_NULL_HANDLE : probe.instance, command.name);
                if (!address) { ++missing; row(command.name, "FAIL", "required core entry point is null"); }
            }
            row("Vulkan " + version(core) + " Core", api >= core && !missing ? "PASS" : "FAIL",
                api < core ? "API version below requirement" : std::to_string(total) +
                " entry points checked; " + std::to_string(missing) + " missing; not full conformance");
        }
        run_case("bufferTransfer", [&] { buffer_smoke(probe, physical, family); });
        if (timeline.timelineSemaphore) run_case("timelineSemaphore.hostSignalWait", [&] { timeline_smoke(probe, api >= VK_API_VERSION_1_2); });
        else row("timelineSemaphore.hostSignalWait", "SKIP", "feature unavailable", "functional");
        if (reset.hostQueryReset) run_case("hostQueryReset.reset", [&] { query_reset_smoke(probe, physical, family, api >= VK_API_VERSION_1_2, families[family].timestampValidBits != 0); });
        else row("hostQueryReset.reset", "SKIP", "feature unavailable", "functional");
        if (enable_sync.synchronization2) run_case("synchronization2.transfer", [&] { synchronization_smoke(probe, physical, family); });
        else row("synchronization2.transfer", "SKIP", "feature unavailable", "functional");
        if (enable_dynamic.dynamicRendering && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            run_case("dynamicRendering.clear", [&] { dynamic_rendering_smoke(probe, physical, family); });
        else row("dynamicRendering.clear", dynamic.dynamicRendering ? "FAIL" : "SKIP",
                 dynamic.dynamicRendering ? "graphics queue or extension dependencies unavailable" : "feature unavailable", "functional");
        if (has("VK_KHR_push_descriptor") && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
            run_case("pushDescriptor.compute", [&] { push_descriptor_smoke(probe, physical, family); });
        else row("pushDescriptor.compute", "SKIP", "push descriptor or compute queue unavailable", "functional");
        const bool graphics = (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool dynamic_draw = enable_dynamic.dynamicRendering != VK_FALSE;
        if (graphics) run_case("graphics.draw", [&] { draw_smoke(probe, physical, family, dynamic_draw, 0); });
        else row("graphics.draw", "SKIP", "no graphics queue", "functional");
        const char *draw_names[]{"", "multiDrawIndirect.draw", "drawIndirectFirstInstance.draw",
            "vertexAttributeInstanceRateDivisor.draw", "fillModeNonSolid.line", "shaderDrawParameters.draw"};
        const bool draw_available[]{false, enabled_features.multiDrawIndirect != VK_FALSE,
            enabled_features.drawIndirectFirstInstance != VK_FALSE,
            enable_divisor.vertexAttributeInstanceRateDivisor != VK_FALSE,
            enabled_features.fillModeNonSolid != VK_FALSE, enable_draw.shaderDrawParameters != VK_FALSE};
        for (unsigned mode = 1; mode <= 5; ++mode) {
            if (graphics && draw_available[mode])
                run_case(draw_names[mode], [&] { draw_smoke(probe, physical, family, dynamic_draw, mode,
                    enabled_features.multiDrawIndirect && enabled_features.drawIndirectFirstInstance); });
            else row(draw_names[mode], "SKIP", "feature or graphics queue unavailable", "functional");
        }
        if (graphics && enabled_features.vertexPipelineStoresAndAtomics)
            run_case("vertexPipelineStoresAndAtomics.draw", [&] { vertex_stores_smoke(probe, physical, family, dynamic_draw); });
        else row("vertexPipelineStoresAndAtomics.draw", "SKIP", "feature or graphics queue unavailable", "functional");
        if (graphics && enabled_features.samplerAnisotropy)
            run_case("samplerAnisotropy.sample", [&] { sampler_smoke(probe, physical, family, dynamic_draw, properties.limits.maxSamplerAnisotropy); });
        else row("samplerAnisotropy.sample", "SKIP", "feature or graphics queue unavailable", "functional");
#ifdef __ANDROID__
        if (window && has("VK_KHR_swapchain"))
            run_case("VK_KHR_swapchain.present", [&] { swapchain_smoke(probe,physical,family,window); });
        else
#endif
            row("VK_KHR_swapchain.present", "SKIP", "Android surface or swapchain extension unavailable; use the plugin UI for present testing", "functional");
        if (!probe.retain_device) probe.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(probe.device, nullptr);
        probe.device = VK_NULL_HANDLE;
    }
    row("complete", "INFO", "Inventory and available representative probes completed; not full Vulkan conformance", "completion");
    return 0;
}
} // namespace
#ifdef VKFIX_POST_JNI
#include <jni.h>
extern "C" JNIEXPORT jstring JNICALL
Java_com_launchers_1plugin_renderer_DriverPostNative_run(JNIEnv *env, jobject, jstring library, jobject surface) {
    if (!library) return nullptr;
    const char *path = env->GetStringUTFChars(library, nullptr);
    if (!path) return nullptr;
    std::string report;
    report_sink = &report;
    PostWindow *window = surface ? ANativeWindow_fromSurface(env,surface) : nullptr;
    try { run(path,window); }
    catch (const std::exception &error) { row("probe", "FAIL", error.what(), "error"); }
    report_sink = nullptr;
    if (window) ANativeWindow_release(window);
    env->ReleaseStringUTFChars(library, path);
    return env->NewStringUTF(report.c_str());
}
#else
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 2) { std::fprintf(stderr, "Usage: %s [loader-library]\n", argv[0]); return 2; }
#ifdef _WIN32
    const char *default_loader = "vulkan-1.dll";
#else
    const char *default_loader = "libvulkan.so";
#endif
    try { return run(argc == 2 ? argv[1] : default_loader); }
    catch (const std::exception &e) { row("probe", "FAIL", e.what(), "error"); return 1; }
}
#endif

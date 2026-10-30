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
#include "driver_post_shaders.h"

namespace {
thread_local std::string *report_sink = nullptr;
unsigned core11_passes = 0;
unsigned core11_skips = 0;
unsigned core11_failures = 0;
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
    if (name.rfind("core11.", 0) == 0 && name != "core11.summary") {
        if (!std::strcmp(status, "PASS")) ++core11_passes;
        else if (!std::strcmp(status, "SKIP")) ++core11_skips;
        else if (!std::strcmp(status, "FAIL")) ++core11_failures;
    }
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

void buffer_memory2_smoke(Probe &p, VkPhysicalDevice physical) {
    auto get2 = reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(
        p.gdpa(p.device, "vkGetBufferMemoryRequirements2"));
    if (!get2) get2 = reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(
        p.gdpa(p.device, "vkGetBufferMemoryRequirements2KHR"));
    auto bind2 = reinterpret_cast<PFN_vkBindBufferMemory2>(
        p.gdpa(p.device, "vkBindBufferMemory2"));
    if (!bind2) bind2 = reinterpret_cast<PFN_vkBindBufferMemory2>(
        p.gdpa(p.device, "vkBindBufferMemory2KHR"));
    if (!get2 || !bind2) throw ProbeMismatch("Vulkan 1.1 buffer memory entry points are unavailable");

    auto create = p.dev<PFN_vkCreateBuffer>("vkCreateBuffer");
    auto destroy = p.dev<PFN_vkDestroyBuffer>("vkDestroyBuffer");
    auto get1 = p.dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements");
    auto allocate = p.dev<PFN_vkAllocateMemory>("vkAllocateMemory");
    auto free_memory = p.dev<PFN_vkFreeMemory>("vkFreeMemory");
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = 256;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    try {
        require(create(p.device, &ci, nullptr, &buffer), "create memory2 buffer");
        VkMemoryRequirements legacy{};
        get1(p.device, buffer, &legacy);
        VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
        info.buffer = buffer;
        VkMemoryRequirements2 modern{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        get2(p.device, &info, &modern);
        if (modern.memoryRequirements.size != legacy.size ||
            modern.memoryRequirements.alignment != legacy.alignment ||
            modern.memoryRequirements.memoryTypeBits != legacy.memoryTypeBits) {
            char detail[192];
            std::snprintf(detail, sizeof detail,
                          "vkGetBufferMemoryRequirements2 differs from legacy "
                          "(size %llu/%llu, align %llu/%llu, bits 0x%x/0x%x)",
                          (unsigned long long)modern.memoryRequirements.size,
                          (unsigned long long)legacy.size,
                          (unsigned long long)modern.memoryRequirements.alignment,
                          (unsigned long long)legacy.alignment,
                          unsigned(modern.memoryRequirements.memoryTypeBits),
                          unsigned(legacy.memoryTypeBits));
            throw ProbeMismatch(detail);
        }
        VkPhysicalDeviceMemoryProperties properties{};
        p.get<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &properties);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((legacy.memoryTypeBits & (1u << i)) != 0) { type = i; break; }
        }
        if (type == UINT32_MAX) throw ProbeMismatch("no memory type for buffer memory2");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = legacy.size;
        allocation.memoryTypeIndex = type;
        require(allocate(p.device, &allocation, nullptr, &memory), "allocate memory2 backing");
        VkBindBufferMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
        bind.buffer = buffer;
        bind.memory = memory;
        require(bind2(p.device, 1, &bind), "vkBindBufferMemory2");
        row("core11.bufferMemory2", "PASS",
            "vkGetBufferMemoryRequirements2 matches legacy and vkBindBufferMemory2 succeeds",
            "functional");
    } catch (...) {
        if (buffer) destroy(p.device, buffer, nullptr);
        if (memory) free_memory(p.device, memory, nullptr);
        throw;
    }
    if (buffer) destroy(p.device, buffer, nullptr);
    if (memory) free_memory(p.device, memory, nullptr);
}

void image_memory2_smoke(Probe &p, VkPhysicalDevice physical) {
    auto get2 = reinterpret_cast<PFN_vkGetImageMemoryRequirements2>(
        p.gdpa(p.device, "vkGetImageMemoryRequirements2"));
    if (!get2) get2 = reinterpret_cast<PFN_vkGetImageMemoryRequirements2>(
        p.gdpa(p.device, "vkGetImageMemoryRequirements2KHR"));
    auto bind2 = reinterpret_cast<PFN_vkBindImageMemory2>(
        p.gdpa(p.device, "vkBindImageMemory2"));
    if (!bind2) bind2 = reinterpret_cast<PFN_vkBindImageMemory2>(
        p.gdpa(p.device, "vkBindImageMemory2KHR"));
    if (!get2 || !bind2) throw ProbeMismatch("Vulkan 1.1 image memory entry points are unavailable");

    auto create = p.dev<PFN_vkCreateImage>("vkCreateImage");
    auto destroy = p.dev<PFN_vkDestroyImage>("vkDestroyImage");
    auto get1 = p.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
    auto allocate = p.dev<PFN_vkAllocateMemory>("vkAllocateMemory");
    auto free_memory = p.dev<PFN_vkFreeMemory>("vkFreeMemory");
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {16, 16, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    try {
        require(create(p.device, &ci, nullptr, &image), "create memory2 image");
        VkMemoryRequirements legacy{};
        get1(p.device, image, &legacy);
        VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        info.image = image;
        VkMemoryRequirements2 modern{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        get2(p.device, &info, &modern);
        if (modern.memoryRequirements.size != legacy.size ||
            modern.memoryRequirements.alignment != legacy.alignment ||
            modern.memoryRequirements.memoryTypeBits != legacy.memoryTypeBits) {
            char detail[192];
            std::snprintf(detail, sizeof detail,
                          "vkGetImageMemoryRequirements2 differs from legacy "
                          "(size %llu/%llu, align %llu/%llu, bits 0x%x/0x%x)",
                          (unsigned long long)modern.memoryRequirements.size,
                          (unsigned long long)legacy.size,
                          (unsigned long long)modern.memoryRequirements.alignment,
                          (unsigned long long)legacy.alignment,
                          unsigned(modern.memoryRequirements.memoryTypeBits),
                          unsigned(legacy.memoryTypeBits));
            throw ProbeMismatch(detail);
        }
        VkPhysicalDeviceMemoryProperties properties{};
        p.get<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &properties);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((legacy.memoryTypeBits & (1u << i)) != 0) { type = i; break; }
        }
        if (type == UINT32_MAX) throw ProbeMismatch("no memory type for image memory2");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = legacy.size;
        allocation.memoryTypeIndex = type;
        require(allocate(p.device, &allocation, nullptr, &memory), "allocate memory2 image backing");
        VkBindImageMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
        bind.image = image;
        bind.memory = memory;
        require(bind2(p.device, 1, &bind), "vkBindImageMemory2");
        row("core11.imageMemory2", "PASS",
            "vkGetImageMemoryRequirements2 matches legacy and vkBindImageMemory2 succeeds",
            "functional");
    } catch (...) {
        if (image) destroy(p.device, image, nullptr);
        if (memory) free_memory(p.device, memory, nullptr);
        throw;
    }
    if (image) destroy(p.device, image, nullptr);
    if (memory) free_memory(p.device, memory, nullptr);
}

void core11_query_chain(Probe &p, VkPhysicalDevice physical) {
    auto family2 = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceQueueFamilyProperties2"));
    if (!family2) family2 = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceQueueFamilyProperties2KHR"));
    auto family1 = p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!family2) throw ProbeMismatch("vkGetPhysicalDeviceQueueFamilyProperties2 is unavailable");
    uint32_t family_count = 0;
    family1(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> legacy_families(family_count);
    if (family_count) family1(physical, &family_count, legacy_families.data());
    uint32_t modern_family_count = 0;
    family2(physical, &modern_family_count, nullptr);
    std::vector<VkQueueFamilyProperties2> modern_families(modern_family_count);
    for (auto &family : modern_families)
        family.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    if (modern_family_count) family2(physical, &modern_family_count, modern_families.data());
    if (family_count != modern_family_count)
        throw ProbeMismatch("queue-family count differs between legacy and Vulkan 1.1 queries");
    for (uint32_t i = 0; i < family_count; ++i) {
        const auto &a = legacy_families[i];
        const auto &b = modern_families[i].queueFamilyProperties;
        if (a.queueFlags != b.queueFlags || a.queueCount != b.queueCount ||
            a.timestampValidBits != b.timestampValidBits ||
            a.minImageTransferGranularity.width != b.minImageTransferGranularity.width ||
            a.minImageTransferGranularity.height != b.minImageTransferGranularity.height ||
            a.minImageTransferGranularity.depth != b.minImageTransferGranularity.depth)
            throw ProbeMismatch("queue-family properties differ between legacy and Vulkan 1.1 queries");
    }

    auto memory2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceMemoryProperties2"));
    if (!memory2) memory2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceMemoryProperties2KHR"));
    if (!memory2) throw ProbeMismatch("vkGetPhysicalDeviceMemoryProperties2 is unavailable");
    VkPhysicalDeviceMemoryProperties legacy_memory{};
    p.get<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
        physical, &legacy_memory);
    VkPhysicalDeviceMemoryProperties2 modern_memory{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    memory2(physical, &modern_memory);
    const auto &m = modern_memory.memoryProperties;
    if (m.memoryTypeCount != legacy_memory.memoryTypeCount ||
        m.memoryHeapCount != legacy_memory.memoryHeapCount)
        throw ProbeMismatch("memory type/heap counts differ between legacy and Vulkan 1.1 queries");
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if (m.memoryTypes[i].propertyFlags != legacy_memory.memoryTypes[i].propertyFlags ||
            m.memoryTypes[i].heapIndex != legacy_memory.memoryTypes[i].heapIndex)
            throw ProbeMismatch("memory type properties differ between legacy and Vulkan 1.1 queries");
    for (uint32_t i = 0; i < m.memoryHeapCount; ++i)
        if (m.memoryHeaps[i].size != legacy_memory.memoryHeaps[i].size ||
            m.memoryHeaps[i].flags != legacy_memory.memoryHeaps[i].flags)
            throw ProbeMismatch("memory heap properties differ between legacy and Vulkan 1.1 queries");

    auto format2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFormatProperties2"));
    if (!format2) format2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFormatProperties2KHR"));
    if (!format2) throw ProbeMismatch("vkGetPhysicalDeviceFormatProperties2 is unavailable");
    VkFormatProperties legacy_format{};
    p.get<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
        physical, VK_FORMAT_R8G8B8A8_UNORM, &legacy_format);
    VkFormatProperties2 modern_format{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    format2(physical, VK_FORMAT_R8G8B8A8_UNORM, &modern_format);
    if (legacy_format.linearTilingFeatures != modern_format.formatProperties.linearTilingFeatures ||
        legacy_format.optimalTilingFeatures != modern_format.formatProperties.optimalTilingFeatures ||
        legacy_format.bufferFeatures != modern_format.formatProperties.bufferFeatures)
        throw ProbeMismatch("format properties differ between legacy and Vulkan 1.1 queries");

    auto image2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceImageFormatProperties2"));
    if (!image2) image2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceImageFormatProperties2KHR"));
    if (!image2) throw ProbeMismatch("vkGetPhysicalDeviceImageFormatProperties2 is unavailable");
    VkPhysicalDeviceImageFormatInfo2 image_info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.type = VK_IMAGE_TYPE_2D;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageFormatProperties legacy_image{};
    const auto legacy_image_result = p.get<PFN_vkGetPhysicalDeviceImageFormatProperties>(
        "vkGetPhysicalDeviceImageFormatProperties")(
        physical, image_info.format, image_info.type, image_info.tiling, image_info.usage,
        image_info.flags, &legacy_image);
    require(legacy_image_result, "legacy image format properties");
    VkImageFormatProperties2 modern_image{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    require(image2(physical, &image_info, &modern_image), "vkGetPhysicalDeviceImageFormatProperties2");
    const auto &mi = modern_image.imageFormatProperties;
    if (legacy_image.maxExtent.width != mi.maxExtent.width ||
        legacy_image.maxExtent.height != mi.maxExtent.height ||
        legacy_image.maxExtent.depth != mi.maxExtent.depth ||
        legacy_image.maxMipLevels != mi.maxMipLevels ||
        legacy_image.maxArrayLayers != mi.maxArrayLayers ||
        legacy_image.sampleCounts != mi.sampleCounts ||
        legacy_image.maxResourceSize != mi.maxResourceSize)
        throw ProbeMismatch("image format properties differ between legacy and Vulkan 1.1 queries");

    auto sparse2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSparseImageFormatProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceSparseImageFormatProperties2"));
    if (!sparse2) sparse2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSparseImageFormatProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR"));
    if (!sparse2) throw ProbeMismatch("vkGetPhysicalDeviceSparseImageFormatProperties2 is unavailable");
    auto sparse1 = p.get<PFN_vkGetPhysicalDeviceSparseImageFormatProperties>(
        "vkGetPhysicalDeviceSparseImageFormatProperties");
    uint32_t legacy_sparse_count = 0;
    sparse1(physical, image_info.format, image_info.type, VK_SAMPLE_COUNT_1_BIT,
            image_info.usage, image_info.tiling, &legacy_sparse_count, nullptr);
    std::vector<VkSparseImageFormatProperties> legacy_sparse(legacy_sparse_count);
    if (legacy_sparse_count)
        sparse1(physical, image_info.format, image_info.type, VK_SAMPLE_COUNT_1_BIT,
                image_info.usage, image_info.tiling, &legacy_sparse_count, legacy_sparse.data());
    VkPhysicalDeviceSparseImageFormatInfo2 sparse_info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2};
    sparse_info.format = image_info.format;
    sparse_info.type = image_info.type;
    sparse_info.samples = VK_SAMPLE_COUNT_1_BIT;
    sparse_info.usage = image_info.usage;
    sparse_info.tiling = image_info.tiling;
    uint32_t modern_sparse_count = 0;
    sparse2(physical, &sparse_info, &modern_sparse_count, nullptr);
    std::vector<VkSparseImageFormatProperties2> modern_sparse(modern_sparse_count);
    for (auto &properties : modern_sparse)
        properties.sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2;
    if (modern_sparse_count)
        sparse2(physical, &sparse_info, &modern_sparse_count, modern_sparse.data());
    if (legacy_sparse_count != modern_sparse_count)
        throw ProbeMismatch("sparse image format count differs between legacy and Vulkan 1.1 queries");
    for (uint32_t i = 0; i < legacy_sparse_count; ++i) {
        const auto &a = legacy_sparse[i];
        const auto &b = modern_sparse[i].properties;
        if (a.aspectMask != b.aspectMask || a.flags != b.flags ||
            a.imageGranularity.width != b.imageGranularity.width ||
            a.imageGranularity.height != b.imageGranularity.height ||
            a.imageGranularity.depth != b.imageGranularity.depth)
            throw ProbeMismatch("sparse image format properties differ between legacy and Vulkan 1.1 queries");
    }
    row("core11.queryChain", "PASS",
        "queue-family, memory, format, image-format and sparse-image-format 1.1 queries match legacy results",
        "functional");
}

void multiview_render_pass_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    auto get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!get2) get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2KHR"));
    if (!get2) throw ProbeMismatch("vkGetPhysicalDeviceFeatures2 is unavailable");
    VkPhysicalDeviceMultiviewFeatures features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features};
    get2(physical, &query);
    if (!features.multiview) {
        row("core11.multiviewRenderPass", "SKIP", "feature unavailable", "functional");
        return;
    }
    auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceProperties2"));
    if (!get_props) get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceProperties2KHR"));
    if (get_props) {
        VkPhysicalDeviceMultiviewProperties limits{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &limits};
        get_props(physical, &properties);
        if (limits.maxMultiviewViewCount < 2)
            throw ProbeMismatch("multiview feature reports fewer than two views");
    }
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    uint32_t view_mask = 0x3u;
    VkRenderPassMultiviewCreateInfo multiview{
        VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    multiview.subpassCount = 1;
    multiview.pViewMasks = &view_mask;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, &multiview};
    ci.attachmentCount = 1;
    ci.pAttachments = &attachment;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    const VkResult result = p.dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(
        p.device, &ci, nullptr, &render_pass);
    if (result != VK_SUCCESS)
        throw ProbeMismatch("multiview render pass creation failed while the feature is reported");

    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo vs_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vs_ci.codeSize = sizeof post_multiview_vert;
    vs_ci.pCode = post_multiview_vert;
    VkShaderModuleCreateInfo fs_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fs_ci.codeSize = sizeof post_multiview_frag;
    fs_ci.pCode = post_multiview_frag;
    if (p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
            p.device, &vs_ci, nullptr, &vertex) != VK_SUCCESS ||
        p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
            p.device, &fs_ci, nullptr, &fragment) != VK_SUCCESS) {
        if (vertex) p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(p.device, vertex, nullptr);
        if (fragment) p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(p.device, fragment, nullptr);
        p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(p.device, render_pass, nullptr);
        throw ProbeMismatch("multiview shader creation failed");
    }
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
        p.device, &layout_ci, nullptr, &layout);
    VkPipelineShaderStageCreateInfo stages[2]{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, 16, 16, 0, 1};
    VkRect2D scissor{{0, 0}, {16, 16}};
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkGraphicsPipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_ci.stageCount = 2;
    pipeline_ci.pStages = stages;
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterization;
    pipeline_ci.pMultisampleState = &multisample;
    pipeline_ci.pColorBlendState = &blend;
    pipeline_ci.layout = layout;
    pipeline_ci.renderPass = render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pipeline_result =
        layout ? p.dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(
                     p.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline)
               : VK_ERROR_INITIALIZATION_FAILED;
    if (pipeline_result != VK_SUCCESS || !pipeline) {
        if (pipeline) p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(p.device, pipeline, nullptr);
        if (layout) p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(p.device, layout, nullptr);
        p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(p.device, vertex, nullptr);
        p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(p.device, fragment, nullptr);
        p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(p.device, render_pass, nullptr);
        throw ProbeMismatch("multiview render pass pipeline creation failed");
    }

    auto get_memory = p.get<PFN_vkGetPhysicalDeviceMemoryProperties>(
        "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties memory_properties{};
    get_memory(physical, &memory_properties);
    const auto pick_memory = [&](uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        return UINT32_MAX;
    };

    VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_ci.extent = {16, 16, 1};
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 2;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    require(p.dev<PFN_vkCreateImage>("vkCreateImage")(p.device, &image_ci, nullptr, &image),
            "create multiview image");
    VkMemoryRequirements image_requirements{};
    p.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
        p.device, image, &image_requirements);
    const uint32_t image_type = pick_memory(image_requirements.memoryTypeBits, 0);
    VkMemoryAllocateInfo image_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    image_allocate.allocationSize = image_requirements.size;
    image_allocate.memoryTypeIndex = image_type;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    require(p.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(
                p.device, &image_allocate, nullptr, &image_memory), "allocate multiview image");
    require(p.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(
                p.device, image, image_memory, 0), "bind multiview image");
    VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_ci.image = image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    VkImageView image_view = VK_NULL_HANDLE;
    require(p.dev<PFN_vkCreateImageView>("vkCreateImageView")(
                p.device, &view_ci, nullptr, &image_view), "create multiview image view");
    VkFramebufferCreateInfo framebuffer_ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_ci.renderPass = render_pass;
    framebuffer_ci.attachmentCount = 1;
    framebuffer_ci.pAttachments = &image_view;
    framebuffer_ci.width = 16;
    framebuffer_ci.height = 16;
    framebuffer_ci.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    require(p.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(
                p.device, &framebuffer_ci, nullptr, &framebuffer), "create multiview framebuffer");

    VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_ci.size = 2 * 16 * 16 * 4;
    buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer readback = VK_NULL_HANDLE;
    require(p.dev<PFN_vkCreateBuffer>("vkCreateBuffer")(
                p.device, &buffer_ci, nullptr, &readback), "create multiview readback");
    VkMemoryRequirements buffer_requirements{};
    p.dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(
        p.device, readback, &buffer_requirements);
    const uint32_t buffer_type = pick_memory(buffer_requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo buffer_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    buffer_allocate.allocationSize = buffer_requirements.size;
    buffer_allocate.memoryTypeIndex = buffer_type;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    require(p.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(
                p.device, &buffer_allocate, nullptr, &readback_memory), "allocate readback");
    require(p.dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
                p.device, readback, readback_memory, 0), "bind readback");
    void *readback_ptr = nullptr;
    require(p.dev<PFN_vkMapMemory>("vkMapMemory")(
                p.device, readback_memory, 0, VK_WHOLE_SIZE, 0, &readback_ptr), "map readback");

    VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_ci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    require(p.dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
                p.device, &pool_ci, nullptr, &pool), "create multiview pool");
    VkCommandBufferAllocateInfo command_ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_ci.commandPool = pool;
    command_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_ci.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    require(p.dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
                p.device, &command_ci, &cb), "allocate multiview command buffer");
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    require(p.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cb, &begin_info),
            "begin multiview commands");
    VkImageMemoryBarrier image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.image = image;
    image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    image_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &image_barrier);
    VkClearValue clear{};
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo render_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render_begin.renderPass = render_pass;
    render_begin.framebuffer = framebuffer;
    render_begin.renderArea = {{0, 0}, {16, 16}};
    render_begin.clearValueCount = 1;
    render_begin.pClearValues = &clear;
    p.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(
        cb, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(
        cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    p.dev<PFN_vkCmdDraw>("vkCmdDraw")(cb, 3, 1, 0, 0);
    p.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cb);
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &image_barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
    copy.imageExtent = {16, 16, 1};
    p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    VkMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &host_barrier, 0, nullptr, 0, nullptr);
    require(p.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cb), "end multiview commands");

    VkQueue queue = VK_NULL_HANDLE;
    p.dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(p.device, family, 0, &queue);
    if (!queue) throw ProbeMismatch("graphics queue unavailable for multiview draw");
    VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    require(p.dev<PFN_vkCreateFence>("vkCreateFence")(p.device, &fence_ci, nullptr, &fence),
            "create multiview fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    require(p.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(
                queue, 1, &submit, fence), "submit multiview");
    require(p.dev<PFN_vkWaitForFences>("vkWaitForFences")(
                p.device, 1, &fence, VK_TRUE, 5000000000ull), "wait multiview");
    VkMappedMemoryRange invalidate{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    invalidate.memory = readback_memory;
    invalidate.size = VK_WHOLE_SIZE;
    p.dev<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(
        p.device, 1, &invalidate);
    const auto *pixels = static_cast<const uint8_t *>(readback_ptr);
    const auto pixel = [&](uint32_t layer) {
        return pixels + layer * 16 * 16 * 4 + (8 * 16 + 8) * 4;
    };
    const bool view0_red = pixel(0)[0] > 200 && pixel(0)[1] < 50;
    const bool view1_green = pixel(1)[0] < 50 && pixel(1)[1] > 200;
    p.dev<PFN_vkDestroyFence>("vkDestroyFence")(p.device, fence, nullptr);
    p.dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(p.device, pool, nullptr);
    p.dev<PFN_vkUnmapMemory>("vkUnmapMemory")(p.device, readback_memory);
    p.dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(p.device, readback, nullptr);
    p.dev<PFN_vkFreeMemory>("vkFreeMemory")(p.device, readback_memory, nullptr);
    p.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(p.device, framebuffer, nullptr);
    p.dev<PFN_vkDestroyImageView>("vkDestroyImageView")(p.device, image_view, nullptr);
    p.dev<PFN_vkDestroyImage>("vkDestroyImage")(p.device, image, nullptr);
    p.dev<PFN_vkFreeMemory>("vkFreeMemory")(p.device, image_memory, nullptr);
    p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(p.device, pipeline, nullptr);
    p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(p.device, layout, nullptr);
    p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(p.device, vertex, nullptr);
    p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(p.device, fragment, nullptr);
    p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(p.device, render_pass, nullptr);
    if (!view0_red || !view1_green)
        throw ProbeMismatch("multiview drawing did not select gl_ViewIndex per layer");
    row("core11.multiviewRenderPass", "PASS",
        "a two-view multiview render pass rendered gl_ViewIndex-selected colors into both layers",
        "functional");
}

void aggregate_repair_smoke(Probe &p, VkPhysicalDevice physical) {
    setenv("VULKAN_FIX_TEST_CORRUPT_CORE11_AGGREGATE", "1", 1);
    auto get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!get2) get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2KHR"));
    auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceProperties2"));
    if (!get_props) get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceProperties2KHR"));
    if (!get2 || !get_props) {
        unsetenv("VULKAN_FIX_TEST_CORRUPT_CORE11_AGGREGATE");
        throw ProbeMismatch("aggregate repair query entry points unavailable");
    }
    VkPhysicalDeviceVulkan11Features aggregate_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 feature_query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &aggregate_features};
    get2(physical, &feature_query);
    VkPhysicalDeviceMultiviewFeatures multiview{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceVariablePointersFeatures variable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
    VkPhysicalDeviceFeatures2 subfeature_query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &multiview};
    multiview.pNext = &variable;
    get2(physical, &subfeature_query);

    VkPhysicalDeviceVulkan11Properties aggregate_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceProperties2 property_query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &aggregate_properties};
    get_props(physical, &property_query);
    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceMultiviewProperties multiview_limits{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
    VkPhysicalDeviceProperties2 subproperty_query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
    subgroup.pNext = &multiview_limits;
    get_props(physical, &subproperty_query);
    unsetenv("VULKAN_FIX_TEST_CORRUPT_CORE11_AGGREGATE");

    if (aggregate_features.multiview != multiview.multiview ||
        aggregate_features.variablePointers != variable.variablePointers ||
        aggregate_properties.subgroupSize != subgroup.subgroupSize ||
        aggregate_properties.maxMultiviewViewCount != multiview_limits.maxMultiviewViewCount)
        throw ProbeMismatch("corrupted Vulkan11 aggregate was not repaired");
    row("core11.aggregateRepair", "PASS",
        "corrupted Vulkan11 aggregate values were rebuilt from promoted sub-structures",
        "functional");
}

void sparse_memory_requirements2_smoke(Probe &p, VkPhysicalDevice physical) {
    auto get_features = p.get<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures");
    VkPhysicalDeviceFeatures features{};
    get_features(physical, &features);
    VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_ci.flags = features.sparseBinding ? VK_IMAGE_CREATE_SPARSE_BINDING_BIT : 0;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_ci.extent = {64, 64, 1};
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    auto destroy_image = p.dev<PFN_vkDestroyImage>("vkDestroyImage");
    require(p.dev<PFN_vkCreateImage>("vkCreateImage")(p.device, &image_ci, nullptr, &image),
            "create sparse image");
    VkImageSparseMemoryRequirementsInfo2 info{
        VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2};
    info.image = image;
    auto query2 = p.dev<PFN_vkGetImageSparseMemoryRequirements2>(
        "vkGetImageSparseMemoryRequirements2");
    if (!query2) query2 = p.dev<PFN_vkGetImageSparseMemoryRequirements2>(
        "vkGetImageSparseMemoryRequirements2KHR");
    auto query1 = p.dev<PFN_vkGetImageSparseMemoryRequirements>(
        "vkGetImageSparseMemoryRequirements");
    uint32_t count2 = 0;
    query2(p.device, &info, &count2, nullptr);
    std::vector<VkSparseImageMemoryRequirements2> requirements2(count2);
    for (auto &entry : requirements2)
        entry = {VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2, nullptr};
    query2(p.device, &info, &count2, requirements2.data());
    uint32_t count1 = 0;
    query1(p.device, image, &count1, nullptr);
    std::vector<VkSparseImageMemoryRequirements> requirements1(count1);
    query1(p.device, image, &count1, requirements1.data());
    destroy_image(p.device, image, nullptr);
    if (count1 != count2 || count1 != requirements2.size())
        throw ProbeMismatch("sparse image memory requirements2 count mismatch");
    for (uint32_t i = 0; i < count1; ++i) {
        const auto &a = requirements1[i];
        const auto &b = requirements2[i].memoryRequirements;
        if (a.imageMipTailFirstLod != b.imageMipTailFirstLod ||
            a.imageMipTailSize != b.imageMipTailSize ||
            a.imageMipTailOffset != b.imageMipTailOffset ||
            a.imageMipTailStride != b.imageMipTailStride ||
            a.formatProperties.aspectMask != b.formatProperties.aspectMask ||
            a.formatProperties.flags != b.formatProperties.flags ||
            a.formatProperties.imageGranularity.width !=
                b.formatProperties.imageGranularity.width ||
            a.formatProperties.imageGranularity.height !=
                b.formatProperties.imageGranularity.height ||
            a.formatProperties.imageGranularity.depth !=
                b.formatProperties.imageGranularity.depth)
            throw ProbeMismatch("sparse image memory requirements2 differs from legacy");
    }
    row("core11.sparseMemoryRequirements2", "PASS",
        features.sparseBinding
            ? "vkGetImageSparseMemoryRequirements2 matches the legacy sparse-image query"
            : "vkGetImageSparseMemoryRequirements2 matches the legacy query on a non-sparse image",
        "functional");
}

void core11_properties_consistency(Probe &p, VkPhysicalDevice physical) {
    auto get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceProperties2"));
    if (!get2) get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceProperties2KHR"));
    if (!get2) throw ProbeMismatch("vkGetPhysicalDeviceProperties2 is unavailable");
    VkPhysicalDeviceVulkan11Properties aggregate{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceProperties2 aggregate_query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &aggregate};
    get2(physical, &aggregate_query);

    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDevicePointClippingProperties point_clipping{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES};
    VkPhysicalDeviceMultiviewProperties multiview{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
    VkPhysicalDeviceProtectedMemoryProperties protected_memory{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES};
    VkPhysicalDeviceMaintenance3Properties maintenance3{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    id.pNext = &subgroup;
    subgroup.pNext = &point_clipping;
    point_clipping.pNext = &multiview;
    multiview.pNext = &protected_memory;
    protected_memory.pNext = &maintenance3;
    VkPhysicalDeviceProperties2 separate_query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    get2(physical, &separate_query);

    const auto differ = [&](const char *name, uint64_t lhs, uint64_t rhs) {
        if (lhs == rhs) return;
        throw ProbeMismatch(std::string("VkPhysicalDeviceVulkan11Properties.") + name +
                            " disagrees with its native property struct");
    };
    differ("deviceUUID", std::memcmp(aggregate.deviceUUID, id.deviceUUID, VK_UUID_SIZE),
           0);
    differ("driverUUID", std::memcmp(aggregate.driverUUID, id.driverUUID, VK_UUID_SIZE),
           0);
    differ("deviceLUID", std::memcmp(aggregate.deviceLUID, id.deviceLUID, VK_LUID_SIZE), 0);
    differ("deviceNodeMask", aggregate.deviceNodeMask, id.deviceNodeMask);
    differ("deviceLUIDValid", aggregate.deviceLUIDValid, id.deviceLUIDValid);
    differ("subgroupSize", aggregate.subgroupSize, subgroup.subgroupSize);
    differ("subgroupSupportedStages", aggregate.subgroupSupportedStages,
           subgroup.supportedStages);
    differ("subgroupSupportedOperations", aggregate.subgroupSupportedOperations,
           subgroup.supportedOperations);
    differ("subgroupQuadOperationsInAllStages", aggregate.subgroupQuadOperationsInAllStages,
           subgroup.quadOperationsInAllStages);
    differ("pointClippingBehavior", aggregate.pointClippingBehavior,
           point_clipping.pointClippingBehavior);
    differ("maxMultiviewViewCount", aggregate.maxMultiviewViewCount,
           multiview.maxMultiviewViewCount);
    differ("maxMultiviewInstanceIndex", aggregate.maxMultiviewInstanceIndex,
           multiview.maxMultiviewInstanceIndex);
    differ("protectedNoFault", aggregate.protectedNoFault, protected_memory.protectedNoFault);
    differ("maxPerSetDescriptors", aggregate.maxPerSetDescriptors,
           maintenance3.maxPerSetDescriptors);
    differ("maxMemoryAllocationSize", aggregate.maxMemoryAllocationSize,
           maintenance3.maxMemoryAllocationSize);
    row("core11.propertiesAggregate", "PASS",
        "VkPhysicalDeviceVulkan11Properties agrees with every native 1.1 property struct",
        "functional");
}

void core11_feature_consistency(Probe &p, VkPhysicalDevice physical) {
    auto get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!get2) get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2KHR"));
    if (!get2) throw ProbeMismatch("vkGetPhysicalDeviceFeatures2 is unavailable");
    VkPhysicalDeviceVulkan11Features aggregate{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 aggregate_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                             &aggregate};
    get2(physical, &aggregate_query);

    VkPhysicalDevice16BitStorageFeatures storage16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceMultiviewFeatures multiview{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceVariablePointersFeatures variable_pointers{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
    VkPhysicalDeviceProtectedMemoryFeatures protected_memory{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    storage16.pNext = &multiview;
    multiview.pNext = &variable_pointers;
    variable_pointers.pNext = &protected_memory;
    protected_memory.pNext = &ycbcr;
    ycbcr.pNext = &draw_parameters;
    VkPhysicalDeviceFeatures2 separate_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                            &storage16};
    get2(physical, &separate_query);

    const auto differ = [&](const char *name, bool lhs, bool rhs) {
        if (lhs == rhs) return;
        throw ProbeMismatch(std::string("VkPhysicalDeviceVulkan11Features.") + name +
                            " disagrees with its native feature struct");
    };
    differ("storageBuffer16BitAccess", aggregate.storageBuffer16BitAccess,
           storage16.storageBuffer16BitAccess);
    differ("uniformAndStorageBuffer16BitAccess", aggregate.uniformAndStorageBuffer16BitAccess,
           storage16.uniformAndStorageBuffer16BitAccess);
    differ("storagePushConstant16", aggregate.storagePushConstant16, storage16.storagePushConstant16);
    differ("storageInputOutput16", aggregate.storageInputOutput16, storage16.storageInputOutput16);
    differ("multiview", aggregate.multiview, multiview.multiview);
    differ("multiviewGeometryShader", aggregate.multiviewGeometryShader,
           multiview.multiviewGeometryShader);
    differ("multiviewTessellationShader", aggregate.multiviewTessellationShader,
           multiview.multiviewTessellationShader);
    differ("variablePointersStorageBuffer", aggregate.variablePointersStorageBuffer,
           variable_pointers.variablePointersStorageBuffer);
    differ("variablePointers", aggregate.variablePointers, variable_pointers.variablePointers);
    differ("protectedMemory", aggregate.protectedMemory, protected_memory.protectedMemory);
    differ("samplerYcbcrConversion", aggregate.samplerYcbcrConversion,
           ycbcr.samplerYcbcrConversion);
    differ("shaderDrawParameters", aggregate.shaderDrawParameters,
           draw_parameters.shaderDrawParameters);
    row("core11.featureAggregate", "PASS",
        "VkPhysicalDeviceVulkan11Features agrees with every native 1.1 feature struct",
        "functional");
}

void device_group_enumeration_smoke(Probe &p, VkPhysicalDevice physical) {
    auto fn = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(
        p.gipa(p.instance, "vkEnumeratePhysicalDeviceGroups"));
    if (!fn) fn = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(
        p.gipa(p.instance, "vkEnumeratePhysicalDeviceGroupsKHR"));
    if (!fn) throw ProbeMismatch("vkEnumeratePhysicalDeviceGroups is unavailable");
    uint32_t count = 0;
    require(fn(p.instance, &count, nullptr), "enumerate device group count");
    if (!count || count > 65536) throw ProbeMismatch("invalid physical device group count");
    std::vector<VkPhysicalDeviceGroupProperties> groups(count);
    for (auto &group : groups) {
        group.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
        group.pNext = nullptr;
    }
    require(fn(p.instance, &count, groups.data()), "enumerate device groups");
    bool found = false;
    for (uint32_t g = 0; g < count && !found; ++g) {
        for (uint32_t d = 0; d < groups[g].physicalDeviceCount; ++d) {
            if (groups[g].physicalDevices[d] == physical) { found = true; break; }
        }
    }
    if (!found) throw ProbeMismatch("physical device is absent from every enumerated group");
    row("deviceGroup.enumerate", "PASS",
        "vkEnumeratePhysicalDeviceGroups returns a group containing the probed physical device",
        "functional");
}

void descriptor_update_template_smoke(Probe &p) {
    auto create_template = reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(
        p.gdpa(p.device, "vkCreateDescriptorUpdateTemplate"));
    if (!create_template) create_template = reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(
        p.gdpa(p.device, "vkCreateDescriptorUpdateTemplateKHR"));
    auto destroy_template = reinterpret_cast<PFN_vkDestroyDescriptorUpdateTemplate>(
        p.gdpa(p.device, "vkDestroyDescriptorUpdateTemplate"));
    if (!destroy_template) destroy_template = reinterpret_cast<PFN_vkDestroyDescriptorUpdateTemplate>(
        p.gdpa(p.device, "vkDestroyDescriptorUpdateTemplateKHR"));
    auto update_template = reinterpret_cast<PFN_vkUpdateDescriptorSetWithTemplate>(
        p.gdpa(p.device, "vkUpdateDescriptorSetWithTemplate"));
    if (!update_template) update_template = reinterpret_cast<PFN_vkUpdateDescriptorSetWithTemplate>(
        p.gdpa(p.device, "vkUpdateDescriptorSetWithTemplateKHR"));
    if (!create_template || !destroy_template || !update_template)
        throw ProbeMismatch("Vulkan 1.1 descriptor update template entry points are unavailable");

    auto create_layout = p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    auto destroy_layout = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    auto create_pool = p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool");
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    auto allocate_sets = p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    auto create_buffer = p.dev<PFN_vkCreateBuffer>("vkCreateBuffer");
    auto destroy_buffer = p.dev<PFN_vkDestroyBuffer>("vkDestroyBuffer");

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDescriptorUpdateTemplate tmpl = VK_NULL_HANDLE;
    try {
        require(create_layout(p.device, &layout_ci, nullptr, &layout), "create template layout");
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_ci.maxSets = 1;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &pool_size;
        require(create_pool(p.device, &pool_ci, nullptr, &pool), "create template pool");
        VkDescriptorSetAllocateInfo set_ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_ai.descriptorPool = pool;
        set_ai.descriptorSetCount = 1;
        set_ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        require(allocate_sets(p.device, &set_ai, &set), "allocate template set");
        VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_ci.size = 256;
        buffer_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        require(create_buffer(p.device, &buffer_ci, nullptr, &buffer), "create template buffer");

        VkDescriptorUpdateTemplateEntry entry{};
        entry.dstBinding = 0;
        entry.dstArrayElement = 0;
        entry.descriptorCount = 1;
        entry.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        entry.offset = 0;
        entry.stride = sizeof(VkDescriptorBufferInfo);
        VkDescriptorUpdateTemplateCreateInfo template_ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        template_ci.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        template_ci.descriptorUpdateEntryCount = 1;
        template_ci.pDescriptorUpdateEntries = &entry;
        template_ci.descriptorSetLayout = layout;
        require(create_template(p.device, &template_ci, nullptr, &tmpl),
                "create descriptor update template");
        VkDescriptorBufferInfo buffer_info{buffer, 0, 256};
        update_template(p.device, set, tmpl, &buffer_info);
        row("core11.descriptorUpdateTemplate", "PASS",
            "vkCreateDescriptorUpdateTemplate and vkUpdateDescriptorSetWithTemplate succeeded",
            "functional");
    } catch (...) {
        if (tmpl) destroy_template(p.device, tmpl, nullptr);
        if (buffer) destroy_buffer(p.device, buffer, nullptr);
        if (pool) destroy_pool(p.device, pool, nullptr);
        if (layout) destroy_layout(p.device, layout, nullptr);
        throw;
    }
    if (tmpl) destroy_template(p.device, tmpl, nullptr);
    if (buffer) destroy_buffer(p.device, buffer, nullptr);
    if (pool) destroy_pool(p.device, pool, nullptr);
    if (layout) destroy_layout(p.device, layout, nullptr);
}

void external_properties_smoke(Probe &p, VkPhysicalDevice physical) {
    auto buffer = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalBufferProperties>(
        p.gipa(p.instance, "vkGetPhysicalDeviceExternalBufferProperties"));
    if (!buffer) buffer = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalBufferProperties>(
        p.gipa(p.instance, "vkGetPhysicalDeviceExternalBufferPropertiesKHR"));
    auto fence = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalFenceProperties>(
        p.gipa(p.instance, "vkGetPhysicalDeviceExternalFenceProperties"));
    if (!fence) fence = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalFenceProperties>(
        p.gipa(p.instance, "vkGetPhysicalDeviceExternalFencePropertiesKHR"));
    auto semaphore = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
        p.gipa(p.instance, "vkGetPhysicalDeviceExternalSemaphoreProperties"));
    if (!semaphore) semaphore = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
        p.gipa(p.instance, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"));
    if (!buffer || !fence || !semaphore)
        throw ProbeMismatch("Vulkan 1.1 external property queries are unavailable");
    VkPhysicalDeviceExternalBufferInfo buffer_info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties buffer_properties{
        VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    buffer(physical, &buffer_info, &buffer_properties);
    const VkExternalMemoryFeatureFlags memory_mask =
        VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT |
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    if (buffer_properties.externalMemoryProperties.externalMemoryFeatures & ~memory_mask)
        throw ProbeMismatch("external buffer features contain undefined bits");

    VkPhysicalDeviceExternalFenceInfo fence_info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
    fence_info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalFenceProperties fence_properties{
        VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
    fence(physical, &fence_info, &fence_properties);
    const VkExternalFenceFeatureFlags fence_mask =
        VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT;
    if (fence_properties.externalFenceFeatures & ~fence_mask)
        throw ProbeMismatch("external fence features contain undefined bits");

    VkPhysicalDeviceExternalSemaphoreInfo semaphore_info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    semaphore_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalSemaphoreProperties semaphore_properties{
        VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    semaphore(physical, &semaphore_info, &semaphore_properties);
    const VkExternalSemaphoreFeatureFlags semaphore_mask =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    if (semaphore_properties.externalSemaphoreFeatures & ~semaphore_mask)
        throw ProbeMismatch("external semaphore features contain undefined bits");
    row("core11.externalProperties", "PASS",
        "buffer, fence and semaphore external capability queries returned defined feature bits",
        "functional");
}

void sampler_ycbcr_smoke(Probe &p, VkPhysicalDevice physical) {
    auto create = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
        p.gdpa(p.device, "vkCreateSamplerYcbcrConversion"));
    if (!create) create = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
        p.gdpa(p.device, "vkCreateSamplerYcbcrConversionKHR"));
    auto destroy = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
        p.gdpa(p.device, "vkDestroySamplerYcbcrConversion"));
    if (!destroy) destroy = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
        p.gdpa(p.device, "vkDestroySamplerYcbcrConversionKHR"));
    if (!create || !destroy)
        throw ProbeMismatch("Vulkan 1.1 sampler YCbCr conversion entry points are unavailable");
    const VkFormat format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    VkFormatProperties format_properties{};
    p.get<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
        physical, format, &format_properties);
    if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
        row("core11.samplerYcbcrConversion", "SKIP",
            "the representative multiplanar YCbCr format is not samplable", "functional");
        return;
    }
    VkSamplerYcbcrConversionCreateInfo ci{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    ci.format = format;
    ci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    ci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    ci.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    ci.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    ci.chromaFilter = VK_FILTER_LINEAR;
    ci.forceExplicitReconstruction = VK_FALSE;
    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    require(create(p.device, &ci, nullptr, &conversion), "create sampler YCbCr conversion");
    destroy(p.device, conversion, nullptr);
    row("core11.samplerYcbcrConversion", "PASS",
        "vkCreateSamplerYcbcrConversion and vkDestroySamplerYcbcrConversion succeeded",
        "functional");
}

void descriptor_set_layout_support_smoke(Probe &p) {
    auto query = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(
        p.gdpa(p.device, "vkGetDescriptorSetLayoutSupport"));
    if (!query) query = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(
        p.gdpa(p.device, "vkGetDescriptorSetLayoutSupportKHR"));
    if (!query) throw ProbeMismatch("vkGetDescriptorSetLayoutSupport is unavailable");
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    VkDescriptorSetLayoutSupport support{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
    query(p.device, &ci, &support);
    if (!support.supported)
        throw ProbeMismatch("a basic storage-buffer descriptor set layout is reported unsupported");
    row("core11.descriptorSetLayoutSupport", "PASS",
        "vkGetDescriptorSetLayoutSupport accepts a basic 1.1 layout", "functional");
}

void device_queue2_smoke(Probe &p, uint32_t family) {
    auto get2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
        p.gdpa(p.device, "vkGetDeviceQueue2"));
    auto khr = reinterpret_cast<PFN_vkGetDeviceQueue2>(
        p.gdpa(p.device, "vkGetDeviceQueue2KHR"));
    if (!get2) get2 = khr;
    if (!get2) throw ProbeMismatch("vkGetDeviceQueue2 is unavailable");
    auto get1 = p.dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
    VkDeviceQueueInfo2 info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    info.queueFamilyIndex = family;
    info.queueIndex = 0;
    VkQueue modern = VK_NULL_HANDLE;
    VkQueue legacy = VK_NULL_HANDLE;
    get2(p.device, &info, &modern);
    if (!modern && khr && khr != get2) khr(p.device, &info, &modern);
    get1(p.device, family, 0, &legacy);
    if (!modern || !legacy || modern != legacy)
        throw ProbeMismatch("vkGetDeviceQueue2(flags=0) did not return the vkGetDeviceQueue handle");
    row("deviceQueue2.sameQueue", "PASS",
        "flags==0 returns the same native queue as vkGetDeviceQueue", "functional");
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
        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_conversion{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
        VkPhysicalDevice16BitStorageFeatures storage16{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        auto append = [&](auto &value) { value.pNext = features.pNext; features.pNext = &value; };
        if (api >= VK_API_VERSION_1_1) append(draw);
        if (api >= VK_API_VERSION_1_1 || has("VK_KHR_16bit_storage")) append(storage16);
        if (api >= VK_API_VERSION_1_1 || has("VK_KHR_sampler_ycbcr_conversion"))
            append(ycbcr_conversion);
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
        feature("storageBuffer16BitAccess", storage16.storageBuffer16BitAccess);
        feature("samplerYcbcrConversion", ycbcr_conversion.samplerYcbcrConversion);
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
        VkPhysicalDeviceSamplerYcbcrConversionFeatures enable_ycbcr{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
        VkPhysicalDevice16BitStorageFeatures enable_storage16{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
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
        if (ycbcr_conversion.samplerYcbcrConversion) {
            enable_ycbcr.samplerYcbcrConversion = VK_TRUE;
            enable_ycbcr.pNext = const_cast<void *>(dci.pNext); dci.pNext = &enable_ycbcr;
            if (api < VK_API_VERSION_1_1)
                enabled_extensions.push_back("VK_KHR_sampler_ycbcr_conversion");
        }
        if (api >= VK_API_VERSION_1_1 && storage16.storageBuffer16BitAccess) {
            enable_storage16.storageBuffer16BitAccess = VK_TRUE;
            enable_storage16.uniformAndStorageBuffer16BitAccess =
                storage16.uniformAndStorageBuffer16BitAccess;
            enable_storage16.storagePushConstant16 = VK_FALSE;
            enable_storage16.storageInputOutput16 = VK_FALSE;
            enable_storage16.pNext = const_cast<void *>(dci.pNext); dci.pNext = &enable_storage16;
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
        if (api >= VK_API_VERSION_1_1) {
            run_case("core11.queryChain", [&] { core11_query_chain(probe, physical); });
            run_case("core11.externalProperties",
                     [&] { external_properties_smoke(probe, physical); });
            run_case("core11.propertiesAggregate",
                     [&] { core11_properties_consistency(probe, physical); });
            run_case("core11.multiviewRenderPass",
                     [&] { multiview_render_pass_smoke(probe, physical, family); });
            run_case("core11.featureAggregate",
                     [&] { core11_feature_consistency(probe, physical); });
            run_case("core11.sparseMemoryRequirements2",
                     [&] { sparse_memory_requirements2_smoke(probe, physical); });
            run_case("core11.aggregateRepair",
                     [&] { aggregate_repair_smoke(probe, physical); });
            if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                run_case("core11.dispatchBase",
                         [&] { dispatch_base_smoke(probe, physical, family, 0, 0, 0, 4,
                                                   "core11.dispatchBase"); });
                run_case("core11.dispatchBaseNonZero",
                         [&] { dispatch_base_smoke(probe, physical, family, 1, 0, 0, 3,
                                                   "core11.dispatchBaseNonZero"); });
                run_case("core11.dispatchBaseNonZero2",
                         [&] { dispatch_base_smoke(probe, physical, family, 2, 0, 0, 3,
                                                   "core11.dispatchBaseNonZero2"); });
                run_case("core11.dispatchBaseYZ",
                         [&] { dispatch_base_smoke(probe, physical, family, 0, 1, 1, 1,
                                                   "core11.dispatchBaseYZ"); });
                run_case("core11.dispatchBaseImage",
                         [&] { dispatch_base_image_smoke(probe, physical, family); });
                run_case("core11.dispatchBaseAtomic",
                         [&] { dispatch_base_atomic_smoke(probe, physical, family); });
                if (enable_storage16.storageBuffer16BitAccess)
                    run_case("core11.storage16",
                             [&] { storage16_smoke(probe, physical, family); });
                else
                    row("core11.storage16", "SKIP", "feature unavailable", "functional");
                run_case("core11.variablePointers",
                         [&] { variable_pointers_smoke(probe, physical, family); });
                run_case("core11.variablePointersStorageBuffer",
                         [&] { variable_pointers_storage_smoke(probe, physical, family); });
                run_case("core11.spirv13Downgrade",
                         [&] { spirv13_downgrade_smoke(probe, physical, family); });
            } else {
                row("core11.dispatchBase", "SKIP", "no compute queue", "functional");
                row("core11.dispatchBaseNonZero", "SKIP", "no compute queue", "functional");
                row("core11.dispatchBaseNonZero2", "SKIP", "no compute queue", "functional");
                row("core11.dispatchBaseYZ", "SKIP", "no compute queue", "functional");
                row("core11.dispatchBaseImage", "SKIP", "no compute queue", "functional");
                row("core11.dispatchBaseAtomic", "SKIP", "no compute queue", "functional");
                row("core11.storage16", "SKIP", "no compute queue", "functional");
                row("core11.variablePointers", "SKIP", "no compute queue", "functional");
                row("core11.variablePointersStorageBuffer", "SKIP", "no compute queue",
                    "functional");
                row("core11.spirv13Downgrade", "SKIP", "no compute queue", "functional");
            }
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                run_case("core11.negativeViewport",
                         [&] { negative_viewport_smoke(probe, physical, family, false, false,
                                                       false, false, false, false,
                                                       "core11.negativeViewport"); });
                run_case("core11.dynamicNegativeViewport",
                         [&] { negative_viewport_smoke(probe, physical, family, true, false,
                                                       false, false, false, false,
                                                       "core11.dynamicNegativeViewport"); });
                run_case("core11.dynamicNegativeViewportIndirect",
                         [&] { negative_viewport_smoke(probe, physical, family, true, true,
                                                       false, false, false, false,
                                                       "core11.dynamicNegativeViewportIndirect"); });
                run_case("core11.dynamicNegativeViewportIndexed",
                         [&] { negative_viewport_smoke(probe, physical, family, true, false,
                                                       true, false, false, false,
                                                       "core11.dynamicNegativeViewportIndexed"); });
                run_case("core11.dynamicNegativeViewportIndexedIndirect",
                         [&] { negative_viewport_smoke(probe, physical, family, true, true,
                                                       true, false, false, false,
                                                       "core11.dynamicNegativeViewportIndexedIndirect"); });
                run_case("core11.dynamicPositiveViewport",
                         [&] { negative_viewport_smoke(probe, physical, family, true, false,
                                                       false, true, false, false,
                                                       "core11.dynamicPositiveViewport"); });
                run_case("core11.staticMixedViewport",
                         [&] { negative_viewport_smoke(probe, physical, family, false, false,
                                                       false, true, true, false,
                                                       "core11.staticMixedViewport"); });
                run_case("core11.dynamicMixedViewport",
                         [&] { negative_viewport_smoke(probe, physical, family, true, false,
                                                       false, true, true, false,
                                                       "core11.dynamicMixedViewport"); });
                run_case("core11.staticMixedNegativeViewport",
                         [&] { negative_viewport_smoke(probe, physical, family, false, false,
                                                       false, false, true, false,
                                                       "core11.staticMixedNegativeViewport"); });
                run_case("core11.dynamicMixedNegativeViewport",
                         [&] { negative_viewport_smoke(probe, physical, family, true, false,
                                                       false, false, true, false,
                                                       "core11.dynamicMixedNegativeViewport"); });
                run_case("core11.staticConstantViewportIndex",
                         [&] { negative_viewport_smoke(probe, physical, family, false, false,
                                                       false, true, true, true,
                                                       "core11.staticConstantViewportIndex"); });
                run_case("core11.dynamicConstantViewportIndex",
                         [&] { negative_viewport_smoke(probe, physical, family, true, false,
                                                       false, true, true, true,
                                                       "core11.dynamicConstantViewportIndex"); });
            } else {
                row("core11.negativeViewport", "SKIP", "no graphics queue", "functional");
                row("core11.dynamicNegativeViewport", "SKIP", "no graphics queue", "functional");
                row("core11.dynamicNegativeViewportIndirect", "SKIP", "no graphics queue",
                    "functional");
                row("core11.dynamicNegativeViewportIndexed", "SKIP", "no graphics queue",
                    "functional");
                row("core11.dynamicNegativeViewportIndexedIndirect", "SKIP",
                    "no graphics queue", "functional");
                row("core11.dynamicPositiveViewport", "SKIP", "no graphics queue",
                    "functional");
                row("core11.staticMixedViewport", "SKIP", "no graphics queue", "functional");
                row("core11.dynamicMixedViewport", "SKIP", "no graphics queue", "functional");
                row("core11.staticMixedNegativeViewport", "SKIP", "no graphics queue",
                    "functional");
                row("core11.dynamicMixedNegativeViewport", "SKIP", "no graphics queue",
                    "functional");
                row("core11.staticConstantViewportIndex", "SKIP", "no graphics queue",
                    "functional");
                row("core11.dynamicConstantViewportIndex", "SKIP", "no graphics queue",
                    "functional");
            }
            run_case("deviceQueue2.sameQueue", [&] { device_queue2_smoke(probe, family); });
            run_case("core11.bufferMemory2", [&] { buffer_memory2_smoke(probe, physical); });
            run_case("core11.imageMemory2", [&] { image_memory2_smoke(probe, physical); });
            run_case("core11.descriptorUpdateTemplate",
                     [&] { descriptor_update_template_smoke(probe); });
            run_case("core11.descriptorSetLayoutSupport",
                     [&] { descriptor_set_layout_support_smoke(probe); });
            if (enable_ycbcr.samplerYcbcrConversion)
                run_case("core11.samplerYcbcrConversion",
                         [&] { sampler_ycbcr_smoke(probe, physical); });
            else
                row("core11.samplerYcbcrConversion", "SKIP", "feature unavailable", "functional");
            run_case("deviceGroup.enumerate",
                     [&] { device_group_enumeration_smoke(probe, physical); });
        } else {
            row("core11.queryChain", "SKIP", "Vulkan 1.1 query chain unavailable",
                "functional");
            row("core11.externalProperties", "SKIP", "Vulkan 1.1 external queries unavailable",
                "functional");
            row("core11.propertiesAggregate", "SKIP", "Vulkan 1.1 properties query unavailable",
                "functional");
            row("core11.multiviewRenderPass", "SKIP", "Vulkan 1.1 feature query unavailable",
                "functional");
            row("core11.featureAggregate", "SKIP", "Vulkan 1.1 feature query unavailable",
                "functional");
            row("core11.sparseMemoryRequirements2", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.aggregateRepair", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.dispatchBase", "SKIP", "Vulkan 1.1 device-group commands unavailable",
                "functional");
            row("core11.dispatchBaseNonZero", "SKIP",
                "Vulkan 1.1 device-group commands unavailable", "functional");
            row("core11.dispatchBaseNonZero2", "SKIP",
                "Vulkan 1.1 device-group commands unavailable", "functional");
            row("core11.dispatchBaseYZ", "SKIP",
                "Vulkan 1.1 device-group commands unavailable", "functional");
            row("core11.dispatchBaseImage", "SKIP",
                "Vulkan 1.1 device-group commands unavailable", "functional");
            row("core11.dispatchBaseAtomic", "SKIP",
                "Vulkan 1.1 device-group commands unavailable", "functional");
            row("core11.storage16", "SKIP", "Vulkan 1.1 feature query unavailable",
                "functional");
            row("core11.variablePointers", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.variablePointersStorageBuffer", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.spirv13Downgrade", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.negativeViewport", "SKIP", "Vulkan 1.1 feature query unavailable",
                "functional");
            row("core11.dynamicNegativeViewport", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.dynamicNegativeViewportIndirect", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.dynamicNegativeViewportIndexed", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.dynamicNegativeViewportIndexedIndirect", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.dynamicPositiveViewport", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.staticMixedViewport", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.dynamicMixedViewport", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.staticMixedNegativeViewport", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.dynamicMixedNegativeViewport", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.staticConstantViewportIndex", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("core11.dynamicConstantViewportIndex", "SKIP",
                "Vulkan 1.1 feature query unavailable", "functional");
            row("deviceQueue2.sameQueue", "SKIP", "Vulkan 1.1 query unavailable", "functional");
            row("core11.bufferMemory2", "SKIP", "Vulkan 1.1 memory entry points unavailable",
                "functional");
            row("core11.imageMemory2", "SKIP", "Vulkan 1.1 memory entry points unavailable",
                "functional");
            row("core11.descriptorUpdateTemplate", "SKIP",
                "Vulkan 1.1 descriptor update template entry points unavailable", "functional");
            row("core11.descriptorSetLayoutSupport", "SKIP",
                "Vulkan 1.1 descriptor set layout support query unavailable", "functional");
            row("core11.samplerYcbcrConversion", "SKIP", "Vulkan 1.1 feature query unavailable",
                "functional");
            row("deviceGroup.enumerate", "SKIP", "Vulkan 1.1 device group enumeration unavailable",
                "functional");
        }
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
    char core11_summary[160];
    std::snprintf(core11_summary, sizeof core11_summary,
                  "core11 probes: %u passed, %u skipped, %u failed",
                  core11_passes, core11_skips, core11_failures);
    row("core11.summary", core11_failures ? "FAIL" : "PASS", core11_summary, "functional");
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

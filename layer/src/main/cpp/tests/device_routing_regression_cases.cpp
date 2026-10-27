#include "core/layer_manager.h"
#include "shader/shader_rewriter.h"
#include <chrono>
#include <thread>
#include <time.h>

struct DeviceRoutingTestAccess {
    static std::unique_lock<std::shared_mutex> hold_tables(LayerManager& manager) {
        return std::unique_lock<std::shared_mutex>(manager.m_table_rw_mutex);
    }
    static uint32_t count(LayerManager& manager) { return manager.m_device_count.load(); }
    static bool metadata_empty(LayerManager& manager) {
        std::shared_lock<std::shared_mutex> tables(manager.m_table_rw_mutex);
        std::shared_lock<std::shared_mutex> commands(manager.m_cmd_device_rw_mutex);
        std::shared_lock<std::shared_mutex> info(manager.m_device_info_mutex);
        return manager.m_device_tables.empty() && manager.m_device_bypass_flags.empty() &&
               manager.m_cmd_devices.empty() && manager.m_queue_devices.empty() &&
               manager.m_device_queues.empty() && manager.m_device_spirv_versions.empty();
    }
};

namespace {
bool active = false;
unsigned native_indirect_calls = 0;
bool native_indirect_args = true;
const auto physical_a = (VkPhysicalDevice)(uintptr_t)0x810001;
const auto physical_b = (VkPhysicalDevice)(uintptr_t)0x810002;
const auto device_a = (VkDevice)(uintptr_t)0x820001;
const auto device_b = (VkDevice)(uintptr_t)0x820002;
class RoutingDivisorModule final : public IVulkanLayerModule {
public:
    const char* get_name() const override { return "VK_EXT_vertex_attribute_divisor"; }
    bool is_enabled() const override { return active; }
    bool is_device_native(VkDevice device) override { return device == device_b; }
};
VkResult VKAPI_CALL create(VkPhysicalDevice physical, const VkDeviceCreateInfo*,
                           const VkAllocationCallbacks*, VkDevice* out) {
    *out = physical == physical_a ? device_a : device_b;
    return VK_SUCCESS;
}
void VKAPI_CALL destroy(VkDevice, const VkAllocationCallbacks*) {}
void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* out) {
    *out = {};
    out->apiVersion = physical == physical_a ? VK_API_VERSION_1_0 : VK_API_VERSION_1_2;
}
VkResult VKAPI_CALL allocate(VkDevice device, const VkCommandBufferAllocateInfo*, VkCommandBuffer* out) {
    *out = (VkCommandBuffer)(uintptr_t(device) + 0x10000);
    return VK_SUCCESS;
}
void VKAPI_CALL queue(VkDevice device, uint32_t, uint32_t, VkQueue* out) {
    *out = (VkQueue)(uintptr_t(device) + 0x20000);
}
void VKAPI_CALL draw_indirect(VkCommandBuffer, VkBuffer buffer, VkDeviceSize offset,
                             uint32_t count, uint32_t stride) {
    ++native_indirect_calls;
    native_indirect_args &= buffer == (VkBuffer)uint64_t(0x123) && offset == 12 && count == 3 && stride == 20;
}
}

void* device_routing_mock_proc(const char* name) {
    if (!active) return nullptr;
    if (!strcmp(name, "vkCreateDevice")) return (void*)create;
    if (!strcmp(name, "vkDestroyDevice")) return (void*)destroy;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (void*)properties;
    if (!strcmp(name, "vkAllocateCommandBuffers")) return (void*)allocate;
    if (!strcmp(name, "vkGetDeviceQueue")) return (void*)queue;
    if (!strcmp(name, "vkCmdDrawIndirect")) return (void*)draw_indirect;
    return nullptr;
}

// Lifecycle stress logs belong in the test output, not the phone's logcat ring.
extern "C" int __android_log_vprint(int, const char*, const char*, va_list) { return 0; }

void run_device_routing_regressions(void (*check)(bool, const char*)) {
    active = true;
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    auto& manager = LayerManager::get();
    manager.register_module(std::make_unique<RoutingDivisorModule>());
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    VkDevice a{}, b{};
    check(manager.dispatch_create_device(physical_a, &info, nullptr, &a) == VK_SUCCESS && a == device_a,
          "create persistent logical device through the real layer lifecycle");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandBufferCount = 1;
    VkCommandBuffer command{};
    check(manager.dispatch_allocate_command_buffers(a, &allocation, &command) == VK_SUCCESS,
          "record command ownership through public allocation dispatch");
    VkQueue owned_queue{};
    manager.dispatch_get_device_queue(a, 0, 0, &owned_queue);
    check(manager.get_device_for_cmd(command) == a && manager.get_device_for_queue(owned_queue) == a,
          "single-device command and queue owners agree");
    check(manager.dispatch_create_device(physical_b, &info, nullptr, &b) == VK_SUCCESS && b == device_b,
          "create second device with a different native SPIR-V ceiling");
    bool reached = false;
    {
        auto gate = DeviceRoutingTestAccess::hold_tables(manager);
        std::thread retire([&] { manager.dispatch_destroy_device(b, nullptr); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (DeviceRoutingTestAccess::count(manager) != 1 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        reached = DeviceRoutingTestAccess::count(manager) == 1;
        check(reached, "device retirement reaches the dispatch-table publication gate");
        check(manager.get_device_for_cmd(command) == a,
              "retiring another device cannot redirect a surviving command buffer");
        check(manager.get_device_for_queue(owned_queue) == a,
              "retiring another device cannot redirect a surviving queue");
        check(!manager.is_divisor_bypass_active(),
              "retiring a native device cannot bypass the surviving device's emulation");
        gate.unlock();
        retire.join();
    }
    check(manager.get_device_max_spirv_version(a) == ShaderRewriter::api_version_to_spirv(VK_API_VERSION_1_0),
          "surviving device keeps its own native SPIR-V ceiling after primary retirement");

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> misroutes{0}, queries{0};
    std::thread reader([&] {
        while (!stop.load()) {
            if (manager.get_device_for_cmd(command) != a || manager.get_device_for_queue(owned_queue) != a)
                misroutes.fetch_add(1);
            queries.fetch_add(1);
        }
    });
    bool created = true;
    for (unsigned i = 0; i < 512; ++i) {
        if (manager.dispatch_create_device(physical_b, &info, nullptr, &b) != VK_SUCCESS) {
            created = false;
            break;
        }
        manager.dispatch_destroy_device(b, nullptr);
    }
    stop.store(true);
    reader.join();
    printf("Concurrent command/queue misroutes: %llu / %llu queries\n",
           (unsigned long long)misroutes.load(), (unsigned long long)queries.load());
    check(created && queries.load() && misroutes.load() == 0,
          "512 other-device create/destroy cycles preserve concurrent command and queue ownership");
    manager.dispatch_destroy_device(a, nullptr);
    check(manager.get_device_for_cmd(command) == VK_NULL_HANDLE &&
              manager.get_device_for_queue(owned_queue) == VK_NULL_HANDLE,
          "last device destruction clears command and queue ownership");
    check(manager.dispatch_create_device(physical_b, &info, nullptr, &b) == VK_SUCCESS &&
              manager.is_divisor_bypass_active(),
          "a sole native device restores the lock-free drawing bypass");
    VkCommandBuffer command_b{};
    VkQueue queue_b{};
    manager.dispatch_allocate_command_buffers(b, &allocation, &command_b);
    manager.dispatch_get_device_queue(b, 0, 0, &queue_b);
    manager.dispatch_cmd_draw_indirect(command_b, (VkBuffer)uint64_t(0x123), 12, 3, 20);
    check(native_indirect_calls == 1 && native_indirect_args,
          "sole native device forwards indirect drawing once with unchanged arguments");
    timespec start{}, end{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);
    unsigned invalid = 0;
    for (unsigned i = 0; i < 1000000; ++i)
        invalid += manager.get_device_for_cmd(command_b) != b || manager.get_device_for_queue(queue_b) != b;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);
    printf("Single-device owner lookup CPU: %lld us / 1000000 command+queue pairs\n",
           (long long)((int64_t(end.tv_sec - start.tv_sec) * 1000000000 + end.tv_nsec - start.tv_nsec) / 1000));
    check(invalid == 0, "one million hot command/queue lookups keep the sole device owner");
    manager.dispatch_create_device(physical_a, &info, nullptr, &a);
    check(!manager.is_divisor_bypass_active(), "adding an emulated device disables the global drawing bypass");
    manager.dispatch_destroy_device(a, nullptr);
    check(manager.is_divisor_bypass_active() && manager.get_device_for_cmd(command_b) == b &&
              manager.get_device_for_queue(queue_b) == b,
          "retiring the emulated device restores the native survivor's owner and bypass");
    check(manager.get_device_max_spirv_version(b) == ShaderRewriter::api_version_to_spirv(VK_API_VERSION_1_2),
          "native survivor retains its SPIR-V ceiling when the emulated primary retires");
    manager.dispatch_free_command_buffers(b, allocation.commandPool, 1, &command_b);
    manager.dispatch_destroy_device(b, nullptr);
    check(DeviceRoutingTestAccess::count(manager) == 0 && DeviceRoutingTestAccess::metadata_empty(manager),
          "device churn retires every device-owned routing and shader metadata entry");
    active = false;
}

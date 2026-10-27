#include "core/layer_manager.h"
#include "modules/vulkan_1_4/vertex_attribute_divisor.h"
#include <array>
#include <cstring>
#include <unordered_set>
#include <thread>

struct CommandPoolTestAccess {
    static size_t retained(LayerManager& manager) {
        std::shared_lock<std::shared_mutex> lock(manager.m_cmd_device_rw_mutex);
        return manager.m_cmd_devices.size();
    }
    static bool pools_empty(LayerManager& manager) {
        std::shared_lock<std::shared_mutex> lock(manager.m_cmd_device_rw_mutex);
        return manager.m_pool_commands.empty();
    }
};

struct DivisorCommandTestAccess {
    static void enable_tracking(VertexAttributeDivisorModule& module) {
        module.m_has_divisor_pipelines.store(true);
    }
    static bool bound(VertexAttributeDivisorModule& module, VkCommandBuffer command, VkBuffer buffer) {
        std::lock_guard<std::mutex> lock(module.m_cmd_mutex);
        auto it = module.m_cmd_states.find((uint64_t)(uintptr_t)command);
        return it != module.m_cmd_states.end() && it->second->bound_buffers[0] == buffer;
    }
};

namespace {
bool active = false;
uintptr_t next_command = 0x940000;
unsigned native_frees = 0;
bool fail_allocation = false;
const auto physical = (VkPhysicalDevice)(uintptr_t)0x910001;
const auto device = (VkDevice)(uintptr_t)0x920001;

struct Observer final : IVulkanLayerModule {
    const char* name;
    std::unordered_set<VkCommandBuffer> commands;
    unsigned resets = 0, frees = 0;
    explicit Observer(const char* name) : name(name) {}
    const char* get_name() const override { return name; }
    bool is_enabled() const override { return active; }
    void on_post_allocate_command_buffers(VkDevice, const VkCommandBufferAllocateInfo* info,
                                         VkResult, VkCommandBuffer* out) override {
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) commands.insert(out[i]);
    }
    void on_free_command_buffers(VkDevice, uint32_t count, const VkCommandBuffer* out) override {
        for (uint32_t i = 0; i < count; ++i) frees += unsigned(commands.erase(out[i]));
    }
    void on_reset_command_buffer(VkCommandBuffer cmd, VkCommandBufferResetFlags) override {
        resets += commands.count(cmd) != 0;
    }
};
VkResult VKAPI_CALL create(VkPhysicalDevice, const VkDeviceCreateInfo*,
                           const VkAllocationCallbacks*, VkDevice* out) {
    *out = device;
    return VK_SUCCESS;
}
void VKAPI_CALL destroy(VkDevice, const VkAllocationCallbacks*) {}
void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    *out = {};
    out->apiVersion = VK_API_VERSION_1_0;
}
VkResult VKAPI_CALL allocate(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
    if (fail_allocation) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i)
        out[i] = (VkCommandBuffer)++next_command;
    return VK_SUCCESS;
}
void VKAPI_CALL free_commands(VkDevice, VkCommandPool, uint32_t count, const VkCommandBuffer*) {
    native_frees += count;
}
}

void* command_pool_mock_proc(const char* name) {
    if (!active) return nullptr;
    if (!strcmp(name, "vkCreateDevice")) return (void*)create;
    if (!strcmp(name, "vkDestroyDevice")) return (void*)destroy;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (void*)properties;
    if (!strcmp(name, "vkAllocateCommandBuffers")) return (void*)allocate;
    if (!strcmp(name, "vkFreeCommandBuffers")) return (void*)free_commands;
    return nullptr;
}

void run_command_pool_regressions(void (*check)(bool, const char*)) {
    active = true;
    auto& manager = LayerManager::get();
    std::array<Observer*, 4> observers{};
    const char* names[] = {"VK_KHR_push_descriptor", "VK_EXT_vertex_attribute_divisor",
                          "VK_KHR_dynamic_rendering", "VK_KHR_draw_indirect_count"};
    for (size_t i = 0; i < observers.size(); ++i) {
        auto observer = std::make_unique<Observer>(names[i]);
        observers[i] = observer.get();
        manager.register_module(std::move(observer));
    }
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice out{};
    check(manager.dispatch_create_device(physical, &info, nullptr, &out) == VK_SUCCESS,
          "create device for command-pool lifetime regression");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = (VkCommandPool)uint64_t(0x123400000055);
    allocation.commandBufferCount = 3;
    std::array<VkCommandBuffer, 3> commands{};
    manager.dispatch_allocate_command_buffers(out, &allocation, commands.data());
    const auto baseline = CommandPoolTestAccess::retained(manager) - commands.size();
    manager.track_command_pool_reset(out, allocation.commandPool, false);
    bool reset_all = true;
    for (auto observer : observers) reset_all &= observer->resets == 3 && observer->commands.size() == 3;
    check(reset_all && CommandPoolTestAccess::retained(manager) == baseline + 3,
          "pool reset clears every module's recording state while preserving command ownership");
    manager.dispatch_free_command_buffers(out, allocation.commandPool, 1, commands.data());
    manager.track_command_pool_reset(out, allocation.commandPool, true);
    bool freed_all = true;
    for (auto observer : observers) freed_all &= observer->frees == 3 && observer->commands.empty();
    check(freed_all && CommandPoolTestAccess::retained(manager) == baseline,
          "pool destruction implicitly retires all module and routing command metadata");
    check(native_frees == 1, "implicit pool retirement does not double-free native command buffers");

    bool bounded = true;
    for (unsigned i = 0; i < 512; ++i) {
        allocation.commandPool = (VkCommandPool)uint64_t(0x123400001000 + i);
        manager.dispatch_allocate_command_buffers(out, &allocation, commands.data());
        manager.track_command_pool_reset(out, allocation.commandPool, true);
        bounded &= CommandPoolTestAccess::retained(manager) == baseline;
    }
    for (auto observer : observers) bounded &= observer->commands.empty();
    check(bounded, "512 implicit pool destructions leave no command metadata on the live device");
    const auto before_failure = CommandPoolTestAccess::retained(manager);
    fail_allocation = true;
    const auto failed = manager.dispatch_allocate_command_buffers(out, &allocation, commands.data());
    fail_allocation = false;
    manager.track_command_pool_reset(out, allocation.commandPool, true);
    check(failed == VK_ERROR_OUT_OF_HOST_MEMORY && CommandPoolTestAccess::retained(manager) == before_failure,
          "failed command allocation publishes no pool ownership");
    allocation.commandPool = (VkCommandPool)uint64_t(0x111100000055);
    manager.dispatch_allocate_command_buffers(out, &allocation, commands.data());
    auto other = allocation;
    other.commandPool = (VkCommandPool)uint64_t(0x222200000055);
    std::array<VkCommandBuffer, 3> other_commands{};
    manager.dispatch_allocate_command_buffers(out, &other, other_commands.data());
    manager.track_command_pool_reset(out, allocation.commandPool, true);
    check(CommandPoolTestAccess::retained(manager) == baseline + 3,
          "retiring a pool preserves another pool whose handle has identical low 32 bits");
    manager.dispatch_free_command_buffers(out, other.commandPool, 3, other_commands.data());
    check(CommandPoolTestAccess::retained(manager) == baseline && CommandPoolTestAccess::pools_empty(manager),
          "explicit freeing of a pool's final command removes its reverse index");
    manager.dispatch_destroy_device(out, nullptr);
    active = false;

    // The Vulkan driver may reuse a freed command handle. Allocation and free
    // are fully ordered here; a TLS cache must never keep a dangling state.
    VertexAttributeDivisorModule divisor;
    DivisorCommandTestAccess::enable_tracking(divisor);
    VkBuffer buffer = (VkBuffer)uint64_t(0x9876);
    VkDeviceSize offset = 16;
    divisor.on_post_allocate_command_buffers(out, &allocation, VK_SUCCESS, commands.data());
    divisor.on_cmd_bind_vertex_buffers(commands[0], 0, 1, &buffer, &offset);
    divisor.on_free_command_buffers(out, 3, commands.data());
    divisor.on_post_allocate_command_buffers(out, &allocation, VK_SUCCESS, commands.data());
    divisor.on_cmd_bind_vertex_buffers(commands[0], 0, 1, &buffer, &offset);
    divisor.on_free_command_buffers(out, 3, commands.data());
    std::atomic<unsigned> phase{0};
    std::thread recorder([&] {
        divisor.on_cmd_bind_pipeline(commands[0], VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)uint64_t(0x88));
        divisor.on_cmd_bind_vertex_buffers(commands[0], 0, 1, &buffer, &offset);
        phase.store(1);
        while (phase.load() != 2) std::this_thread::yield();
        divisor.on_cmd_bind_vertex_buffers(commands[0], 0, 1, &buffer, &offset);
    });
    while (phase.load() != 1) std::this_thread::yield();
    divisor.on_free_command_buffers(out, 3, commands.data());
    divisor.on_post_allocate_command_buffers(out, &allocation, VK_SUCCESS, commands.data());
    phase.store(2);
    recorder.join();
    check(DivisorCommandTestAccess::bound(divisor, commands[0], buffer),
          "ordered cross-thread command reuse replaces retired TLS recording state");
    divisor.on_free_command_buffers(out, 3, commands.data());
    divisor.on_post_allocate_command_buffers(out, &allocation, VK_SUCCESS, commands.data());
    divisor.on_cmd_bind_vertex_buffers(commands[0], 0, 1, &buffer, &offset);
    check(DivisorCommandTestAccess::bound(divisor, commands[0], buffer),
          "same-thread command reuse replaces retired TLS recording state");
    divisor.on_free_command_buffers(out, 3, commands.data());
}

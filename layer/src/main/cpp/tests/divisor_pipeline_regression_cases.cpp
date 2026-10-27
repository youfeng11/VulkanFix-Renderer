#include "core/layer_manager.h"
#include "modules/vulkan_1_4/vertex_attribute_divisor.h"
#include <array>
#include <cstring>
#include <thread>
#include <time.h>

struct DivisorPipelineTestAccess {
    static void physical(VertexAttributeDivisorModule& module, VkPhysicalDevice physical, bool native) {
        std::unique_lock<std::shared_mutex> lock(module.m_rw_mutex);
        module.m_phys_devices[(uint64_t)(uintptr_t)physical] = {native, native, native, native, true};
    }
    static size_t pending(VertexAttributeDivisorModule& module) {
        std::shared_lock<std::shared_mutex> lock(module.m_rw_mutex);
        return module.m_pending_divisors.size();
    }
    static size_t pipelines(VertexAttributeDivisorModule& module) {
        std::shared_lock<std::shared_mutex> lock(module.m_rw_mutex);
        return module.m_pipeline_divisors.size();
    }
    static uint32_t divisor(VertexAttributeDivisorModule& module, VkCommandBuffer command) {
        std::lock_guard<std::mutex> lock(module.m_cmd_mutex);
        auto it = module.m_cmd_states.find((uint64_t)(uintptr_t)command);
        if (it == module.m_cmd_states.end() || !it->second->active_divisor_info ||
            it->second->active_divisor_info->divisors.empty()) return 0;
        return it->second->active_divisor_info->divisors[0].divisor;
    }
};

namespace {
bool active = false;
unsigned failure_mode = 0;
uint64_t pipeline_base = 0x123400006000;
uintptr_t command_serial = 0x734000;
bool rewritten_strides = true;
const auto physical_a = (VkPhysicalDevice)(uintptr_t)0x731001;
const auto device_a = (VkDevice)(uintptr_t)0x732001;
const auto device_b = (VkDevice)(uintptr_t)0x732002;
class EmulatedDivisor final : public VertexAttributeDivisorModule {
    bool is_enabled() const override { return active; }
    bool is_device_native(VkDevice) override { return false; }
};
VkResult VKAPI_CALL create(VkPhysicalDevice physical, const VkDeviceCreateInfo*,
                           const VkAllocationCallbacks*, VkDevice* out) {
    *out = physical == physical_a ? device_a : device_b;
    return VK_SUCCESS;
}
void VKAPI_CALL destroy(VkDevice, const VkAllocationCallbacks*) {}
VkResult VKAPI_CALL allocate(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) out[i] = (VkCommandBuffer)++command_serial;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL pipelines(VkDevice, VkPipelineCache, uint32_t count,
                              const VkGraphicsPipelineCreateInfo* infos,
                              const VkAllocationCallbacks*, VkPipeline* out) {
    for (uint32_t i = 0; i < count; ++i) {
        rewritten_strides &= infos[i].pVertexInputState &&
            infos[i].pVertexInputState->pVertexBindingDescriptions[0].stride == 0;
        out[i] = failure_mode == 2 || (failure_mode == 1 && i != 0)
                     ? VK_NULL_HANDLE : (VkPipeline)(pipeline_base + i);
    }
    return failure_mode ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
void VKAPI_CALL destroy_pipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}
struct Batch {
    std::array<VkGraphicsPipelineCreateInfo, 2> infos{};
    std::array<VkPipelineVertexInputStateCreateInfo, 2> inputs{};
    std::array<VkPipelineVertexInputDivisorStateCreateInfoEXT, 2> extensions{};
    std::array<VkVertexInputBindingDivisorDescriptionEXT, 2> divisors{};
    std::array<VkVertexInputBindingDescription, 2> bindings{};
    Batch(uint32_t a, uint32_t b) {
        for (size_t i = 0; i < infos.size(); ++i) {
            infos[i].sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            infos[i].pVertexInputState = &inputs[i];
            inputs[i].sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            inputs[i].pNext = &extensions[i];
            inputs[i].vertexBindingDescriptionCount = 1;
            inputs[i].pVertexBindingDescriptions = &bindings[i];
            extensions[i].sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
            extensions[i].vertexBindingDivisorCount = 1;
            extensions[i].pVertexBindingDivisors = &divisors[i];
            divisors[i] = {0, i ? b : a};
            bindings[i] = {0, 16, VK_VERTEX_INPUT_RATE_INSTANCE};
        }
    }
};
}

void* divisor_pipeline_mock_proc(const char* name) {
    if (!active) return nullptr;
    if (!strcmp(name, "vkCreateDevice")) return (void*)create;
    if (!strcmp(name, "vkDestroyDevice")) return (void*)destroy;
    if (!strcmp(name, "vkAllocateCommandBuffers")) return (void*)allocate;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (void*)pipelines;
    if (!strcmp(name, "vkDestroyPipeline")) return (void*)destroy_pipeline;
    return nullptr;
}

void run_divisor_pipeline_regressions(void (*check)(bool, const char*)) {
    active = true;
    auto& manager = LayerManager::get();
    auto owned = std::make_unique<EmulatedDivisor>();
    auto& module = *owned;
    manager.register_module(std::move(owned));
    VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice a{}, b{};
    manager.dispatch_create_device(physical_a, &create_info, nullptr, &a);
    manager.dispatch_create_device((VkPhysicalDevice)(uintptr_t)0x731002, &create_info, nullptr, &b);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = (VkCommandPool)uint64_t(0x123400001000);
    allocation.commandBufferCount = 1;
    VkCommandBuffer command_a{}, command_b{};
    manager.dispatch_allocate_command_buffers(a, &allocation, &command_a);
    manager.dispatch_allocate_command_buffers(b, &allocation, &command_b);
    auto bind = [&](VkCommandBuffer cmd, VkPipeline pipeline) {
        module.on_cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        return DivisorPipelineTestAccess::divisor(module, cmd);
    };
    auto compile = [&](VkDevice device, Batch& batch, uint32_t count, VkPipeline* out) {
        return manager.dispatch_create_graphics_pipelines(device, VK_NULL_HANDLE, count,
                                                           batch.infos.data(), nullptr, out);
    };
    Batch initial(2, 3);
    VkPipeline result[2]{};
    check(compile(a, initial, 2, result) == VK_SUCCESS && rewritten_strides,
          "batched divisor pipelines rewrite native instance strides");
    check(bind(command_a, result[0]) == 2 && bind(command_a, result[1]) == 3 &&
              DivisorPipelineTestAccess::pending(module) == 0,
          "every pipeline in a batch receives its own divisor and releases pending metadata");
    bind(command_a, result[0]);
    manager.dispatch_destroy_pipeline(a, result[0], nullptr);
    Batch replacement(5, 1);
    VkPipeline replaced{};
    compile(a, replacement, 1, &replaced);
    check(bind(command_a, replaced) == 5, "reused pipeline handle invalidates cached divisor definitions");
    Batch foreign(7, 1);
    VkPipeline same_handle{};
    compile(b, foreign, 1, &same_handle);
    check(bind(command_b, same_handle) == 7 && bind(command_a, replaced) == 5,
          "equal pipeline handles on different devices retain independent divisor definitions");
    manager.dispatch_destroy_pipeline(b, same_handle, nullptr);
    bind(command_a, result[1]);
    check(bind(command_a, replaced) == 5, "destroying a foreign pipeline preserves the surviving definition");
    manager.dispatch_destroy_pipeline(a, replaced, nullptr);
    manager.dispatch_destroy_pipeline(a, result[1], nullptr);

    pipeline_base += 0x100;
    failure_mode = 1;
    Batch partial(11, 13);
    const auto partial_result = compile(a, partial, 2, result);
    check(partial_result == VK_ERROR_OUT_OF_DEVICE_MEMORY && result[0] && !result[1] &&
              bind(command_a, result[0]) == 11 && DivisorPipelineTestAccess::pending(module) == 0,
          "partial batch failure retains successful pipeline metadata and retires every pending entry");
    manager.dispatch_destroy_pipeline(a, result[0], nullptr);
    failure_mode = 2;
    bool bounded = true;
    for (unsigned i = 0; i < 64; ++i) {
        compile(a, partial, 2, result);
        bounded &= DivisorPipelineTestAccess::pending(module) == 0;
    }
    check(bounded, "repeated failed pipeline batches leave no pending divisor metadata");
    failure_mode = 0;
    manager.track_command_pool_reset(a, allocation.commandPool, true);
    manager.track_command_pool_reset(b, allocation.commandPool, true);
    manager.dispatch_destroy_device(b, nullptr);
    manager.dispatch_destroy_device(a, nullptr);
    check(DivisorPipelineTestAccess::pending(module) == 0 &&
              DivisorPipelineTestAccess::pipelines(module) == 0,
          "pipeline and device destruction retire all divisor compilation metadata");
    active = false;

    // Exercise the concrete module's capability cache as well as its base
    // class: separate atomics for a cached device and flag can tear here.
    VertexAttributeDivisorModule probe;
    const auto native_physical = (VkPhysicalDevice)(uintptr_t)0x738001;
    const auto emulated_physical = (VkPhysicalDevice)(uintptr_t)0x738002;
    DivisorPipelineTestAccess::physical(probe, native_physical, true);
    DivisorPipelineTestAccess::physical(probe, emulated_physical, false);
    probe.on_post_create_device(native_physical, a, VK_SUCCESS, nullptr);
    probe.on_post_create_device(emulated_physical, b, VK_SUCCESS, nullptr);
    std::atomic<bool> start{false};
    std::atomic<unsigned> errors{0};
    std::array<std::thread, 8> threads;
    for (unsigned i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            unsigned local = 0;
            for (unsigned n = 0; n < 200000; ++n) {
                const bool native = ((i+n)&1) == 0;
                local += probe.is_device_native(native ? a : b) != native;
            }
            errors.fetch_add(local);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    printf("Concurrent divisor capability misroutes: %u / 1600000 queries\n", errors.load());
    check(errors.load() == 0, "concrete divisor module never mixes concurrent device capability results");
    probe.on_destroy_device(a);
    check(!probe.is_device_native(a) && !probe.is_device_native(b),
          "divisor device destruction invalidates its cached capability");
    probe.on_post_create_device(emulated_physical, a, VK_SUCCESS, nullptr);
    check(!probe.is_device_native(a), "reused divisor device handle does not inherit old hardware support");
    probe.on_destroy_device(a);
    probe.on_post_create_device(native_physical, a, VK_SUCCESS, nullptr);
    unsigned native_count = 0;
    timespec before{}, after{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &before);
    for (unsigned i = 0; i < 1000000; ++i) native_count += probe.is_device_native(a);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &after);
    printf("Divisor cached capability CPU: %lld us / 1000000 queries\n",
        (long long)((int64_t(after.tv_sec-before.tv_sec)*1000000000+after.tv_nsec-before.tv_nsec)/1000));
    check(native_count == 1000000, "one million hot divisor capability lookups retain the native result");
    probe.on_destroy_device(a);
    probe.on_destroy_device(b);
}

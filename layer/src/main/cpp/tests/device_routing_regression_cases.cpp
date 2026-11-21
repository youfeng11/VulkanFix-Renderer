#include "core/layer_manager.h"
#include "core/native_instance_state.h"
#include "core/tbdr_barrier_optimizer.h"
#include "modules/vulkan_1_1/device_group.h"
#include "modules/vulkan_1_1/dispatch_base.h"
#include "modules/vulkan_1_1/vulkan_1_1_core.h"
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
unsigned native_mask_calls = 0;
unsigned khr_mask_calls = 0, core_dispatch_calls = 0, khr_dispatch_calls = 0;
unsigned legacy_dispatch_calls = 0, core_peer_calls = 0, khr_peer_calls = 0;
bool group_args_valid = true;
unsigned native_buffer_bind_calls = 0, native_image_bind_calls = 0;
const void* expected_buffer_group{}; const void* expected_image_group{};
bool native_binding_args = true;
bool native_indirect_args = true;
const auto physical_a = (VkPhysicalDevice)(uintptr_t)0x810001;
const auto physical_b = (VkPhysicalDevice)(uintptr_t)0x810002;
const auto physical_c = (VkPhysicalDevice)(uintptr_t)0x810003;
const auto physical_d = (VkPhysicalDevice)(uintptr_t)0x810004;
const auto device_a = (VkDevice)(uintptr_t)0x820001;
const auto device_b = (VkDevice)(uintptr_t)0x820002;
class RoutingDivisorModule final : public IVulkanLayerModule {
public:
    const char* get_name() const override { return "VK_EXT_vertex_attribute_divisor"; }
    bool is_enabled() const override { return active; }
    bool is_device_native(VkDevice device) override { return device == device_b; }
};
template<class Base> class RoutingModule final : public Base {
    bool is_enabled() const override { return active; }
};
VkResult VKAPI_CALL create(VkPhysicalDevice physical, const VkDeviceCreateInfo*,
                           const VkAllocationCallbacks*, VkDevice* out) {
    *out = physical == physical_a ? device_a : device_b;
    return VK_SUCCESS;
}
void VKAPI_CALL destroy(VkDevice, const VkAllocationCallbacks*) {}
void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* out) {
    *out = {};
    out->apiVersion = physical == physical_a || physical == physical_d ? VK_API_VERSION_1_0 : VK_API_VERSION_1_2;
}
VkResult VKAPI_CALL extensions(VkPhysicalDevice physical, const char*, uint32_t* count,
                               VkExtensionProperties* out) {
    if (physical != physical_c && physical != physical_d) { *count = 0; return VK_SUCCESS; }
    const uint32_t size = physical == physical_d ? 2 : 1;
    if (!out) { *count = size; return VK_SUCCESS; }
    const uint32_t written = std::min(*count,size);
    for (uint32_t i = 0; i < written; ++i) {
        out[i] = {}; out[i].specVersion = 1;
        strcpy(out[i].extensionName,i ? VK_KHR_MAINTENANCE1_EXTENSION_NAME :
            physical == physical_c ? VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME : VK_KHR_DEVICE_GROUP_EXTENSION_NAME);
    }
    *count = written; return written < size ? VK_INCOMPLETE : VK_SUCCESS;
}
VkResult VKAPI_CALL allocate(VkDevice device, const VkCommandBufferAllocateInfo*, VkCommandBuffer* out) {
    *out = (VkCommandBuffer)(uintptr_t(device) + 0x10000);
    return VK_SUCCESS;
}
void VKAPI_CALL queue(VkDevice device, uint32_t, uint32_t, VkQueue* out) {
    *out = (VkQueue)(uintptr_t(device) + 0x20000);
}
void VKAPI_CALL families(VkPhysicalDevice,uint32_t* count,VkQueueFamilyProperties* out) {
    if (!out) { *count = 1; return; }
    if (*count) {
        *out = {}; out->queueCount = 1; out->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        *count = 1;
    }
}
void VKAPI_CALL draw_indirect(VkCommandBuffer, VkBuffer buffer, VkDeviceSize offset,
                             uint32_t count, uint32_t stride) {
    ++native_indirect_calls;
    native_indirect_args &= buffer == (VkBuffer)uint64_t(0x123) && offset == 12 && count == 3 && stride == 20;
}
void VKAPI_CALL set_mask(VkCommandBuffer, uint32_t mask) { if (mask == 1) ++native_mask_calls; }
void VKAPI_CALL set_mask_khr(VkCommandBuffer, uint32_t mask) { if (mask == 1) ++khr_mask_calls; }
void group_arguments(uint32_t x,uint32_t y,uint32_t z,uint32_t nx,uint32_t ny,uint32_t nz) {
    group_args_valid &= x == 1 && y == 2 && z == 3 && nx == 4 && ny == 5 && nz == 6;
}
void VKAPI_CALL dispatch_core(VkCommandBuffer,uint32_t x,uint32_t y,uint32_t z,uint32_t nx,uint32_t ny,uint32_t nz) {
    ++core_dispatch_calls; group_arguments(x,y,z,nx,ny,nz);
}
void VKAPI_CALL dispatch_khr(VkCommandBuffer,uint32_t x,uint32_t y,uint32_t z,uint32_t nx,uint32_t ny,uint32_t nz) {
    ++khr_dispatch_calls; group_arguments(x,y,z,nx,ny,nz);
}
void VKAPI_CALL dispatch_legacy(VkCommandBuffer,uint32_t x,uint32_t y,uint32_t z) {
    ++legacy_dispatch_calls; group_args_valid &= x == 4 && y == 5 && z == 6;
}
void VKAPI_CALL peer_core(VkDevice,uint32_t heap,uint32_t local,uint32_t remote,VkPeerMemoryFeatureFlags* out) {
    ++core_peer_calls; group_args_valid &= heap == 2 && local == 0 && remote == 1;
    *out = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT;
}
void VKAPI_CALL peer_khr(VkDevice,uint32_t heap,uint32_t local,uint32_t remote,VkPeerMemoryFeatureFlags* out) {
    ++khr_peer_calls; group_args_valid &= heap == 2 && local == 0 && remote == 1;
    *out = VK_PEER_MEMORY_FEATURE_COPY_DST_BIT;
}
VkResult VKAPI_CALL bind_buffer2(VkDevice,uint32_t count,const VkBindBufferMemoryInfo* infos) {
    ++native_buffer_bind_calls;
    native_binding_args &= count == 1 && infos[0].pNext == expected_buffer_group && infos[0].memoryOffset == 64;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL bind_image2(VkDevice,uint32_t count,const VkBindImageMemoryInfo* infos) {
    ++native_image_bind_calls;
    native_binding_args &= count == 1 && infos[0].pNext == expected_image_group && infos[0].memoryOffset == 128;
    return VK_SUCCESS;
}
}

void* device_routing_mock_proc(const char* name) {
    if (!active) return nullptr;
    if (!strcmp(name, "vkCreateDevice")) return (void*)create;
    if (!strcmp(name, "vkDestroyDevice")) return (void*)destroy;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (void*)properties;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (void*)extensions;
    if (!strcmp(name, "vkAllocateCommandBuffers")) return (void*)allocate;
    if (!strcmp(name, "vkGetDeviceQueue")) return (void*)queue;
    if (!strcmp(name,"vkGetPhysicalDeviceQueueFamilyProperties")) return (void*)families;
    if (!strcmp(name, "vkCmdDrawIndirect")) return (void*)draw_indirect;
    if (!strcmp(name, "vkCmdSetDeviceMask")) return (void*)set_mask;
    if (!strcmp(name, "vkCmdSetDeviceMaskKHR")) return (void*)set_mask_khr;
    if (!strcmp(name, "vkCmdDispatchBase")) return (void*)dispatch_core;
    if (!strcmp(name, "vkCmdDispatchBaseKHR")) return (void*)dispatch_khr;
    if (!strcmp(name, "vkCmdDispatch")) return (void*)dispatch_legacy;
    if (!strcmp(name, "vkGetDeviceGroupPeerMemoryFeatures")) return (void*)peer_core;
    if (!strcmp(name, "vkGetDeviceGroupPeerMemoryFeaturesKHR")) return (void*)peer_khr;
    if (!strcmp(name,"vkBindBufferMemory2")) return (void*)bind_buffer2;
    if (!strcmp(name,"vkBindImageMemory2")) return (void*)bind_image2;
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
    manager.dispatch_cmd_set_device_mask(command,1);
    check(!native_mask_calls && !khr_mask_calls && manager.should_intercept_device_proc(a,"vkCmdSetDeviceMaskKHR") &&
        manager.should_intercept_device_proc(a,"vkCmdPipelineBarrier"),
        "legacy route retains singleton mask/barrier interception despite available native aliases");
    check(!(manager.get_device_spirv_extensions(a) & vkfix::NativeMaintenance1) &&
              manager.should_intercept_device_proc(a,"vkEndCommandBuffer") && manager.should_intercept_device_proc(a,"vkResetCommandPool"),
          "legacy image-copy staging retains command errors and pool reset interception even without drawing emulation");
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
    manager.dispatch_cmd_set_device_mask(command_b,1);
    check(native_mask_calls == 1 && !khr_mask_calls,"enabled native Core device-group route forwards its mask through the core command exactly once");
    manager.dispatch_cmd_dispatch_base(command_b,1,2,3,4,5,6);
    VkPeerMemoryFeatureFlags peer{};
    manager.dispatch_get_device_group_peer_memory_features(b,2,0,1,&peer);
    check(core_dispatch_calls == 1 && !khr_dispatch_calls && core_peer_calls == 1 && !khr_peer_calls &&
              peer == VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT && group_args_valid,
          "native Core group command dispatch preserves arguments and never selects available KHR aliases");
    const uint32_t local_device = 0;
    VkBindBufferMemoryDeviceGroupInfo buffer_group{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO}; buffer_group.deviceIndexCount = 1; buffer_group.pDeviceIndices = &local_device;
    VkBindImageMemoryDeviceGroupInfo image_group{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO}; image_group.deviceIndexCount = 1; image_group.pDeviceIndices = &local_device;
    expected_buffer_group = &buffer_group; expected_image_group = &image_group;
    VkBindBufferMemoryInfo buffer_binding{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO}; buffer_binding.pNext = &buffer_group; buffer_binding.memoryOffset = 64;
    VkBindImageMemoryInfo image_binding{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO}; image_binding.pNext = &image_group; image_binding.memoryOffset = 128;
    check(manager.dispatch_bind_buffer_memory2(b,1,&buffer_binding) == VK_SUCCESS && manager.dispatch_bind_image_memory2(b,1,&image_binding) == VK_SUCCESS &&
        native_buffer_bind_calls == 1 && native_image_bind_calls == 1 && native_binding_args,
        "enabled native device-group routes preserve original buffer/image binding chains and offsets");
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
    const auto legacy_instance = (VkInstance)(uintptr_t)0x830001;
    auto dispatch_module = std::make_unique<RoutingModule<DispatchBaseModule>>();
    auto* dispatch_state = dispatch_module.get();
    manager.register_module(std::move(dispatch_module));
    manager.register_module(std::make_unique<RoutingModule<DeviceGroupModule>>());
    const VkPhysicalDevice modern_devices[] = {physical_b, physical_c};
    vkfix::remember_native_instance(legacy_instance, VK_API_VERSION_1_0, VK_API_VERSION_1_0,
                                   {}, {}, VK_API_VERSION_1_1);
    vkfix::remember_physical_devices(legacy_instance, 2, modern_devices);
    check(manager.dispatch_create_device(physical_b, &info, nullptr, &b) == VK_SUCCESS &&
              manager.get_device_max_spirv_version(b) == ShaderRewriter::api_version_to_spirv(VK_API_VERSION_1_0) &&
              manager.get_device_spirv_extensions(b) == 0,
          "modern physical API cannot enable SPIR-V or promoted shader capabilities on a native 1.0 target");
    VkComputePipelineCreateInfo compute_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    compute_info.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    dispatch_state->on_modify_compute_pipeline_create_info(b,compute_info);
    manager.dispatch_allocate_command_buffers(b,&allocation,&command_b);
    manager.dispatch_cmd_dispatch_base(command_b,0,0,0,4,5,6);
    manager.dispatch_cmd_set_device_mask(command_b,1);
    const auto& legacy_table = manager.get_dispatch_table(b);
    check(!compute_info.flags && !legacy_table.CmdDispatchBase && !legacy_table.CmdSetDeviceMask &&
              !legacy_table.GetDeviceGroupPeerMemoryFeatures && legacy_dispatch_calls == 1 &&
              core_dispatch_calls == 1 && !khr_dispatch_calls && native_mask_calls == 1 && !khr_mask_calls,
          "legacy device ignores both nonnull group spellings, retains dispatch-base emulation, and lowers zero-base dispatch");
    const bool optimizer_enabled = TBDRBarrierOptimizer::get().is_enabled();
    TBDRBarrierOptimizer::get().set_enabled(false);
    check(manager.should_intercept_device_proc(b,"vkCmdBindPipeline"),
          "dispatch-base emulation retains pipeline binding without other rendering interception");
    Vulkan11CoreModule core_module;
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 properties_root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&subgroup};
    core_module.on_post_get_properties2(physical_b,&properties_root,nullptr);
    check(subgroup.subgroupSize == 1 && subgroup.supportedOperations == VK_SUBGROUP_FEATURE_BASIC_BIT &&
              (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && !subgroup.quadOperationsInAllStages &&
              properties_root.pNext == &subgroup,
          "legacy application target retains software subgroup properties despite a newer physical GPU version");
    TBDRBarrierOptimizer::get().set_enabled(optimizer_enabled);
    manager.dispatch_destroy_device(b, nullptr);
    check(manager.dispatch_create_device(physical_c, &info, nullptr, &b) == VK_SUCCESS &&
              manager.get_device_max_spirv_version(b) == ShaderRewriter::api_version_to_spirv(VK_API_VERSION_1_0) &&
              manager.get_device_spirv_extensions(b) == vkfix::NativeStorageBufferClass,
          "native 1.0 shader routes use only extensions actually enabled by the promotion dependency plan");
    manager.dispatch_destroy_device(b, nullptr);
    vkfix::forget_native_instance(legacy_instance);
    const auto khr_instance = (VkInstance)(uintptr_t)0x830002;
    vkfix::remember_native_instance(khr_instance,VK_API_VERSION_1_1,VK_API_VERSION_1_1,{});
    vkfix::remember_physical_devices(khr_instance,1,&physical_d);
    check(manager.dispatch_create_device(physical_d,&info,nullptr,&b) == VK_SUCCESS,
          "native 1.0 physical device enables KHR device-group commands through the actual dependency plan");
    manager.dispatch_allocate_command_buffers(b,&allocation,&command_b);
    manager.dispatch_cmd_set_device_mask(command_b,1);
    manager.dispatch_cmd_dispatch_base(command_b,1,2,3,4,5,6);
    peer = 0;
    manager.dispatch_get_device_group_peer_memory_features(b,2,0,1,&peer);
    compute_info.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    dispatch_state->on_modify_compute_pipeline_create_info(b,compute_info);
    check(khr_mask_calls == 1 && khr_dispatch_calls == 1 && khr_peer_calls == 1 &&
              native_mask_calls == 1 && core_dispatch_calls == 1 && core_peer_calls == 1 &&
              peer == VK_PEER_MEMORY_FEATURE_COPY_DST_BIT && compute_info.flags == VK_PIPELINE_CREATE_DISPATCH_BASE_BIT && group_args_valid,
          "KHR group route uses only KHR commands despite available core stubs and disables dispatch-base rewriting");
    TBDRBarrierOptimizer::get().set_enabled(false);
    check(dispatch_state->is_device_native(b) && !manager.should_intercept_device_proc(b,"vkCmdBindPipeline"),
          "native dispatch-base route keeps its independent native pipeline binding shortcut");
    const auto* force_env = getenv("VULKAN_FIX_EMULATE_DISPATCH_BASE");
    const bool had_force = force_env != nullptr;
    const std::string saved_force = force_env ? force_env : "";
    setenv("VULKAN_FIX_EMULATE_DISPATCH_BASE","1",1);
    dispatch_state->set_native_route(b,true);
    compute_info.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    dispatch_state->on_modify_compute_pipeline_create_info(b,compute_info);
    check(!dispatch_state->is_device_native(b) && !compute_info.flags &&
              manager.should_intercept_device_proc(b,"vkCmdBindPipeline"),
          "forced dispatch-base emulation retains bindings even when a native KHR command is available");
    if (had_force) setenv("VULKAN_FIX_EMULATE_DISPATCH_BASE",saved_force.c_str(),1);
    else unsetenv("VULKAN_FIX_EMULATE_DISPATCH_BASE");
    dispatch_state->set_native_route(b,true);
    TBDRBarrierOptimizer::get().set_enabled(optimizer_enabled);
    bool group_shortcuts = false;
    for (auto* name : {"vkCmdSetDeviceMask","vkCmdSetDeviceMaskKHR","vkCmdDispatchBase","vkCmdDispatchBaseKHR",
                       "vkGetDeviceGroupPeerMemoryFeatures","vkGetDeviceGroupPeerMemoryFeaturesKHR"})
        group_shortcuts |= !manager.should_intercept_device_proc(b,name);
    check(!group_shortcuts,"both public group-command spellings retain the authorized route instead of raw loader shortcuts");
    manager.dispatch_destroy_device(b,nullptr);
    vkfix::forget_native_instance(khr_instance);
    active = false;
}

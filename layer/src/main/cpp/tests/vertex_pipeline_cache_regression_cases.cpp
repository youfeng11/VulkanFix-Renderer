#include "core/layer_manager.h"
#include "core/pipeline_cache_manager.h"
#include "core/vertex_store_emulator.h"
#include "vertex_store_fixture.h"
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <thread>

class PipelineCacheManagerTest {
public:
    static std::shared_ptr<DevicePipelineCacheState> install(VkDevice device, VkPipelineCache cache) {
        auto& manager = PipelineCacheManager::get();
        auto state = std::make_shared<DevicePipelineCacheState>();
        state->device = device;
        state->disk_cache = cache;
        state->is_valid = true;
        std::unique_lock<std::shared_mutex> lock(manager.m_mutex);
        manager.m_devices[(uint64_t)(uintptr_t)device] = state;
        manager.m_enabled.store(true);
        return state;
    }
    static void remove(VkDevice device) {
        auto& manager = PipelineCacheManager::get();
        std::unique_lock<std::shared_mutex> lock(manager.m_mutex);
        manager.m_devices.erase((uint64_t)(uintptr_t)device);
        manager.m_enabled.store(false);
    }
};

namespace {
bool active = false;
uint64_t serial = 0x610000;
VkPipelineCache compute_cache{}, graphics_cache{};
unsigned compute_calls = 0, graphics_calls = 0;
bool compute_locked = false, graphics_locked = false;
void check_lock(VkDevice device, bool& held) {
    auto state = PipelineCacheManager::get().get_state(device);
    held = false;
    std::thread probe([&] {
        held = !state->cache_use_mutex.try_lock();
        if (!held) state->cache_use_mutex.unlock();
    });
    probe.join();
}
void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    *out = {};
    out->apiVersion = VK_API_VERSION_1_0;
    out->limits.maxBoundDescriptorSets = 8;
    out->limits.maxPerStageDescriptorStorageBuffers = 16;
}
VkResult VKAPI_CALL shader(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*,
                           VkShaderModule* out) {
    *out = (VkShaderModule)++serial;
    return VK_SUCCESS;
}
void VKAPI_CALL destroy_shader(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}
void VKAPI_CALL destroy_pipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}
VkResult VKAPI_CALL compute(VkDevice device, VkPipelineCache cache, uint32_t count,
                            const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* out) {
    ++compute_calls;
    compute_cache = cache;
    check_lock(device, compute_locked);
    for (uint32_t i = 0; i < count; ++i) out[i] = (VkPipeline)++serial;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL graphics(VkDevice device, VkPipelineCache cache, uint32_t count,
                             const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* out) {
    ++graphics_calls;
    graphics_cache = cache;
    check_lock(device, graphics_locked);
    for (uint32_t i = 0; i < count; ++i) out[i] = (VkPipeline)++serial;
    return VK_SUCCESS;
}
}

void* vertex_pipeline_cache_mock_proc(const char* name) {
    if (!active) return nullptr;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (void*)properties;
    if (!strcmp(name, "vkCreateShaderModule")) return (void*)shader;
    if (!strcmp(name, "vkDestroyShaderModule")) return (void*)destroy_shader;
    if (!strcmp(name, "vkDestroyPipeline")) return (void*)destroy_pipeline;
    if (!strcmp(name, "vkCreateComputePipelines")) return (void*)compute;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (void*)graphics;
    return nullptr;
}

void run_vertex_pipeline_cache_regressions(void (*check)(bool, const char*)) {
    active = true;
    auto& manager = LayerManager::get();
    auto& emulator = VertexStoreEmulator::get();
    auto device = (VkDevice)(uintptr_t)0x615000;
    auto physical = (VkPhysicalDevice)(uintptr_t)0x616000;
    auto disk = (VkPipelineCache)uint64_t(0x123400006170);
    auto app = (VkPipelineCache)uint64_t(0x123400006180);
    manager.init_device_dispatch_table(device);
    emulator.enable(device, physical);
    auto state = PipelineCacheManagerTest::install(device, disk);
    VkDescriptorSetLayout set = (VkDescriptorSetLayout)uint64_t(0x619000);
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    emulator.set_layout(device, set, &set_info);
    VkPipelineLayout layout = (VkPipelineLayout)uint64_t(0x61a000);
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set;
    emulator.layout(device, layout, &layout_info);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_0);
    std::vector<uint32_t> words;
    const bool assembled = tools.Assemble(vertex_store_source, &words);
    check(assembled, "assemble vertex-store shader for pipeline-cache integration");
    VkShaderModule module = (VkShaderModule)uint64_t(0x61b000);
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize = words.size() * sizeof(uint32_t);
    shader_info.pCode = words.data();
    emulator.shader(device, module, &shader_info);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = module;
    stage.pName = "main";
    VkVertexInputBindingDescription binding{0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                     {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8}};
    VkPipelineVertexInputStateCreateInfo inputs{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    inputs.vertexBindingDescriptionCount = 1;
    inputs.pVertexBindingDescriptions = &binding;
    inputs.vertexAttributeDescriptionCount = 2;
    inputs.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 1;
    info.pStages = &stage;
    info.pVertexInputState = &inputs;
    info.pInputAssemblyState = &assembly;
    info.layout = layout;
    VkPipeline pipeline{};
    check(manager.dispatch_create_graphics_pipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS &&
              compute_calls == 1 && graphics_calls == 1,
          "vertex emulation compiles compute and replay through real manager dispatch");
    check(compute_cache == disk && graphics_cache == disk && compute_locked && graphics_locked,
          "both emulation pipelines use the persistent cache under compilation guards");
    check(state->pending_pipelines_count.load() == 2,
          "both emulation compilations notify cache persistence exactly once");
    manager.dispatch_destroy_pipeline(device, pipeline, nullptr);
    check(manager.dispatch_create_graphics_pipelines(device, app, 1, &info, nullptr, &pipeline) == VK_SUCCESS &&
              compute_cache == app && graphics_cache == app && !compute_locked && !graphics_locked &&
              state->pending_pipelines_count.load() == 2,
          "emulation preserves application cache ownership without duplicate persistent-cache notifications");
    manager.dispatch_destroy_pipeline(device, pipeline, nullptr);
    emulator.destroy_device(device);
    manager.remove_device_dispatch_table(device);
    PipelineCacheManagerTest::remove(device);
    active = false;
}

#include "core/layer_manager.h"
#include "core/shader_module_cache.h"
#include "core/device_object_key.h"
#include "modules/vulkan_1_1/dispatch_base.h"
#include "modules/vulkan_1_1/negative_viewport.h"
#include "diagnostics/driver_post_shaders.h"
#include <map>
#include <set>

namespace {
bool active = false;
using Key = vkfix::DeviceObjectKey;
struct Less {
    bool operator()(const Key& a, const Key& b) const {
        return a.device != b.device ? std::less<VkDevice>{}(a.device, b.device) : a.handle < b.handle;
    }
};
const auto physical = (VkPhysicalDevice)(uintptr_t)0xe1000;
const auto a = (VkDevice)(uintptr_t)0xe2000, b = (VkDevice)(uintptr_t)0xe2001;
unsigned next_device = 0, dispatches = 0, draws = 0, wrong_parent = 0;
bool flags_valid = true;
VkResult variant_error = VK_SUCCESS;
std::map<VkDevice, unsigned> layouts, compute_variants, layout_frees, viewport_frees;
std::map<VkCommandBuffer, VkPipeline> bound;
std::set<Key, Less> live;
template<class Base> struct Module : Base { bool is_enabled() const override { return active; } };
VkResult VKAPI_CALL create(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice* out) {
    *out = next_device++ ? b : a; return VK_SUCCESS;
}
void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) { *out = {}; out->apiVersion = VK_API_VERSION_1_1; }
void VKAPI_CALL destroy_device(VkDevice, const VkAllocationCallbacks*) {}
VkResult VKAPI_CALL allocate(VkDevice device, const VkCommandBufferAllocateInfo*, VkCommandBuffer* out) {
    *out = (VkCommandBuffer)(uintptr_t(device) + 0x10000); return VK_SUCCESS;
}
VkResult VKAPI_CALL layout(VkDevice device, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* out) {
    *out = (VkPipelineLayout)uint64_t(0x700 + layouts[device]++); return VK_SUCCESS;
}
void VKAPI_CALL destroy_layout(VkDevice device, VkPipelineLayout handle, const VkAllocationCallbacks*) {
    if (handle == (VkPipelineLayout)uint64_t(0x701)) ++layout_frees[device];
}
VkResult VKAPI_CALL shader(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* out) {
    *out = (VkShaderModule)uint64_t(0x901); return VK_SUCCESS;
}
void VKAPI_CALL destroy_shader(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}
VkResult VKAPI_CALL compute(VkDevice device, VkPipelineCache, uint32_t count,
    const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks*, VkPipeline* out) {
    for (uint32_t i = 0; i < count; ++i) {
        flags_valid &= !(infos[i].flags & VK_PIPELINE_CREATE_DISPATCH_BASE_BIT);
        if (infos[i].layout == (VkPipelineLayout)uint64_t(0x701)) {
            if (variant_error != VK_SUCCESS) return variant_error;
            out[i] = (VkPipeline)uint64_t(0x501 + compute_variants[device]++);
            live.insert(Key::make(device, out[i]));
        } else out[i] = (VkPipeline)uint64_t(0x500);
    }
    return VK_SUCCESS;
}
VkResult VKAPI_CALL graphics(VkDevice device, VkPipelineCache, uint32_t count,
    const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* out) {
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = (VkPipeline)uint64_t(0x801); live.insert(Key::make(device, out[i]));
    }
    return VK_SUCCESS;
}
void VKAPI_CALL destroy_pipeline(VkDevice device, VkPipeline handle, const VkAllocationCallbacks*) {
    if (handle == (VkPipeline)uint64_t(0x500) || handle == (VkPipeline)uint64_t(0x800)) return;
    wrong_parent += live.erase(Key::make(device, handle)) != 1;
    if (handle == (VkPipeline)uint64_t(0x801)) ++viewport_frees[device];
}
void VKAPI_CALL bind(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipeline pipeline) { bound[cmd] = pipeline; }
void VKAPI_CALL dispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) { ++dispatches; }
void VKAPI_CALL viewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*) {}
void VKAPI_CALL draw(VkCommandBuffer cmd, uint32_t, uint32_t, uint32_t, uint32_t) {
    if (bound[cmd] == (VkPipeline)uint64_t(0x801)) ++draws;
}
} // namespace
void* core11_pipeline_mock_proc(const char* name) {
    if (!active) return nullptr;
#define PROC(command, function) if (!strcmp(name, #command)) return (void*)function
    PROC(vkCreateDevice, create); PROC(vkDestroyDevice, destroy_device);
    PROC(vkGetPhysicalDeviceProperties, properties); PROC(vkAllocateCommandBuffers, allocate);
    PROC(vkCreatePipelineLayout, layout); PROC(vkDestroyPipelineLayout, destroy_layout);
    PROC(vkCreateShaderModule, shader); PROC(vkDestroyShaderModule, destroy_shader);
    PROC(vkCreateComputePipelines, compute); PROC(vkCreateGraphicsPipelines, graphics);
    PROC(vkDestroyPipeline, destroy_pipeline); PROC(vkCmdBindPipeline, bind);
    PROC(vkCmdDispatch, dispatch); PROC(vkCmdSetViewport, viewport); PROC(vkCmdDraw, draw);
#undef PROC
    return nullptr;
}
void run_core11_pipeline_regressions(void (*check)(bool, const char*)) {
    active = true;
    setenv("VULKAN_FIX_EMULATE_DISPATCH_BASE", "1", 1);
    auto& manager = LayerManager::get();
    auto compute_module = std::make_unique<Module<DispatchBaseModule>>();
    auto* compute_state = compute_module.get();
    manager.register_module(std::move(compute_module));
    auto viewport_module = std::make_unique<Module<NegativeViewportModule>>();
    auto* viewport_state = viewport_module.get();
    manager.register_module(std::move(viewport_module));
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice devices[2]{};
    VkCommandBuffer commands[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        check(manager.dispatch_create_device(physical, &device_info, nullptr, &devices[i]) == VK_SUCCESS,
              "core11 lifetime: create an independently tracked emulated device");
        auto device = devices[i];
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandBufferCount = 1;
        check(manager.dispatch_allocate_command_buffers(device, &allocation, &commands[i]) == VK_SUCCESS,
              "core11 lifetime: establish command ownership through allocation");
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VkPipelineLayout source{};
        check(manager.dispatch_create_pipeline_layout(device, &li, nullptr, &source) == VK_SUCCESS,
              "core11 lifetime: retain a compatible private pipeline layout");
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = sizeof(post_push_comp); si.pCode = post_push_comp;
        auto module = (VkShaderModule)uint64_t(0x600);
        vkfix::ShaderModuleCache::get().remember(device, module, &si);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT; ci.layout = source;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main";
        VkPipeline pipeline{};
        check(manager.dispatch_create_compute_pipelines(device, {}, 1, &ci, nullptr, &pipeline) == VK_SUCCESS &&
                  ci.flags == VK_PIPELINE_CREATE_DISPATCH_BASE_BIT,
              "core11 lifetime: remove unsupported native flags without changing caller input");
        vkfix::ShaderModuleCache::get().forget(device, module);
        // Reusing the same module handle must not change the existing pipeline.
        si.codeSize = sizeof(post_spirv14_comp); si.pCode = post_spirv14_comp;
        vkfix::ShaderModuleCache::get().remember(device, module, &si);
        manager.dispatch_destroy_pipeline_layout(device, source, nullptr);
        check(layout_frees[device] == 0, "core11 lifetime: source-layout destruction retains pipeline-owned layout");
        manager.dispatch_cmd_bind_pipeline(commands[i], VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        manager.dispatch_cmd_dispatch_base(commands[i], 1, 0, 0, 1, 1, 1);
        check(compute_variants[device] == 1 && manager.command_recording_error(commands[i]) == VK_SUCCESS &&
                  bound[commands[i]] == pipeline,
              "core11 lifetime: dispatch survives destroyed/reused shader and destroyed application layout");

        si.codeSize = sizeof(post_draw_vert); si.pCode = post_draw_vert;
        vkfix::ShaderModuleCache::get().remember(device, module, &si);
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT; stage.module = module; stage.pName = "main";
        VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        view.viewportCount = 1;
        VkDynamicState state = VK_DYNAMIC_STATE_VIEWPORT;
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 1; dynamic.pDynamicStates = &state;
        VkGraphicsPipelineCreateInfo graphics_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        graphics_info.stageCount = 1; graphics_info.pStages = &stage;
        graphics_info.pViewportState = &view; graphics_info.pDynamicState = &dynamic;
        std::vector<void*> allocations;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        viewport_state->on_modify_pipeline_create_info(device, 0, graphics_info, vi, allocations);
        const auto graphics_pipeline = (VkPipeline)uint64_t(0x800);
        viewport_state->on_post_create_graphics_pipelines(device, 1, &graphics_info, &graphics_pipeline);
        for (auto allocation : allocations) free(allocation);
    }
    check(flags_valid && dispatches == 2, "core11 lifetime: both devices dispatch with legal native flags");
    manager.dispatch_destroy_device(a, nullptr);
    check(viewport_frees[a] == 1 && viewport_frees[b] == 0 && layout_frees[a] == 1 && layout_frees[b] == 0 && !wrong_parent,
          "core11 lifetime: destroying device A cannot destroy device B's variants or private layout");
    const auto cmd = commands[1];
    const auto original = (VkPipeline)uint64_t(0x500);
    manager.dispatch_cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, original);
    manager.dispatch_cmd_dispatch_base(cmd, 1, 0, 0, 1, 1, 1);
    manager.dispatch_cmd_dispatch_base(cmd, 2, 0, 0, 1, 1, 1);
    check(compute_variants[b] == 2 && dispatches == 4 && manager.command_recording_error(cmd) == VK_SUCCESS,
          "core11 lifetime: surviving device retains cached variants and can create new bases");
    variant_error = VK_ERROR_OUT_OF_HOST_MEMORY;
    manager.dispatch_cmd_dispatch_base(cmd, 3, 0, 0, 1, 1, 1);
    check(manager.command_recording_error(cmd) == VK_ERROR_OUT_OF_HOST_MEMORY && dispatches == 4,
          "core11 lifetime: failed internal creation reports recording error instead of silently losing dispatch");
    variant_error = VK_SUCCESS;
    compute_state->on_reset_command_buffer(cmd, 0);
    manager.dispatch_cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, original);
    manager.dispatch_cmd_dispatch_base(cmd, 3, 0, 0, 1, 1, 1);
    check(manager.command_recording_error(cmd) == VK_SUCCESS && dispatches == 5,
          "core11 lifetime: resetting the command buffer permits retry after allocation failure");
    manager.dispatch_cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)uint64_t(0x800));
    VkViewport negative{0, 16, 16, -16, 0, 1};
    check(viewport_state->on_cmd_set_viewport(cmd, 0, 1, &negative), "core11 viewport: surviving command records negative viewport");
    manager.dispatch_cmd_draw(cmd, 3, 1, 0, 0);
    check(draws == 1, "core11 viewport: device B's dynamic flipped pipeline survives device A destruction");
    manager.dispatch_destroy_pipeline(b, original, nullptr);
    check(layout_frees[b] == 1, "core11 lifetime: the last owning pipeline releases its private layout");
    manager.dispatch_destroy_pipeline(b, (VkPipeline)uint64_t(0x800), nullptr);
    check(viewport_frees[b] == 1, "core11 viewport: application pipeline destruction retires its flipped variant immediately");
    VkShaderModuleCreateInfo many{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    many.codeSize = sizeof(post_push_comp); many.pCode = post_push_comp;
    const auto first_module = (VkShaderModule)uint64_t(0x100000);
    for (uint64_t i = 0; i < 4097; ++i)
        vkfix::ShaderModuleCache::get().remember(b, (VkShaderModule)(0x100000 + i), &many);
    auto snapshot = vkfix::ShaderModuleCache::get().acquire(b, first_module);
    check(snapshot && snapshot->size() == sizeof(post_push_comp) / 4 &&
              snapshot == vkfix::ShaderModuleCache::get().acquire(b, first_module),
          "core11 shader cache: 4097 live modules cannot evict another live module and snapshots share immutable bytes");
    std::weak_ptr<const std::vector<uint32_t>> lifetime = snapshot;
    manager.dispatch_destroy_device(b, nullptr);
    check(snapshot && !vkfix::ShaderModuleCache::get().acquire(b, first_module),
          "core11 shader cache: device retirement clears lookup ownership without invalidating an acquired snapshot");
    snapshot.reset();
    check(lifetime.expired(), "core11 shader cache: the last snapshot releases its bytes after device retirement");
    check(live.empty() && !wrong_parent, "core11 lifetime: all private pipelines have exactly one device-correct destruction");
    unsetenv("VULKAN_FIX_EMULATE_DISPATCH_BASE");
    active = false;
}

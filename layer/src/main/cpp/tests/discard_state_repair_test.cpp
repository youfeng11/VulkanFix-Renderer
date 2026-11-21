#include "core/discard_state_repair.h"
#include <cstdio>
#include <cstring>

namespace {
unsigned failures = 0, viewport_calls = 0, scissor_calls = 0;
VkViewport last_viewport{};
VkRect2D last_scissor{};
uint32_t viewport_count = 0, scissor_count = 0;
void check(bool pass, const char *name) {
    std::printf("%s: %s\n", pass ? "PASS" : "FAIL", name);
    failures += !pass;
}
void VKAPI_CALL set_viewport(VkCommandBuffer, uint32_t first, uint32_t count, const VkViewport *values) {
    viewport_calls++;
    viewport_count = count;
    if (!first && count) last_viewport = values[0];
}
void VKAPI_CALL set_scissor(VkCommandBuffer, uint32_t first, uint32_t count, const VkRect2D *values) {
    scissor_calls++;
    scissor_count = count;
    if (!first && count) last_scissor = values[0];
}
void reset_calls() { viewport_calls = scissor_calls = 0; }
}

int main() {
    using vkfix::DiscardStateRepair;
    auto &repair = DiscardStateRepair::get();
    const auto device = reinterpret_cast<VkDevice>(uintptr_t(1));
    const auto other = reinterpret_cast<VkDevice>(uintptr_t(2));
    const auto command = reinterpret_cast<VkCommandBuffer>(uintptr_t(3));
    const VkPipeline pipelines[]{(VkPipeline)uintptr_t(4), (VkPipeline)uintptr_t(5), (VkPipeline)uintptr_t(6)};
    VkPipelineRasterizationStateCreateInfo raster[3]{};
    VkPipelineViewportStateCreateInfo view[3]{};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo infos[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        infos[i] = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        raster[i] = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster[i].rasterizerDiscardEnable = i == 0;
        view[i] = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        view[i].viewportCount = view[i].scissorCount = 2;
        infos[i].pRasterizationState = &raster[i];
        infos[i].pViewportState = &view[i];
        if (i != 2) infos[i].pDynamicState = &dynamic;
    }
    const VkViewport viewports[]{{1, 1, 14, 12, 0, 1}, {2, 2, 9, 8, 0, 1}};
    const VkRect2D scissors[]{{{2, 3}, {11, 11}}, {{4, 5}, {7, 7}}};
    auto populate = [&] {
        repair.viewport(device, command, 1, 1, viewports + 1);
        repair.viewport(device, command, 0, 1, viewports);
        repair.scissor(device, command, 0, 2, scissors);
    };
    auto bind = [&](unsigned index, VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_GRAPHICS) {
        repair.bound(device, command, point, pipelines[index], set_viewport, set_scissor);
    };
    populate();
    bind(0); bind(1);
    check(!viewport_calls && !scissor_calls, "unmeasured devices preserve native state commands");
    repair.enable(device, 16);
    repair.enable(other, 16);
    const auto *discard_viewport = infos[0].pViewportState;
    infos[0].pViewportState = reinterpret_cast<const VkPipelineViewportStateCreateInfo *>(uintptr_t(1));
    repair.remember_pipelines(device, 3, infos, pipelines);
    infos[0].pViewportState = discard_viewport;
    repair.remember_pipelines(other, 3, infos, pipelines);
    populate(); bind(1);
    check(!viewport_calls && !scissor_calls, "ordinary dynamic binds emit no extra state");
    bind(0); bind(1);
    check(viewport_calls == 1 && scissor_calls == 1 && viewport_count == 2 && scissor_count == 2 &&
          !std::memcmp(&last_viewport, viewports, sizeof last_viewport) &&
          !std::memcmp(&last_scissor, scissors, sizeof last_scissor), "partial updates restore both viewport and scissor arrays");
    reset_calls(); bind(1);
    check(!viewport_calls && !scissor_calls, "a repaired transition does not replay state twice");
    bind(0); bind(1, VK_PIPELINE_BIND_POINT_COMPUTE); bind(1);
    check(viewport_calls == 1 && scissor_calls == 1, "compute binds preserve pending graphics restoration");
    reset_calls(); bind(0); bind(2); bind(1);
    check(!viewport_calls && !scissor_calls, "static pipeline state invalidates dynamic snapshots");
    VkPipelineCreateFlags2CreateInfo library{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
    library.flags = VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR;
    auto incomplete = infos[1];
    incomplete.pNext = &library;
    incomplete.pRasterizationState = reinterpret_cast<const VkPipelineRasterizationStateCreateInfo *>(uintptr_t(1));
    repair.remember_pipelines(device, 1, &incomplete, pipelines + 1);
    populate(); bind(0); bind(1);
    check(!viewport_calls && !scissor_calls, "library flags2 excludes ignored pipeline state pointers");
    repair.remember_pipelines(device, 3, infos, pipelines);
    populate(); bind(0); repair.invalidate_viewport(command); bind(1);
    check(!viewport_calls && scissor_calls == 1, "counted viewport commands cannot resurrect stale viewport values");
    reset_calls(); populate(); bind(0); repair.invalidate_scissor(command); bind(1);
    check(viewport_calls == 1 && !scissor_calls, "counted scissor commands cannot resurrect stale scissor values");
    reset_calls(); populate(); bind(0); repair.forget_command(command); bind(1);
    check(!viewport_calls && !scissor_calls, "command reset and secondary execution clear restoration");
    populate(); bind(0);
    repair.forget_pipeline(device, pipelines[1]); bind(1);
    check(!viewport_calls && !scissor_calls, "unknown or destroyed pipelines invalidate restoration");
    repair.remember_pipelines(device, 3, infos, pipelines);
    populate(); bind(0);
    repair.forget_device(device); bind(1);
    check(!viewport_calls && !scissor_calls && !repair.enabled(device) && repair.enabled(other),
          "device cleanup preserves another device with coincident handles");
    repair.forget_device(other);
    std::printf("Discard state repair failures: %u\n", failures);
    return failures ? 1 : 0;
}

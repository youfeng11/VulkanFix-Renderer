#pragma once
#include "../core/driver_trust_shaders.h"
#include "../core/native_probe_resources.h"
#include <array>

namespace vkfix::nativeprobe {
enum class DiscardStateMode { Inherited, Reissued, Rasterized };
struct DiscardStateReadback {
    std::string detail;
    bool passed;
    bool lost_state;
};

template<class Context, class Ready>
DiscardStateReadback probe_discard_state(Context &p, VkPhysicalDevice physical, uint32_t family,
                                        DiscardStateMode mode, Ready ready) {
    Resources<Context> gpu(p, physical, family);
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    const VkSubpassDependency outgoing{0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = pass_info.subpassCount = pass_info.dependencyCount = 1;
    pass_info.pAttachments = &color;
    pass_info.pSubpasses = &subpass;
    pass_info.pDependencies = &outgoing;
    VkRenderPass pass{};
    auto destroy_pass = p.template dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    check(p.template dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(p.device, &pass_info, nullptr, &pass),
          "discard-state render pass");
    gpu.objects.push_back([=, &p] { destroy_pass(p.device, pass, nullptr); });

    const auto target = gpu.image();
    const auto first_target = gpu.image();
    const auto framebuffer = gpu.framebuffer(pass, target.view);
    const auto first_framebuffer = gpu.framebuffer(pass, first_target.view);
    const auto output = gpu.buffer(16 * 16 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const VkPushConstantRange color_range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &color_range;
    VkPipelineLayout layout{};
    auto destroy_layout = p.template dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    check(p.template dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device, &layout_info, nullptr, &layout),
          "discard-state pipeline layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, layout, nullptr); });
    const VkPipelineShaderStageCreateInfo stages[]{
        gpu.shader_stage(VK_SHADER_STAGE_VERTEX_BIT, tiler_vert, sizeof tiler_vert),
        gpu.shader_stage(VK_SHADER_STAGE_FRAGMENT_BIT, tiler_frag, sizeof tiler_frag)
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState write{};
    write.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &write;
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = pass;
    auto make_pipeline = [&](bool discard) {
        raster.rasterizerDiscardEnable = discard;
        info.stageCount = discard ? 1 : 2;
        auto destroy = p.template dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        VkPipeline pipeline{};
        auto result = p.template dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(
            p.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (pipeline) gpu.objects.push_back([=, &p] { destroy(p.device, pipeline, nullptr); });
        check(result, "discard-state graphics pipeline");
        return pipeline;
    };
    const auto visible = make_pipeline(false);
    const bool discard = mode != DiscardStateMode::Rasterized;
    const auto first = discard ? make_pipeline(true) : visible;
    auto begin = p.template dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    auto end = p.template dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    auto bind = p.template dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
    auto viewport = p.template dev<PFN_vkCmdSetViewport>("vkCmdSetViewport");
    auto scissor = p.template dev<PFN_vkCmdSetScissor>("vkCmdSetScissor");
    auto draw = p.template dev<PFN_vkCmdDraw>("vkCmdDraw");
    auto push = p.template dev<PFN_vkCmdPushConstants>("vkCmdPushConstants");
    auto copy = p.template dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    // Each setting clips a different edge of the expected rectangle.
    const VkViewport viewport_value{1, 1, 14, 12, 0, 1};
    const VkRect2D scissor_value{{2, 3}, {11, 11}};
    const VkClearValue black{{{0.f, 0.f, 0.f, 1.f}}};
    const float green[]{0.f, 1.f, 0.f, 1.f};

    ready();
    unsigned failed = 0, blank = 0;
    std::array<unsigned, 2> matched{};
    for (unsigned trial = 0; trial < matched.size(); ++trial) {
        const auto cb = gpu.begin();
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = pass;
        bi.framebuffer = trial ? first_framebuffer : framebuffer;
        bi.renderArea.extent = {16, 16};
        bi.clearValueCount = 1;
        bi.pClearValues = &black;
        begin(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, first);
        viewport(cb, 0, 1, &viewport_value);
        scissor(cb, 0, 1, &scissor_value);
        if (!discard) push(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof black.color.float32, black.color.float32);
        draw(cb, 3, 1, 0, 0);
        if (trial) {
            end(cb);
            bi.framebuffer = framebuffer;
            begin(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        }
        bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, visible);
        if (mode == DiscardStateMode::Reissued) {
            viewport(cb, 0, 1, &viewport_value);
            scissor(cb, 0, 1, &scissor_value);
        }
        push(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof green, green);
        draw(cb, 3, 1, 0, 0);
        end(cb);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {16, 16, 1};
        copy(cb, target.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.handle, 1, &region);
        gpu.host_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        gpu.submit(cb);
        gpu.invalidate(output);
        constexpr std::array<uint8_t, 4> clear_pixel{0, 0, 0, 255};
        constexpr std::array<uint8_t, 4> draw_pixel{0, 255, 0, 255};
        const auto *pixels = static_cast<const uint8_t *>(output.mapped);
        bool all_clear = true;
        for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 0; x < 16; ++x) {
            const auto *pixel = pixels + (y * 16 + x) * 4;
            const auto &expected = (x >= 2 && x < 13 && y >= 3 && y < 13) ? draw_pixel : clear_pixel;
            matched[trial] += std::memcmp(pixel, expected.data(), expected.size()) == 0;
            all_clear &= std::memcmp(pixel, clear_pixel.data(), clear_pixel.size()) == 0;
        }
        failed += matched[trial] != 256;
        blank += all_clear;
    }
    auto detail = "viewport/scissor readback matched " + std::to_string(matched[0]) +
                  "/256 pixels in one pass, " + std::to_string(matched[1]) + "/256 across passes";
    return {std::move(detail), failed == 0, mode == DiscardStateMode::Inherited && failed > 0 && failed == blank};
}
} // namespace vkfix::nativeprobe

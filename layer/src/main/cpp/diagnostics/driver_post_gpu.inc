// Included inside the POST implementation's private namespace.
// Resource lifetime is scoped to a disposable probe process and bounded fence.
#include "driver_post_shaders.h"
#include "variable_pointers_shader.h"
#include "variable_pointers_storage_shader.h"
using GpuProbe = vkfix::nativeprobe::Resources<Probe>;

void synchronization_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    GpuProbe gpu(p, physical, family);
    auto source = gpu.buffer(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto target = gpu.buffer(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto cb = gpu.begin();
    p.dev<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(cb, source.handle, 0, 256, 0x24681357);
    auto barrier = p.dev<PFN_vkCmdPipelineBarrier2KHR>("vkCmdPipelineBarrier2KHR");
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; memory.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &memory;
    barrier(cb, &dep);
    VkBufferCopy copy{0, 0, 256};
    p.dev<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(cb, source.handle, target.handle, 1, &copy);
    memory.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; memory.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    barrier(cb, &dep);
    gpu.submit(cb, true);
    gpu.invalidate(target);
    for (unsigned i = 0; i < 64; ++i)
        if (static_cast<uint32_t *>(target.mapped)[i] != 0x24681357)
            throw ProbeMismatch("synchronization2 transfer readback mismatch");
    row("synchronization2.transfer", "PASS", "fill/barrier2/copy/barrier2/submit2: 64 words matched", "functional");
}

void query_reset_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family, bool core, bool timestamps) {
    if (!timestamps) {
        row("hostQueryReset.reset", "SKIP", "selected queue has no timestamp support", "functional");
        return;
    }
    GpuProbe gpu(p,physical,family);
    auto reset=p.dev<PFN_vkResetQueryPool>(core ? "vkResetQueryPool" : "vkResetQueryPoolEXT");
    auto destroy=p.dev<PFN_vkDestroyQueryPool>("vkDestroyQueryPool");
    auto results=p.dev<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults");
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; ci.queryType=VK_QUERY_TYPE_TIMESTAMP; ci.queryCount=2;
    VkQueryPool pool{};
    require(p.dev<PFN_vkCreateQueryPool>("vkCreateQueryPool")(p.device,&ci,nullptr,&pool), "create timestamp pool");
    gpu.objects.push_back([=,&p]{ destroy(p.device,pool,nullptr); });
    auto cb=gpu.begin();
    p.dev<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(cb,pool,0,2);
    auto write=p.dev<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp");
    write(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool,0);
    write(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool,1);
    gpu.submit(cb);
    uint64_t data[4]{0,UINT64_MAX,0,UINT64_MAX};
    auto flags=VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    require(results(p.device,pool,0,2,sizeof data,data,2*sizeof(uint64_t),flags), "read written timestamp queries");
    if (!data[1] || !data[3]) throw ProbeMismatch("written queries did not become available");
    reset(p.device,pool,0,2);
    data[1]=data[3]=UINT64_MAX;
    const auto result=results(p.device,pool,0,2,sizeof data,data,2*sizeof(uint64_t),flags);
    if (result!=VK_NOT_READY || data[1] || data[3])
        throw ProbeMismatch("reset queries did not return unavailable/NOT_READY");
    row("hostQueryReset.reset", "PASS", "GPU-written queries were available, then host reset returned NOT_READY with zero availability", "functional");
}

void dynamic_rendering_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    GpuProbe gpu(p, physical, family);
    auto image = gpu.image();
    auto output = gpu.buffer(16 * 16 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto cb = gpu.begin();
    auto barrier = p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = image.handle; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ib.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            0, nullptr, 0, nullptr, 1, &ib);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = image.view; attachment.imageLayout = ib.newLayout;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = {{1.f, 0.f, 1.f, 1.f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {16,16}; rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &attachment;
    p.dev<PFN_vkCmdBeginRenderingKHR>("vkCmdBeginRenderingKHR")(cb, &rendering);
    p.dev<PFN_vkCmdEndRenderingKHR>("vkCmdEndRenderingKHR")(cb);
    ib.oldLayout = ib.newLayout; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &ib);
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {16,16,1};
    p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cb, image.handle, ib.newLayout, output.handle, 1, &copy);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(output);
    const auto pixels = static_cast<unsigned char *>(output.mapped);
    for (unsigned i = 0; i < 256; ++i)
        if (pixels[i*4] != 255 || pixels[i*4+1] != 0 || pixels[i*4+2] != 255 || pixels[i*4+3] != 255)
            throw ProbeMismatch("dynamic rendering clear/readback mismatch");
    row("dynamicRendering.clear", "PASS", "16x16 dynamic rendering clear/store and 256 RGBA pixel readbacks matched", "functional");
}

void negative_viewport_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family,
                             bool dynamic, bool indirect, bool indexed, bool positive,
                             bool mixed, bool constant_viewport, const char *row_name) {
    GpuProbe gpu(p, physical, family);
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
    VkRenderPassCreateInfo render_pass_ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass_ci.attachmentCount = 1;
    render_pass_ci.pAttachments = &attachment;
    render_pass_ci.subpassCount = 1;
    render_pass_ci.pSubpasses = &subpass;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    auto destroy_render_pass = p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    require(p.dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(
                p.device, &render_pass_ci, nullptr, &render_pass), "create negative-viewport render pass");

    VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push_range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    VkResult result = p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
        p.device, &layout_ci, nullptr, &layout);
    if (result != VK_SUCCESS) {
        destroy_render_pass(p.device, render_pass, nullptr);
        throw ProbeMismatch("could not create negative-viewport pipeline layout");
    }

    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = constant_viewport ? sizeof post_viewport_index_vert
                                           : sizeof post_draw_vert;
    shader_ci.pCode = constant_viewport ? post_viewport_index_vert : post_draw_vert;
    result = p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
        p.device, &shader_ci, nullptr, &vertex);
    if (result == VK_SUCCESS) {
        shader_ci.codeSize = sizeof post_draw_frag;
        shader_ci.pCode = post_draw_frag;
        result = p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
            p.device, &shader_ci, nullptr, &fragment);
    }
    if (result != VK_SUCCESS) {
        if (vertex) destroy_shader(p.device, vertex, nullptr);
        destroy_layout(p.device, layout, nullptr);
        destroy_render_pass(p.device, render_pass, nullptr);
        throw ProbeMismatch("could not create negative-viewport shader modules");
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkVertexInputBindingDescription vertex_binding{0, 4, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vertex_attribute{0, 0, VK_FORMAT_R32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 1;
    vertex_input.pVertexAttributeDescriptions = &vertex_attribute;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewports[2] = {
        {0, positive ? 0.0f : 16.0f, 16, positive ? 16.0f : -16.0f, 0, 1},
        {0, positive ? 16.0f : 0.0f, 16, positive ? -16.0f : 16.0f, 0, 1}};
    VkRect2D scissors[2] = {{{0, 0}, {16, 16}}, {{0, 0}, {16, 16}}};
    const uint32_t viewport_count = mixed ? 2u : 1u;
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = viewport_count;
    viewport_state.pViewports = dynamic ? nullptr : viewports;
    viewport_state.scissorCount = viewport_count;
    viewport_state.pScissors = scissors;
    VkDynamicState dynamic_viewport = VK_DYNAMIC_STATE_VIEWPORT;
    VkPipelineDynamicStateCreateInfo dynamic_state{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = 1;
    dynamic_state.pDynamicStates = &dynamic_viewport;
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
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
    pipeline_ci.pDynamicState = dynamic ? &dynamic_state : nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    result = p.dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(
        p.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        if (pipeline) p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(p.device, pipeline, nullptr);
        destroy_shader(p.device, fragment, nullptr);
        destroy_shader(p.device, vertex, nullptr);
        destroy_layout(p.device, layout, nullptr);
        destroy_render_pass(p.device, render_pass, nullptr);
        throw ProbeMismatch("negative viewport height was rejected (maintenance1 is not usable)");
    }

    auto image = gpu.image();
    VkFramebufferCreateInfo framebuffer_ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_ci.renderPass = render_pass;
    framebuffer_ci.attachmentCount = 1;
    framebuffer_ci.pAttachments = &image.view;
    framebuffer_ci.width = framebuffer_ci.height = 16;
    framebuffer_ci.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    auto destroy_framebuffer = p.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
    require(p.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(
                p.device, &framebuffer_ci, nullptr, &framebuffer), "create negative-viewport framebuffer");
    auto vertex_input_buffer = gpu.buffer(4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto indirect_buffer = gpu.buffer(sizeof(VkDrawIndexedIndirectCommand),
                                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    if (indexed) {
        auto *indirect_data = static_cast<VkDrawIndexedIndirectCommand *>(indirect_buffer.mapped);
        indirect_data->indexCount = 3;
        indirect_data->instanceCount = 1;
        indirect_data->firstIndex = 0;
        indirect_data->vertexOffset = 0;
        indirect_data->firstInstance = 0;
    } else {
        auto *indirect_data = static_cast<VkDrawIndirectCommand *>(indirect_buffer.mapped);
        indirect_data->vertexCount = 3;
        indirect_data->instanceCount = 1;
        indirect_data->firstVertex = 0;
        indirect_data->firstInstance = 0;
    }
    auto index_buffer = gpu.buffer(6, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    auto *indices = static_cast<uint16_t *>(index_buffer.mapped);
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    auto output = gpu.buffer(16 * 16 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkClearValue clear{};
    clear.color.float32[0] = 0.0f;
    clear.color.float32[1] = 0.0f;
    clear.color.float32[2] = 0.0f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = render_pass;
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, {16, 16}};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    auto cb = gpu.begin();
    VkImageMemoryBarrier image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.image = image.handle;
    image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    image_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &image_barrier);
    p.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (dynamic)
        p.dev<PFN_vkCmdSetViewport>("vkCmdSetViewport")(
            cb, 0, viewport_count, viewports);
    VkDeviceSize vertex_offset = 0;
    p.dev<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")(
        cb, 0, 1, &vertex_input_buffer.handle, &vertex_offset);
    uint32_t mode = 4;
    p.dev<PFN_vkCmdPushConstants>("vkCmdPushConstants")(
        cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof mode, &mode);
    if (indirect) {
        if (indexed)
            p.dev<PFN_vkCmdDrawIndexedIndirect>("vkCmdDrawIndexedIndirect")(
                cb, indirect_buffer.handle, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
        else
            p.dev<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect")(
                cb, indirect_buffer.handle, 0, 1, sizeof(VkDrawIndirectCommand));
    } else if (indexed) {
        p.dev<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer")(
            cb, index_buffer.handle, 0, VK_INDEX_TYPE_UINT16);
        p.dev<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed")(cb, 3, 1, 0, 0, 0);
    } else {
        p.dev<PFN_vkCmdDraw>("vkCmdDraw")(cb, 3, 1, 0, 0);
    }
    p.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cb);
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &image_barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {16, 16, 1};
    p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        cb, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.handle, 1, &copy);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(output);
    const auto *pixels = static_cast<const uint8_t *>(output.mapped);
    unsigned top_green = 0, bottom_green = 0;
    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 16; ++x) {
            const bool green = pixels[(y * 16 + x) * 4 + 1] > 100;
            if (green) (y < 8 ? top_green : bottom_green)++;
        }
    }
    destroy_framebuffer(p.device, framebuffer, nullptr);
    p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(p.device, pipeline, nullptr);
    destroy_shader(p.device, fragment, nullptr);
    destroy_shader(p.device, vertex, nullptr);
    destroy_layout(p.device, layout, nullptr);
    destroy_render_pass(p.device, render_pass, nullptr);
    const bool expected_positive = constant_viewport && mixed ? !positive : positive;
    const bool orientation_wrong =
        expected_positive ? top_green <= bottom_green + 20 : bottom_green <= top_green + 20;
    if (orientation_wrong) {
        char detail[160];
        std::snprintf(detail, sizeof detail,
                      "viewport pixel counts top=%u bottom=%u first=%u,%u,%u,%u",
                      top_green, bottom_green, pixels[0], pixels[1], pixels[2], pixels[3]);
        throw ProbeMismatch(detail);
    }
    row(row_name, "PASS",
        expected_positive
            ? "positive viewport kept the triangle unflipped (top/bottom pixel counts)"
            : "negative viewport height inverted the rendered triangle (top/bottom pixel counts)",
        "functional");
}

void storage16_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    GpuProbe gpu(p, physical, family);
    auto output = gpu.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    auto destroy_set = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    require(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
                p.device, &layout_ci, nullptr, &set_layout), "create storage16 layout");
    gpu.objects.push_back([=, &p] { destroy_set(p.device, set_layout, nullptr); });
    VkPipelineLayoutCreateInfo pipeline_layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_ci.setLayoutCount = 1;
    pipeline_layout_ci.pSetLayouts = &set_layout;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    require(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
                p.device, &pipeline_layout_ci, nullptr, &pipeline_layout), "create storage16 pipeline layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, pipeline_layout, nullptr); });
    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = sizeof post_storage16_comp;
    shader_ci.pCode = post_storage16_comp;
    VkShaderModule shader = VK_NULL_HANDLE;
    require(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
                p.device, &shader_ci, nullptr, &shader), "create storage16 shader");
    gpu.objects.push_back([=, &p] { destroy_shader(p.device, shader, nullptr); });
    VkComputePipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_ci.layout = pipeline_layout;
    pipeline_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_ci.stage.module = shader;
    pipeline_ci.stage.pName = "main";
    VkPipeline pipeline = VK_NULL_HANDLE;
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    auto pipeline_result = p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
        p.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline);
    if (pipeline) gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });
    require(pipeline_result, "create storage16 pipeline");
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    require(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device, &pool_ci, nullptr, &pool),
            "create storage16 pool");
    gpu.objects.push_back([=, &p] { destroy_pool(p.device, pool, nullptr); });
    VkDescriptorSetAllocateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_ci.descriptorPool = pool;
    set_ci.descriptorSetCount = 1;
    set_ci.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    require(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device, &set_ci, &set),
            "allocate storage16 set");
    VkDescriptorBufferInfo buffer_info{output.handle, 0, 16};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 1, &write, 0, nullptr);
    auto cb = gpu.begin();
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
    p.dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cb, 1, 1, 1);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(output);
    const auto value = *static_cast<const uint16_t *>(output.mapped);
    if (value != 0x1234u)
        throw ProbeMismatch("16-bit storage buffer write did not return 0x1234");
    row("core11.storage16", "PASS",
        "a StorageBuffer16BitAccess shader wrote and read back 0x1234", "functional");
}

void dispatch_base_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family,
                         uint32_t base_x, uint32_t base_y, uint32_t base_z,
                         uint32_t groups, const char *row_name) {
    auto set_mask = reinterpret_cast<PFN_vkCmdSetDeviceMask>(
        p.gdpa(p.device, "vkCmdSetDeviceMask"));
    if (!set_mask) set_mask = reinterpret_cast<PFN_vkCmdSetDeviceMask>(
        p.gdpa(p.device, "vkCmdSetDeviceMaskKHR"));
    auto dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(
        p.gdpa(p.device, "vkCmdDispatchBase"));
    if (!dispatch_base) dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(
        p.gdpa(p.device, "vkCmdDispatchBaseKHR"));
    if (!set_mask || !dispatch_base)
        throw ProbeMismatch("Vulkan 1.1 device-group command entry points are unavailable");

    GpuProbe gpu(p, physical, family);
    auto output = gpu.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    auto destroy_set = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    require(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
                p.device, &layout_ci, nullptr, &set_layout), "create dispatch-base layout");
    gpu.objects.push_back([=, &p] { destroy_set(p.device, set_layout, nullptr); });
    VkPipelineLayoutCreateInfo pipeline_layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_ci.setLayoutCount = 1;
    pipeline_layout_ci.pSetLayouts = &set_layout;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    require(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
                p.device, &pipeline_layout_ci, nullptr, &pipeline_layout), "create dispatch-base pipeline layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, pipeline_layout, nullptr); });
    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = sizeof post_push_comp;
    shader_ci.pCode = post_push_comp;
    VkShaderModule shader = VK_NULL_HANDLE;
    require(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device, &shader_ci, nullptr, &shader),
            "create dispatch-base shader");
    gpu.objects.push_back([=, &p] { destroy_shader(p.device, shader, nullptr); });
    VkComputePipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    // VUID-vkCmdDispatchBase-baseGroupX-00427: non-zero bases require this flag.
    pipeline_ci.flags = (base_x | base_y | base_z) != 0
                            ? VK_PIPELINE_CREATE_DISPATCH_BASE
                            : 0;
    pipeline_ci.layout = pipeline_layout;
    pipeline_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_ci.stage.module = shader;
    pipeline_ci.stage.pName = "main";
    VkPipeline pipeline = VK_NULL_HANDLE;
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    auto pipeline_result = p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
        p.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline);
    if (pipeline) gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });
    require(pipeline_result, "create dispatch-base pipeline");
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    require(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device, &pool_ci, nullptr, &pool),
            "create dispatch-base pool");
    gpu.objects.push_back([=, &p] { destroy_pool(p.device, pool, nullptr); });
    VkDescriptorSetAllocateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_ci.descriptorPool = pool;
    set_ci.descriptorSetCount = 1;
    set_ci.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    require(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device, &set_ci, &set),
            "allocate dispatch-base set");
    VkDescriptorBufferInfo buffer_info{output.handle, 0, 16};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 1, &write, 0, nullptr);
    auto cb = gpu.begin();
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
    set_mask(cb, 1);
    dispatch_base(cb, base_x, base_y, base_z, groups, 1, 1);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(output);
    const auto *values = static_cast<const uint32_t *>(output.mapped);
    const bool nonzero = (base_x | base_y | base_z) != 0;
    if (nonzero) {
        for (unsigned i = 0; i < base_x; ++i)
            if (values[i] != 0xa5a5a5a5u)
                throw ProbeMismatch("dispatch-base wrote outside the base range");
    }
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t index = base_x + i;
        const uint32_t expected = 0x56465800u + index +
                                  (base_y << 8) + (base_z << 16);
        if (values[index] != expected) {
            char detail[192];
            std::snprintf(detail, sizeof detail,
                          "dispatch-base value mismatch at %u (got 0x%x, expected 0x%x)",
                          index, values[index], expected);
            throw ProbeMismatch(detail);
        }
    }
    row(row_name, "PASS",
        nonzero
            ? "vkCmdDispatchBase(non-zero base) preserved all global invocation ID components"
            : "vkCmdSetDeviceMask(1) and vkCmdDispatchBase(base=0) produced the expected compute output",
        "functional");
}

void dispatch_base_image_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    GpuProbe gpu(p, physical, family);
    VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_R32G32B32A32_UINT;
    image_ci.extent = {4, 1, 1};
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    auto destroy_image = p.dev<PFN_vkDestroyImage>("vkDestroyImage");
    require(p.dev<PFN_vkCreateImage>("vkCreateImage")(p.device, &image_ci, nullptr, &image),
            "create dispatch-base image");
    gpu.objects.push_back([=, &p] { destroy_image(p.device, image, nullptr); });
    VkMemoryRequirements image_requirements{};
    p.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
        p.device, image, &image_requirements);
    VkDeviceMemory image_memory = gpu.allocate(image_requirements, 0);
    require(p.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(
                p.device, image, image_memory, 0), "bind dispatch-base image");
    VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_ci.image = image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = image_ci.format;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    auto destroy_view = p.dev<PFN_vkDestroyImageView>("vkDestroyImageView");
    require(p.dev<PFN_vkCreateImageView>("vkCreateImageView")(
                p.device, &view_ci, nullptr, &view), "create dispatch-base image view");
    gpu.objects.push_back([=, &p] { destroy_view(p.device, view, nullptr); });
    auto readback = gpu.buffer(4 * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_ci.bindingCount = 1;
    set_ci.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    auto destroy_set = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    require(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
                p.device, &set_ci, nullptr, &set_layout), "create image dispatch-base set");
    gpu.objects.push_back([=, &p] { destroy_set(p.device, set_layout, nullptr); });
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    require(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
                p.device, &layout_ci, nullptr, &layout), "create image dispatch-base layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, layout, nullptr); });
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = sizeof post_dispatch_image_comp;
    shader_ci.pCode = post_dispatch_image_comp;
    VkShaderModule shader = VK_NULL_HANDLE;
    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    require(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
                p.device, &shader_ci, nullptr, &shader), "create image dispatch-base shader");
    gpu.objects.push_back([=, &p] { destroy_shader(p.device, shader, nullptr); });
    VkComputePipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE;
    pipeline_ci.layout = layout;
    pipeline_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_ci.stage.module = shader;
    pipeline_ci.stage.pName = "main";
    VkPipeline pipeline = VK_NULL_HANDLE;
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    require(p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
                p.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline),
            "create image dispatch-base pipeline");
    gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    require(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(
                p.device, &pool_ci, nullptr, &pool), "create image dispatch-base pool");
    gpu.objects.push_back([=, &p] { destroy_pool(p.device, pool, nullptr); });
    VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_allocate.descriptorPool = pool;
    set_allocate.descriptorSetCount = 1;
    set_allocate.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    require(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(
                p.device, &set_allocate, &set), "allocate image dispatch-base set");
    VkDescriptorImageInfo image_info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &image_info;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 1, &write, 0, nullptr);

    auto cb = gpu.begin();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkClearColorValue clear{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p.dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(
        cb, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    auto dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(
        p.gdpa(p.device, "vkCmdDispatchBase"));
    if (!dispatch_base) dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(
        p.gdpa(p.device, "vkCmdDispatchBaseKHR"));
    if (!dispatch_base) throw ProbeMismatch("vkCmdDispatchBase is unavailable");
    dispatch_base(cb, 1, 0, 0, 2, 1, 1);
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {4, 1, 1};
    p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &copy);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(readback);
    const auto *pixels = static_cast<const uint32_t *>(readback.mapped);
    const auto red = [&](uint32_t x) { return pixels[x * 4]; };
    if (red(0) != 0 || red(1) != 1 || red(2) != 1)
        throw ProbeMismatch("image dispatch-base wrote the wrong coordinates");
    row("core11.dispatchBaseImage", "PASS",
        "vkCmdDispatchBase offset imageStore coordinates through the compute variant",
        "functional");
}

void dispatch_base_atomic_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    GpuProbe gpu(p, physical, family);
    auto output = gpu.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_ci.bindingCount = 1;
    set_ci.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    auto destroy_set = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    require(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
                p.device, &set_ci, nullptr, &set_layout), "create atomic dispatch-base set");
    gpu.objects.push_back([=, &p] { destroy_set(p.device, set_layout, nullptr); });
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    require(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
                p.device, &layout_ci, nullptr, &layout), "create atomic dispatch-base layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, layout, nullptr); });
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = sizeof post_dispatch_atomic_comp;
    shader_ci.pCode = post_dispatch_atomic_comp;
    VkShaderModule shader = VK_NULL_HANDLE;
    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    require(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
                p.device, &shader_ci, nullptr, &shader), "create atomic dispatch-base shader");
    gpu.objects.push_back([=, &p] { destroy_shader(p.device, shader, nullptr); });
    VkComputePipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE;
    pipeline_ci.layout = layout;
    pipeline_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_ci.stage.module = shader;
    pipeline_ci.stage.pName = "main";
    VkPipeline pipeline = VK_NULL_HANDLE;
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    require(p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
                p.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline),
            "create atomic dispatch-base pipeline");
    gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    require(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(
                p.device, &pool_ci, nullptr, &pool), "create atomic dispatch-base pool");
    gpu.objects.push_back([=, &p] { destroy_pool(p.device, pool, nullptr); });
    VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_allocate.descriptorPool = pool;
    set_allocate.descriptorSetCount = 1;
    set_allocate.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    require(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(
                p.device, &set_allocate, &set), "allocate atomic dispatch-base set");
    VkDescriptorBufferInfo buffer_info{output.handle, 0, 16};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 1, &write, 0, nullptr);

    auto cb = gpu.begin();
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    auto dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(
        p.gdpa(p.device, "vkCmdDispatchBase"));
    if (!dispatch_base) dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(
        p.gdpa(p.device, "vkCmdDispatchBaseKHR"));
    if (!dispatch_base) throw ProbeMismatch("vkCmdDispatchBase is unavailable");
    dispatch_base(cb, 1, 0, 0, 2, 1, 1);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(output);
    const auto *values = static_cast<const uint32_t *>(output.mapped);
    if (values[0] != 0xa5a5a5a5u || values[1] != 0xa5a5a5a6u ||
        values[2] != 0xa5a5a5a6u)
        throw ProbeMismatch("atomic dispatch-base wrote the wrong indices");
    row("core11.dispatchBaseAtomic", "PASS",
        "vkCmdDispatchBase offset atomicAdd indices through the compute variant",
        "functional");
}

void variable_pointers_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    auto get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!get2) get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2KHR"));
    if (!get2) {
        row("core11.variablePointers", "SKIP", "feature query unavailable", "functional");
        return;
    }
    VkPhysicalDeviceVariablePointersFeatures features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features};
    get2(physical, &query);
    if (!features.variablePointers) {
        row("core11.variablePointers", "SKIP", "feature unavailable", "functional");
        return;
    }

    GpuProbe gpu(p, physical, family);
    auto output = gpu.buffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_ci.bindingCount = 1;
    set_ci.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    auto destroy_set = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    require(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
                p.device, &set_ci, nullptr, &set_layout), "create variable-pointers set layout");
    gpu.objects.push_back([=, &p] { destroy_set(p.device, set_layout, nullptr); });
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    require(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
                p.device, &layout_ci, nullptr, &layout), "create variable-pointers layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, layout, nullptr); });
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = sizeof post_variable_pointers_comp;
    shader_ci.pCode = post_variable_pointers_comp;
    VkShaderModule shader = VK_NULL_HANDLE;
    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    require(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
                p.device, &shader_ci, nullptr, &shader), "create variable-pointers shader");
    gpu.objects.push_back([=, &p] { destroy_shader(p.device, shader, nullptr); });
    VkComputePipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_ci.layout = layout;
    pipeline_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_ci.stage.module = shader;
    pipeline_ci.stage.pName = "main";
    VkPipeline pipeline = VK_NULL_HANDLE;
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    require(p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
                p.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline),
            "create variable-pointers pipeline");
    gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    require(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(
                p.device, &pool_ci, nullptr, &pool), "create variable-pointers pool");
    gpu.objects.push_back([=, &p] { destroy_pool(p.device, pool, nullptr); });
    VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_allocate.descriptorPool = pool;
    set_allocate.descriptorSetCount = 1;
    set_allocate.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    require(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(
                p.device, &set_allocate, &set), "allocate variable-pointers set");
    VkDescriptorBufferInfo buffer_info{output.handle, 0, 4};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 1, &write, 0, nullptr);
    auto cb = gpu.begin();
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    p.dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cb, 1, 1, 1);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(output);
    const uint32_t value = *static_cast<const uint32_t *>(output.mapped);
    if (value != 4)
        throw ProbeMismatch("variable-pointers compute output mismatch");
    row("core11.variablePointers", "PASS",
        "a compute shader wrote through a pointer loaded from a Function pointer-to-pointer",
        "functional");
}

void variable_pointers_storage_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    auto get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!get2) get2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        p.gipa(p.instance, "vkGetPhysicalDeviceFeatures2KHR"));
    if (!get2) {
        row("core11.variablePointersStorageBuffer", "SKIP", "feature query unavailable",
            "functional");
        return;
    }
    VkPhysicalDeviceVariablePointersFeatures features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features};
    get2(physical, &query);
    if (!features.variablePointersStorageBuffer) {
        row("core11.variablePointersStorageBuffer", "SKIP", "feature unavailable",
            "functional");
        return;
    }

    GpuProbe gpu(p, physical, family);
    auto output = gpu.buffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_ci.bindingCount = 1;
    set_ci.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    auto destroy_set = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    require(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
                p.device, &set_ci, nullptr, &set_layout),
            "create storage variable-pointers set layout");
    gpu.objects.push_back([=, &p] { destroy_set(p.device, set_layout, nullptr); });
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    require(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
                p.device, &layout_ci, nullptr, &layout),
            "create storage variable-pointers layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, layout, nullptr); });
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = sizeof post_variable_pointers_storage_comp;
    shader_ci.pCode = post_variable_pointers_storage_comp;
    VkShaderModule shader = VK_NULL_HANDLE;
    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    require(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
                p.device, &shader_ci, nullptr, &shader),
            "create storage variable-pointers shader");
    gpu.objects.push_back([=, &p] { destroy_shader(p.device, shader, nullptr); });
    VkComputePipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_ci.layout = layout;
    pipeline_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_ci.stage.module = shader;
    pipeline_ci.stage.pName = "main";
    VkPipeline pipeline = VK_NULL_HANDLE;
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    require(p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
                p.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline),
            "create storage variable-pointers pipeline");
    gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    require(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(
                p.device, &pool_ci, nullptr, &pool), "create storage variable-pointers pool");
    gpu.objects.push_back([=, &p] { destroy_pool(p.device, pool, nullptr); });
    VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_allocate.descriptorPool = pool;
    set_allocate.descriptorSetCount = 1;
    set_allocate.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    require(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(
                p.device, &set_allocate, &set), "allocate storage variable-pointers set");
    VkDescriptorBufferInfo buffer_info{output.handle, 0, 4};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 1, &write, 0, nullptr);
    auto cb = gpu.begin();
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
        cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    p.dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cb, 1, 1, 1);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(output);
    const uint32_t value = *static_cast<const uint32_t *>(output.mapped);
    if (value != 4)
        throw ProbeMismatch("VariablePointersStorageBuffer compute output mismatch");
    row("core11.variablePointersStorageBuffer", "PASS",
        "a compute shader stored through a StorageBuffer pointer loaded from Function storage",
        "functional");
}

void spirv13_downgrade_smoke(Probe &p, VkPhysicalDevice, uint32_t) {
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = sizeof post_spirv14_comp;
    shader_ci.pCode = post_spirv14_comp;
    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    VkShaderModule shader = VK_NULL_HANDLE;
    require(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
                p.device, &shader_ci, nullptr, &shader), "create SPIR-V 1.4 shader");
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    require(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
                p.device, &layout_ci, nullptr, &layout), "create downgrade layout");
    VkComputePipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_ci.layout = layout;
    pipeline_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_ci.stage.module = shader;
    pipeline_ci.stage.pName = "main";
    VkPipeline pipeline = VK_NULL_HANDLE;
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    const VkResult result = p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
        p.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline);
    if (pipeline) destroy_pipeline(p.device, pipeline, nullptr);
    destroy_layout(p.device, layout, nullptr);
    destroy_shader(p.device, shader, nullptr);
    if (result != VK_SUCCESS)
        throw ProbeMismatch("SPIR-V 1.4 module was not lowered for the 1.1 device");
    row("core11.spirv13Downgrade", "PASS",
        "a SPIR-V 1.4 compute module was lowered to SPIR-V 1.3 and accepted", "functional");
}

void push_descriptor_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family) {
    GpuProbe gpu(p, physical, family);
    auto output = gpu.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto destroy_set = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; sci.bindingCount = 1; sci.pBindings = &binding;
    VkDescriptorSetLayout set{};
    require(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device, &sci, nullptr, &set), "create push descriptor layout");
    gpu.objects.push_back([=, &p] { destroy_set(p.device, set, nullptr); });
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; lci.setLayoutCount = 1; lci.pSetLayouts = &set;
    VkPipelineLayout layout{};
    require(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device, &lci, nullptr, &layout), "create push pipeline layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, layout, nullptr); });
    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = sizeof post_push_comp; mci.pCode = post_push_comp;
    VkShaderModule shader{};
    require(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device, &mci, nullptr, &shader), "create push shader");
    gpu.objects.push_back([=, &p] { destroy_shader(p.device, shader, nullptr); });
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pci.layout = layout; pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module = shader; pci.stage.pName = "main";
    VkPipeline pipeline{};
    VkResult result = p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(p.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline);
    // A driver may return a non-null partial result on pipeline creation error.
    if (pipeline) gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });
    require(result, "create push compute pipeline");
    auto cb = gpu.begin();
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    VkDescriptorBufferInfo buffer{output.handle, 0, 16};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstBinding = 0; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &buffer;
    p.dev<PFN_vkCmdPushDescriptorSetKHR>("vkCmdPushDescriptorSetKHR")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &write);
    p.dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cb, 4, 1, 1);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(output);
    for (unsigned i = 0; i < 4; ++i)
        if (static_cast<uint32_t *>(output.mapped)[i] != 0x56465800u + i)
            throw ProbeMismatch("push descriptor compute readback mismatch");
    row("pushDescriptor.compute", "PASS", "pushed SSBO descriptor; four compute invocations wrote expected distinct values", "functional");
}

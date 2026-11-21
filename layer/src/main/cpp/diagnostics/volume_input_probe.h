#pragma once
#include "../core/native_probe_resources.h"
#include "driver_post_shaders.h"

namespace vkfix::nativeprobe {
template<class Context> void volume_input_probe(Context& p, VkPhysicalDevice physical, uint32_t family) {
    Resources<Context> gpu(p,physical,family);
    const auto output = gpu.buffer(4*4*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_3D;
    info.flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT; info.extent = {8,8,12}; info.mipLevels = 2; info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT; info.format = VK_FORMAT_R8G8B8A8_UNORM; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const auto volume = gpu.create_image(info);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = volume; vi.format = info.format; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,1,1,5,1};
    VkImageView attachment{}, descriptor_view{}; auto destroy_view = p.template dev<PFN_vkDestroyImageView>("vkDestroyImageView");
    auto create_view = p.template dev<PFN_vkCreateImageView>("vkCreateImageView");
    check(create_view(p.device,&vi,nullptr,&attachment),"create subpass volume view");
    gpu.objects.push_back([=,&p] { destroy_view(p.device,attachment,nullptr); });
    check(create_view(p.device,&vi,nullptr,&descriptor_view),"create equivalent input descriptor view");
    gpu.objects.push_back([=,&p] { destroy_view(p.device,descriptor_view,nullptr); });
    info.imageType = VK_IMAGE_TYPE_2D; info.flags = 0; info.extent = {4,4,1}; info.mipLevels = 1;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const auto color_image = gpu.create_image(info); const auto color_view = gpu.view(color_image,info.format,VK_IMAGE_ASPECT_COLOR_BIT);
    VkAttachmentDescription descriptions[2]{};
    for (auto& a : descriptions) { a.format = info.format; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }
    VkAttachmentReference first{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, input{0,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, second{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpasses[2]{};
    for (auto& s : subpasses) { s.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; s.colorAttachmentCount = 1; }
    subpasses[0].pColorAttachments = &first; subpasses[1].pColorAttachments = &second;
    subpasses[1].inputAttachmentCount = 1; subpasses[1].pInputAttachments = &input;
    VkSubpassDependency dependency{0,1,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,VK_DEPENDENCY_BY_REGION_BIT};
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; pass_info.attachmentCount = 2; pass_info.pAttachments = descriptions;
    pass_info.subpassCount = 2; pass_info.pSubpasses = subpasses; pass_info.dependencyCount = 1; pass_info.pDependencies = &dependency;
    VkRenderPass pass{}; check(p.template dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(p.device,&pass_info,nullptr,&pass),"create two-subpass volume pass");
    auto destroy_pass = p.template dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    gpu.objects.push_back([=,&p] { destroy_pass(p.device,pass,nullptr); });
    VkImageView views[] = {attachment,color_view};
    const auto framebuffer = gpu.framebuffer(pass,2,views,{4,4});
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set_info.bindingCount = 1; set_info.pBindings = &binding;
    VkDescriptorSetLayout set_layout{};
    check(p.template dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device,&set_info,nullptr,&set_layout),"create volume input layout");
    auto destroy_set = p.template dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    gpu.objects.push_back([=,&p] { destroy_set(p.device,set_layout,nullptr); });
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout_info.setLayoutCount = 1; layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout layout{}; check(p.template dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&layout_info,nullptr,&layout),"create volume input pipeline layout");
    auto destroy_layout = p.template dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    gpu.objects.push_back([=,&p] { destroy_layout(p.device,layout,nullptr); });
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool_info.maxSets = 1; pool_info.poolSizeCount = 1; pool_info.pPoolSizes = &size;
    VkDescriptorPool pool{}; check(p.template dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&pool_info,nullptr,&pool),"create volume input pool");
    auto destroy_pool = p.template dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    gpu.objects.push_back([=,&p] { destroy_pool(p.device,pool,nullptr); });
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = pool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &set_layout;
    VkDescriptorSet set{}; check(p.template dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device,&allocate,&set),"allocate volume input set");
    VkDescriptorImageInfo image_info{VK_NULL_HANDLE,descriptor_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = set; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; write.pImageInfo = &image_info;
    p.template dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,1,&write,0,nullptr);
    VkPipelineShaderStageCreateInfo stages[] = {
        gpu.shader_stage(VK_SHADER_STAGE_VERTEX_BIT,post_volume_input_vert,sizeof post_volume_input_vert),
        gpu.shader_stage(VK_SHADER_STAGE_FRAGMENT_BIT,post_volume_input_frag,sizeof post_volume_input_frag)};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,4,4,0,1}; VkRect2D scissor{{0,0},{4,4}};
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = viewport_state.scissorCount = 1; viewport_state.pViewports = &viewport; viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{}; blend_attachment.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &blend_attachment;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; ci.stageCount = 2; ci.pStages = stages;
    ci.pVertexInputState = &vertex; ci.pInputAssemblyState = &assembly; ci.pViewportState = &viewport_state;
    ci.pRasterizationState = &raster; ci.pMultisampleState = &samples; ci.pColorBlendState = &blend;
    ci.layout = layout; ci.renderPass = pass; ci.subpass = 1;
    VkPipeline pipeline{};
    check(p.template dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(p.device,{},1,&ci,nullptr,&pipeline),"create volume input pipeline");
    auto destroy_pipeline = p.template dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    gpu.objects.push_back([=,&p] { destroy_pipeline(p.device,pipeline,nullptr); });
    const auto cmd = gpu.begin(); VkClearValue clears[2]{};
    clears[0].color.float32[0] = 1; clears[0].color.float32[2] = 1; clears[0].color.float32[3] = 1;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = pass; begin.framebuffer = framebuffer;
    begin.renderArea.extent = {4,4}; begin.clearValueCount = 2; begin.pClearValues = clears;
    p.template dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
    p.template dev<PFN_vkCmdNextSubpass>("vkCmdNextSubpass")(cmd,VK_SUBPASS_CONTENTS_INLINE);
    p.template dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    p.template dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
    p.template dev<PFN_vkCmdDraw>("vkCmdDraw")(cmd,3,1,0,0);
    p.template dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cmd);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = color_image;
    barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    p.template dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {4,4,1};
    p.template dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd,color_image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
    gpu.host_barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT); gpu.submit(cmd); gpu.invalidate(output);
    const auto* pixels = static_cast<const uint32_t*>(output.mapped);
    for (unsigned i = 0; i < 16; ++i) if (pixels[i] != 0xffff00ffu)
        throw std::runtime_error("volume input attachment did not observe the previous subpass through its equivalent descriptor view");
}
}

#pragma once
#include "../core/native_probe_resources.h"
#include "driver_post_shaders.h"
#include <cstdio>

namespace vkfix::nativeprobe {
// Exported-API exercise: no renderer-private registries or replay helpers are
// available here. The same workload runs against native and emulated drivers.
template<class Context> void volume_attachment_probe(Context& p, VkPhysicalDevice physical, uint32_t family,
    bool templates, bool secondary, bool dynamic = false, bool events2 = false) {
    Resources<Context> gpu(p,physical,family);
    constexpr uint32_t base_words = 16 * 12 * 16, selected_words = 8 * 6 * 8;
    const auto upload = gpu.buffer((base_words + selected_words) * 4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const auto output = gpu.buffer(selected_words * 4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto marker = [](uint32_t x, uint32_t y, uint32_t z, uint32_t mip) {
        return 0xff000000u | (x * 11 + mip * 17) | ((y * 13 + mip * 23) << 8) | ((z * 7 + mip * 19) << 16);
    };
    auto* words = static_cast<uint32_t*>(upload.mapped);
    for (uint32_t mip = 0; mip < 2; ++mip) {
        const auto width = 16u >> mip, height = 12u >> mip, depth = 16u >> mip, offset = mip ? base_words : 0;
        for (uint32_t z = 0; z < depth; ++z) for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
            words[offset+(z*height+y)*width+x] = marker(x,y,z,mip);
    }
    VkMappedMemoryRange mapped{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; mapped.memory = upload.memory; mapped.size = VK_WHOLE_SIZE;
    check(p.template dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,1,&mapped),"flush volume upload");
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image_info.flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    image_info.imageType = VK_IMAGE_TYPE_3D; image_info.format = VK_FORMAT_R8G8B8A8_UNORM; image_info.extent = {16,12,16};
    image_info.mipLevels = 2; image_info.arrayLayers = 1; image_info.samples = VK_SAMPLE_COUNT_1_BIT; image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto volume = gpu.create_image(image_info);
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view_info.image = volume;
    view_info.format = image_info.format; view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,1,1,2,4};
    VkImageView attachment{}, sampled{};
    auto create_view = p.template dev<PFN_vkCreateImageView>("vkCreateImageView");
    auto destroy_view = p.template dev<PFN_vkDestroyImageView>("vkDestroyImageView");
    check(create_view(p.device,&view_info,nullptr,&attachment),"create volume attachment view");
    gpu.objects.push_back([=,&p] { destroy_view(p.device,attachment,nullptr); });
    view_info.viewType = VK_IMAGE_VIEW_TYPE_3D; view_info.subresourceRange.baseArrayLayer = 0; view_info.subresourceRange.layerCount = 1;
    check(create_view(p.device,&view_info,nullptr,&sampled),"create actual sampler3D view");
    gpu.objects.push_back([=,&p] { destroy_view(p.device,sampled,nullptr); });
    VkAttachmentDescription description{}; description.format = image_info.format; description.samples = VK_SAMPLE_COUNT_1_BIT;
    description.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    description.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; description.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = pass_info.subpassCount = 1; pass_info.pAttachments = &description; pass_info.pSubpasses = &subpass;
    VkRenderPass pass{};
    check(p.template dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(p.device,&pass_info,nullptr,&pass),"create volume render pass");
    auto destroy_pass = p.template dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    gpu.objects.push_back([=,&p] { destroy_pass(p.device,pass,nullptr); });
    VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb_info.renderPass = pass;
    fb_info.attachmentCount = 1; fb_info.pAttachments = &attachment; fb_info.width = 8; fb_info.height = 6; fb_info.layers = 4;
    VkFramebuffer framebuffer{};
    check(p.template dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device,&fb_info,nullptr,&framebuffer),"create layered volume framebuffer");
    auto destroy_fb = p.template dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
    gpu.objects.push_back([=,&p] { destroy_fb(p.device,framebuffer,nullptr); });
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{}; check(p.template dev<PFN_vkCreateSampler>("vkCreateSampler")(p.device,&sampler_info,nullptr,&sampler),"create volume sampler");
    auto destroy_sampler = p.template dev<PFN_vkDestroySampler>("vkDestroySampler");
    gpu.objects.push_back([=,&p] { destroy_sampler(p.device,sampler,nullptr); });
    VkDescriptorSetLayoutBinding bindings[] = {{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set_info.bindingCount = 2; set_info.pBindings = bindings;
    VkDescriptorSetLayout set_layout{};
    check(p.template dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device,&set_info,nullptr,&set_layout),"create volume sample layout");
    auto destroy_set_layout = p.template dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    gpu.objects.push_back([=,&p] { destroy_set_layout(p.device,set_layout,nullptr); });
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout_info.setLayoutCount = 1; layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout layout{}; check(p.template dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&layout_info,nullptr,&layout),"create volume compute layout");
    auto destroy_layout = p.template dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    gpu.objects.push_back([=,&p] { destroy_layout(p.device,layout,nullptr); });
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool_info.maxSets = 2; pool_info.poolSizeCount = 2; pool_info.pPoolSizes = sizes;
    VkDescriptorPool pool{}; check(p.template dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&pool_info,nullptr,&pool),"create sample descriptor pool");
    auto destroy_pool = p.template dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    gpu.objects.push_back([=,&p] { destroy_pool(p.device,pool,nullptr); });
    VkDescriptorSetLayout set_layouts[] = {set_layout,set_layout}; VkDescriptorSet sets[2]{};
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = pool;
    allocation.descriptorSetCount = 2; allocation.pSetLayouts = set_layouts;
    check(p.template dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device,&allocation,sets),"allocate volume sample sets");
    struct Payload { uint64_t prefix; VkDescriptorImageInfo image; VkDescriptorBufferInfo buffer; } payload{};
    payload.image = {sampler,sampled,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; payload.buffer = {output.handle,0,VK_WHOLE_SIZE};
    if (templates) {
        VkDescriptorUpdateTemplateEntry entries[] = {
            {0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,offsetof(Payload,image),sizeof payload.image},
            {1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,offsetof(Payload,buffer),sizeof payload.buffer}};
        VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET; info.descriptorSetLayout = set_layout;
        info.descriptorUpdateEntryCount = 2; info.pDescriptorUpdateEntries = entries;
        VkDescriptorUpdateTemplate update{};
        check(p.template dev<PFN_vkCreateDescriptorUpdateTemplate>("vkCreateDescriptorUpdateTemplate")(p.device,&info,nullptr,&update),"create volume descriptor template");
        p.template dev<PFN_vkUpdateDescriptorSetWithTemplate>("vkUpdateDescriptorSetWithTemplate")(p.device,sets[0],update,&payload);
        p.template dev<PFN_vkDestroyDescriptorUpdateTemplate>("vkDestroyDescriptorUpdateTemplate")(p.device,update,nullptr);
    } else {
        VkWriteDescriptorSet writes[2]{};
        for (auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = sets[0]; write.descriptorCount = 1; }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &payload.image;
        writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &payload.buffer;
        p.template dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,2,writes,0,nullptr);
    }
    // Copy to a separate set: layout translation must survive native copies.
    VkCopyDescriptorSet copies[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        copies[i].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET; copies[i].srcSet = sets[0]; copies[i].dstSet = sets[1];
        copies[i].srcBinding = copies[i].dstBinding = i; copies[i].descriptorCount = 1;
    }
    p.template dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,0,nullptr,2,copies);
    const auto shader = gpu.shader_stage(VK_SHADER_STAGE_COMPUTE_BIT,post_volume_sample_comp,sizeof post_volume_sample_comp);
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pipeline_info.stage = shader; pipeline_info.layout = layout;
    VkPipeline pipeline{};
    check(p.template dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(p.device,{},1,&pipeline_info,nullptr,&pipeline),"create sampler3D compute pipeline");
    auto destroy_pipeline = p.template dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    gpu.objects.push_back([=,&p] { destroy_pipeline(p.device,pipeline,nullptr); });
    VkEventCreateInfo event_info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VkEvent event{};
    check(p.template dev<PFN_vkCreateEvent>("vkCreateEvent")(p.device,&event_info,nullptr,&event),"create volume event");
    auto destroy_event = p.template dev<PFN_vkDestroyEvent>("vkDestroyEvent");
    gpu.objects.push_back([=,&p] { destroy_event(p.device,event,nullptr); });
    auto cmd = gpu.begin();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = volume;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,2,0,1};
    auto image_barrier = p.template dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    image_barrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy regions[2]{};
    for (uint32_t mip = 0; mip < 2; ++mip) {
        regions[mip].bufferOffset = mip ? base_words*4 : 0; regions[mip].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,mip,0,1};
        regions[mip].imageExtent = {16u >> mip,12u >> mip,16u >> mip};
    }
    p.template dev<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(cmd,upload.handle,volume,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,2,regions);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,1,1,0,VK_REMAINING_ARRAY_LAYERS};
    image_barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    const VkRect2D area{{1,1},{5,4}};
    auto clear = [&](VkCommandBuffer target) {
        VkClearAttachment value{}; value.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; value.colorAttachment = 0;
        value.clearValue.color.float32[0] = value.clearValue.color.float32[3] = 1;
        VkClearRect rectangle{{{2,2},{2,2}},3,1};
        p.template dev<PFN_vkCmdClearAttachments>("vkCmdClearAttachments")(target,1,&value,1,&rectangle);
    };
    if (dynamic) {
        VkRenderingAttachmentInfo attachment_info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; attachment_info.imageView = attachment;
        attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; attachment_info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; attachment_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea = area; rendering.layerCount = 4;
        rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &attachment_info;
        p.template dev<PFN_vkCmdBeginRenderingKHR>("vkCmdBeginRenderingKHR")(cmd,&rendering); clear(cmd);
        p.template dev<PFN_vkCmdEndRenderingKHR>("vkCmdEndRenderingKHR")(cmd);
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        image_barrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    } else {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = pass;
        begin.framebuffer = framebuffer; begin.renderArea = area;
        p.template dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cmd,&begin,secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
        if (secondary) {
            VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; info.queueFamilyIndex = family;
            VkCommandPool pool{}; check(p.template dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(p.device,&info,nullptr,&pool),"create volume secondary pool");
            auto destroy = p.template dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool");
            gpu.objects.push_back([=,&p] { destroy(p.device,pool,nullptr); });
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool = pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY; allocation.commandBufferCount = 1; VkCommandBuffer child{};
            check(p.template dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(p.device,&allocation,&child),"allocate volume secondary");
            VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}; inheritance.renderPass = pass; inheritance.framebuffer = framebuffer;
            VkCommandBufferBeginInfo info_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; info_begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT; info_begin.pInheritanceInfo = &inheritance;
            check(p.template dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(child,&info_begin),"begin volume secondary");
            clear(child); check(p.template dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(child),"end volume secondary");
            p.template dev<PFN_vkCmdExecuteCommands>("vkCmdExecuteCommands")(cmd,1,&child);
        } else clear(cmd);
        p.template dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cmd);
    }
    barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    if (events2) {
        VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; image.image = volume;
        image.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; image.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        image.srcAccessMask = barrier.srcAccessMask; image.dstAccessMask = barrier.dstAccessMask;
        image.oldLayout = barrier.oldLayout; image.newLayout = barrier.newLayout; image.subresourceRange = barrier.subresourceRange;
        image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &image;
        p.template dev<PFN_vkCmdSetEvent2KHR>("vkCmdSetEvent2KHR")(cmd,event,&dependency);
        p.template dev<PFN_vkCmdWaitEvents2KHR>("vkCmdWaitEvents2KHR")(cmd,1,&event,&dependency);
        p.template dev<PFN_vkCmdResetEvent2KHR>("vkCmdResetEvent2KHR")(cmd,event,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    } else {
        p.template dev<PFN_vkCmdSetEvent>("vkCmdSetEvent")(cmd,event,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        p.template dev<PFN_vkCmdWaitEvents>("vkCmdWaitEvents")(cmd,1,&event,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,nullptr,0,nullptr,1,&barrier);
        p.template dev<PFN_vkCmdResetEvent>("vkCmdResetEvent")(cmd,event,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    p.template dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    p.template dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&sets[1],0,nullptr);
    p.template dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cmd,8,6,8);
    gpu.host_barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cmd); gpu.invalidate(output);
    const auto* pixels = static_cast<const uint32_t*>(output.mapped);
    for (uint32_t z = 0; z < 8; ++z) for (uint32_t y = 0; y < 6; ++y) for (uint32_t x = 0; x < 8; ++x) {
        const auto expected = z == 5 && x >= 2 && x < 4 && y >= 2 && y < 4 ? 0xff0000ffu : marker(x,y,z,1);
        if (pixels[(z*6+y)*8+x] != expected) {
            char detail[192]; std::snprintf(detail,sizeof detail,"volume sampler3D xyz(%u,%u,%u): got %08x expected %08x",x,y,z,pixels[(z*6+y)*8+x],expected);
            throw std::runtime_error(detail);
        }
    }
}
}

// Shared representative storage-buffer array execution for POST and GPU regression.
#pragma once
#include "native_probe_resources.h"
#include <cstdio>
namespace vkfix::nativeprobe {
struct StorageArrayReadback {
    std::string error;
    bool aggregate_store_lost = false;
};
template<class Context>
StorageArrayReadback storage_buffer_array(Context& p, VkPhysicalDevice physical, uint32_t family,
                                 const uint32_t* words, size_t word_count, bool specialized, bool dispatch_base=false,
                                 const uint32_t* vertex_words=nullptr,size_t vertex_count=0,
                                 void (*ready)()=nullptr) {
    const bool graphics=vertex_words!=nullptr;
    const auto shader_stage=graphics?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_COMPUTE_BIT;
    vkfix::nativeprobe::Resources<Context> gpu(p, physical, family);
    auto u = gpu.buffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto a = gpu.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto b = gpu.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto c = gpu.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const uint32_t u_values[4]{7,11,13,17}, a_values[4]{19,23,29,31}, b_values[4]{37,41,43,47};
    memcpy(u.mapped, u_values, 16); memcpy(a.mapped, a_values, 16); memcpy(b.mapped, b_values, 16);
    const uint32_t c_values[4]{53,59,61,67}; memcpy(c.mapped,c_values,16);
    VkMappedMemoryRange ranges[4]{};
    const VkDeviceMemory memories[4]{u.memory,a.memory,b.memory,c.memory};
    for (unsigned i = 0; i < 4; ++i) {
        ranges[i] = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; ranges[i].memory = memories[i]; ranges[i].size = VK_WHOLE_SIZE;
    }
    check(p.template dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,4,ranges), "flush input");
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i=0; i<4; ++i)
        bindings[i] = {i, i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       i == 3 ? (specialized ? 3u : 2u) : 1u, shader_stage};
    VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_ci.bindingCount=4; set_ci.pBindings=bindings;
    VkDescriptorSetLayout set_layout{};
    check(p.template dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device,&set_ci,nullptr,&set_layout),"set layout");
    auto destroy_set_layout=p.template dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    gpu.objects.push_back([=,&p] { destroy_set_layout(p.device,set_layout,nullptr); });
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_ci.setLayoutCount=1; layout_ci.pSetLayouts=&set_layout;
    VkPipelineLayout layout{};
    check(p.template dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&layout_ci,nullptr,&layout),"pipeline layout");
    auto destroy_layout=p.template dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    gpu.objects.push_back([=,&p] { destroy_layout(p.device,layout,nullptr); });
    VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,specialized ? 5u : 4u}};
    VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_ci.maxSets=1; pool_ci.poolSizeCount=2; pool_ci.pPoolSizes=sizes;
    VkDescriptorPool pool{};
    check(p.template dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&pool_ci,nullptr,&pool),"pool");
    auto destroy_pool=p.template dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    gpu.objects.push_back([=,&p] { destroy_pool(p.device,pool,nullptr); });
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool=pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&set_layout;
    VkDescriptorSet set{};
    check(p.template dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device,&allocate,&set),"set");
    VkDescriptorBufferInfo infos[6]{{u.handle,0,16},{a.handle,0,16},{b.handle,0,16},{a.handle,0,16},{b.handle,0,16},{c.handle,0,16}};
    VkWriteDescriptorSet writes[4]{};
    for (unsigned i=0; i<4; ++i) {
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=set; writes[i].dstBinding=i;
        writes[i].descriptorCount=bindings[i].descriptorCount; writes[i].descriptorType=bindings[i].descriptorType;
        writes[i].pBufferInfo=&infos[i];
    }
    p.template dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,4,writes,0,nullptr);
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize=word_count*4; shader_ci.pCode=words;
    VkShaderModule shader{};
    check(p.template dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&shader_ci,nullptr,&shader),"lowered shader");
    auto destroy_shader=p.template dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    const auto shader_cleanup=gpu.objects.size();
    gpu.objects.push_back([=,&p] { destroy_shader(p.device,shader,nullptr); });
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    if (dispatch_base) ci.flags=VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    ci.layout=layout; ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=shader; ci.stage.pName="main";
    const uint32_t length=3;
    VkSpecializationMapEntry entry{7,0,sizeof length};
    VkSpecializationInfo specialization{1,&entry,sizeof length,&length};
    if (specialized) ci.stage.pSpecializationInfo=&specialization;
    VkPipeline pipeline{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};
    if (!graphics) {
        check(p.template dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(p.device,{},1,&ci,nullptr,&pipeline),"pipeline");
    } else {
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.subpassCount=1; rp.pSubpasses=&subpass;
        check(p.template dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(p.device,&rp,nullptr,&render_pass),"storage fragment render pass");
        auto destroy_rp=p.template dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
        gpu.objects.push_back([=,&p] { destroy_rp(p.device,render_pass,nullptr); });
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass=render_pass; fb.width=fb.height=fb.layers=1;
        check(p.template dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device,&fb,nullptr,&framebuffer),"storage fragment framebuffer");
        auto destroy_fb=p.template dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
        gpu.objects.push_back([=,&p] { destroy_fb(p.device,framebuffer,nullptr); });
        VkShaderModuleCreateInfo vs{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        vs.pCode=vertex_words; vs.codeSize=vertex_count*4;
        VkShaderModule vertex{};
        check(p.template dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&vs,nullptr,&vertex),"storage vertex shader");
        gpu.objects.push_back([=,&p] { destroy_shader(p.device,vertex,nullptr); });
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module=vertex; stages[0].pName="main";
        stages[1]=ci.stage; stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,1,1,0,1}; VkRect2D scissor{{0,0},{1,1}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount=vp.scissorCount=1; vp.pViewports=&viewport; vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode=VK_POLYGON_MODE_FILL; rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount=2; gp.pStages=stages; gp.pVertexInputState=&vi; gp.pInputAssemblyState=&ia;
        gp.pViewportState=&vp; gp.pRasterizationState=&rs; gp.pMultisampleState=&ms; gp.pColorBlendState=&blend;
        gp.layout=layout; gp.renderPass=render_pass;
        check(p.template dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(p.device,{},1,&gp,nullptr,&pipeline),"storage fragment pipeline");
    }
    // Pipelines must retain executable semantics after application modules die.
    // In particular dispatch-base variants must own the specialized source.
    destroy_shader(p.device,shader,nullptr);
    gpu.objects.erase(gpu.objects.begin()+shader_cleanup);
    auto destroy_pipeline=p.template dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    gpu.objects.push_back([=,&p] { destroy_pipeline(p.device,pipeline,nullptr); });
    const auto cmd=gpu.begin();
    if (ready) ready();
    const auto point=graphics?VK_PIPELINE_BIND_POINT_GRAPHICS:VK_PIPELINE_BIND_POINT_COMPUTE;
    if (graphics) {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass=render_pass; begin.framebuffer=framebuffer; begin.renderArea.extent={1,1};
        p.template dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
    }
    p.template dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cmd,point,pipeline);
    p.template dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cmd,point,layout,0,1,&set,0,nullptr);
    if (graphics) {
        p.template dev<PFN_vkCmdDraw>("vkCmdDraw")(cmd,3,1,0,0);
        p.template dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cmd);
    } else if (dispatch_base) p.template dev<PFN_vkCmdDispatchBase>("vkCmdDispatchBase")(cmd,1,0,0,1,1,1);
    else p.template dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cmd,1,1,1);
    gpu.host_barrier(cmd,graphics?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cmd); gpu.invalidate(a); gpu.invalidate(b); gpu.invalidate(c); gpu.invalidate(u);
    const uint32_t expected_a[4]{7,11,60,17}, expected_b[4]{7,dispatch_base?23u:22u,13,17};
    const bool other_buffers_match = !memcmp(u.mapped,u_values,16) && !memcmp(a.mapped,expected_a,16) &&
                                     !memcmp(b.mapped,expected_b,16);
    if (!other_buffers_match || memcmp(c.mapped,c_values,16)) {
        const auto av=static_cast<const uint32_t*>(a.mapped), bv=static_cast<const uint32_t*>(b.mapped);
        const auto cv=static_cast<const uint32_t*>(c.mapped), uv=static_cast<const uint32_t*>(u.mapped);
        char detail[300];
        std::snprintf(detail,sizeof detail,"readback mismatch A=(%u,%u,%u,%u), B=(%u,%u,%u,%u), C=(%u,%u,%u,%u), U=(%u,%u,%u,%u)",
            av[0],av[1],av[2],av[3],bv[0],bv[1],bv[2],bv[3],cv[0],cv[1],cv[2],cv[3],uv[0],uv[1],uv[2],uv[3]);
        const bool store_lost = specialized && other_buffers_match && cv[0] == 0 &&
                                !memcmp(cv + 1, c_values + 1, 3 * sizeof(uint32_t));
        return {detail, store_lost};
    }
    return {};
}
}

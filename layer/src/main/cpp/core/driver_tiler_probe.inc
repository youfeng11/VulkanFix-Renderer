// Included inside driver_trust_probe.cpp's private namespace.
// Same geometry/output in both arms. Only the preserving pass boundaries differ.
bool probe_tiler_pressure(NativeContext &p,VkPhysicalDevice physical,uint32_t family,bool subject,bool multi) {
    Resources gpu(p,physical,family);
    auto image=gpu.image();
    auto output=gpu.buffer(1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto indirect=gpu.buffer(128*sizeof(VkDrawIndirectCommand),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    for (unsigned i=0;i<128;++i) static_cast<VkDrawIndirectCommand*>(indirect.mapped)[i]={65535,1,0,0};
    VkMappedMemoryRange input{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; input.memory=indirect.memory; input.size=VK_WHOLE_SIZE;
    nativeprobe::check(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,1,&input),"tiler indirect input");
    auto destroy_pass=p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    VkAttachmentDescription attachment{};
    attachment.format=VK_FORMAT_R8G8B8A8_UNORM; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1; subpass.pColorAttachments=&ref;
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount=ri.subpassCount=1; ri.pAttachments=&attachment; ri.pSubpasses=&subpass;
    VkRenderPass passes[2]{};
    for (unsigned i=0;i<2;++i) {
        if (i) attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        nativeprobe::check(p.dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(p.device,&ri,nullptr,&passes[i]),"tiler probe pass");
        const auto pass=passes[i]; gpu.objects.push_back([=,&p]{ destroy_pass(p.device,pass,nullptr); });
    }
    auto destroy_framebuffer=p.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass=passes[0]; fi.attachmentCount=1; fi.pAttachments=&image.view; fi.width=fi.height=16; fi.layers=1;
    VkFramebuffer framebuffer{};
    nativeprobe::check(p.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device,&fi,nullptr,&framebuffer),"tiler probe framebuffer");
    gpu.objects.push_back([=,&p]{ destroy_framebuffer(p.device,framebuffer,nullptr); });
    auto destroy_layout=p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    VkPushConstantRange color{VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.pushConstantRangeCount=1; li.pPushConstantRanges=&color;
    VkPipelineLayout layout{};
    nativeprobe::check(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&li,nullptr,&layout),"tiler probe layout");
    gpu.objects.push_back([=,&p]{ destroy_layout(p.device,layout,nullptr); });
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (unsigned i=0;i<2;++i) {
        auto destroy_shader=p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.pCode=i ? nativeprobe::tiler_frag : nativeprobe::tiler_vert;
        sm.codeSize=i ? sizeof nativeprobe::tiler_frag : sizeof nativeprobe::tiler_vert;
        VkShaderModule module{};
        nativeprobe::check(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&sm,nullptr,&module),"tiler probe shader");
        gpu.objects.push_back([=,&p]{ destroy_shader(p.device,module,nullptr); });
        stages[i]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage=i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; stages[i].module=module; stages[i].pName="main";
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,16,16,0,1}; VkRect2D scissor{{0,0},{16,16}};
    VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vs.viewportCount=vs.scissorCount=1; vs.pViewports=&viewport; vs.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState write{}; write.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=1; blend.pAttachments=&write;
    VkDynamicState dynamic=VK_DYNAMIC_STATE_SCISSOR;
    VkPipelineDynamicStateCreateInfo di{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; di.dynamicStateCount=1; di.pDynamicStates=&dynamic;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount=2; pi.pStages=stages; pi.pVertexInputState=&vi; pi.pInputAssemblyState=&ia; pi.pViewportState=&vs;
    pi.pRasterizationState=&rs; pi.pMultisampleState=&ms; pi.pColorBlendState=&blend; pi.pDynamicState=&di;
    pi.layout=layout; pi.renderPass=passes[0];
    auto destroy_pipeline=p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    VkPipeline pipeline{};
    auto result=p.dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(p.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline);
    if (pipeline) gpu.objects.push_back([=,&p]{ destroy_pipeline(p.device,pipeline,nullptr); });
    nativeprobe::check(result,"tiler probe pipeline");
    std::puts("VKFIX_PROBE_READY"); std::fflush(stdout);
    bool matched=true;
    for (unsigned workload=0;workload<(multi ? 3u : 2u);++workload) for (unsigned trial=0;trial<4;++trial) {
        auto cb=gpu.begin();
        auto barrier=p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        ib.image=image.handle; ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; ib.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&ib);
        VkClearValue clear{};
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; bi.renderPass=passes[0]; bi.framebuffer=framebuffer;
        bi.renderArea.extent={16,16}; bi.clearValueCount=1; bi.pClearValues=&clear;
        auto begin=p.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
        auto end=p.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
        begin(cb,&bi,VK_SUBPASS_CONTENTS_INLINE);
        p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        const unsigned commands=workload==1 ? 64 : 16;
        for (unsigned draw=0;draw<commands;++draw) {
            if (!subject && draw && draw%4==0) {
                end(cb);
                VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; memory.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
                memory.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
                barrier(cb,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,1,&memory,0,nullptr,0,nullptr);
                bi.renderPass=passes[1]; bi.clearValueCount=0; bi.pClearValues=nullptr;
                begin(cb,&bi,VK_SUBPASS_CONTENTS_INLINE);
            }
            const unsigned quadrant=std::min(draw,3u);
            VkRect2D area{{int32_t((quadrant%2)*8),int32_t((quadrant/2)*8)},{8,8}};
            p.dev<PFN_vkCmdSetScissor>("vkCmdSetScissor")(cb,0,1,&area);
            const float rgba[]{float((quadrant+trial)&1),float(((quadrant+trial)>>1)&1),float(trial&1),1.f};
            p.dev<PFN_vkCmdPushConstants>("vkCmdPushConstants")(cb,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,16,rgba);
            if (!workload) p.dev<PFN_vkCmdDraw>("vkCmdDraw")(cb,65535,1,0,0);
            else {
                const uint32_t count=workload==2 ? 8 : 1;
                p.dev<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect")(cb,indirect.handle,
                    draw*count*sizeof(VkDrawIndirectCommand),count,sizeof(VkDrawIndirectCommand));
            }
        }
        end(cb);
        ib.oldLayout=ib.newLayout; ib.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={16,16,1};
        p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cb,image.handle,ib.newLayout,output.handle,1,&copy);
        gpu.host_barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        gpu.submit(cb); gpu.invalidate(output);
        const auto pixels=static_cast<const unsigned char*>(output.mapped);
        for (unsigned y=0;y<16;++y) for (unsigned x=0;x<16;++x) {
            const unsigned quadrant=x/8+(y/8)*2;
            const unsigned char expected[]{static_cast<unsigned char>(((quadrant+trial)&1)*255),
                static_cast<unsigned char>((((quadrant+trial)>>1)&1)*255),static_cast<unsigned char>((trial&1)*255),255};
            matched &= !std::memcmp(pixels+(y*16+x)*4,expected,4);
        }
    }
    return matched;
}

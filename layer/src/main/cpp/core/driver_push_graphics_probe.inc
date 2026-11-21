// Included in driver_trust_probe.cpp's private namespace. All shader resources
// are live: unused descriptor declarations cannot establish graphics health.
bool probe_push_graphics(NativeContext &p,VkPhysicalDevice physical,uint32_t family,bool subject,bool indexed_heap,unsigned vertex_mode) {
    const bool vertex_resources=vertex_mode!=0;
    Resources gpu(p,physical,family);
    VkPhysicalDeviceProperties properties{};
    p.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&properties);
    VkFormatProperties format{},texel{};
    auto query_format=p.get<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
    query_format(physical,VK_FORMAT_R8G8B8A8_UNORM,&format);
    query_format(physical,VK_FORMAT_R32G32B32A32_SFLOAT,&texel);
    if (!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !(texel.bufferFeatures&VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT))
        throw std::runtime_error("graphics push probe formats unsupported");
    const auto offset=std::max<VkDeviceSize>({16,properties.limits.minUniformBufferOffsetAlignment,
                                            properties.limits.minStorageBufferOffsetAlignment});
    if (offset>65536) throw std::runtime_error("graphics push alignment exceeds probe budget");
    std::array<Resources::Buffer,4> inputs;
    float values[4][4]{};
    VkBufferView views[4]{};
    for (unsigned i=0;i<4;++i) {
        inputs[i]=gpu.buffer(offset+16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
        values[i][0]=float(i&1); values[i][1]=float((i>>1)&1); values[i][2]=float((i^(i>>1))&1); values[i][3]=1;
        std::memcpy(inputs[i].mapped,values[i],16);
        std::memcpy(static_cast<char*>(inputs[i].mapped)+offset,values[i],16);
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=inputs[i].memory; range.size=VK_WHOLE_SIZE;
        nativeprobe::check(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,1,&range),"graphics push inputs");
        VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
        ci.buffer=inputs[i].handle; ci.format=VK_FORMAT_R32G32B32A32_SFLOAT; ci.range=16;
        nativeprobe::check(p.dev<PFN_vkCreateBufferView>("vkCreateBufferView")(p.device,&ci,nullptr,&views[i]),"graphics push texel view");
        auto destroy=p.dev<PFN_vkDestroyBufferView>("vkDestroyBufferView");
        const auto view=views[i]; gpu.objects.push_back([=,&p]{ destroy(p.device,view,nullptr); });
    }
    auto target=gpu.image();
    auto output=gpu.buffer(1024,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Resources::Image images[3];
    for (auto &image:images) image=gpu.image(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkSampler sampler{};
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter=sci.minFilter=VK_FILTER_NEAREST; sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    nativeprobe::check(p.dev<PFN_vkCreateSampler>("vkCreateSampler")(p.device,&sci,nullptr,&sampler),"graphics push sampler");
    auto destroy_sampler=p.dev<PFN_vkDestroySampler>("vkDestroySampler");
    gpu.objects.push_back([=,&p]{ destroy_sampler(p.device,sampler,nullptr); });
    VkDescriptorSetLayoutBinding bindings[]{
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {6,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {7,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {9,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {11,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    for (auto &binding:bindings) binding.stageFlags=vertex_resources ?
        (binding.binding==2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT) : VK_SHADER_STAGE_ALL;
    // VulkanFix exposes vertex UBOs to its compute/replay stage. Exercise that
    // legal stage-mask combination separately from the unmodified native case.
    if (vertex_mode==2) bindings[1].stageFlags|=VK_SHADER_STAGE_COMPUTE_BIT;
    auto create_set=p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    auto destroy_set=p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    VkDescriptorSetLayout sets[2]{};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    // A non-empty ordinary heap before set 1 exercises a different native path
    // from the original empty-set compute transition. It is not statically used.
    VkDescriptorSetLayoutBinding heap{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_ALL,nullptr};
    si.bindingCount=1; si.pBindings=&heap;
    VkDescriptorSetLayoutBinding heap_bindings[4]{};
    constexpr VkDescriptorBindingFlags partial=VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT|VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags heap_flags[]{partial|VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        partial|VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,partial|VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,partial};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    if (indexed_heap) {
        VkPhysicalDeviceDescriptorIndexingProperties limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext=&limits;
        p.get<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(physical,&props);
        auto count=[](uint32_t requested,uint32_t stage,uint32_t set,uint32_t reserve) {
            const auto bound=std::min(stage,set);
            if (bound<=reserve) throw std::runtime_error("indexed graphics push heap limit unavailable");
            return std::min(requested,bound-reserve);
        };
        heap_bindings[0]={0,VK_DESCRIPTOR_TYPE_SAMPLER,count(128,limits.maxPerStageDescriptorUpdateAfterBindSamplers,limits.maxDescriptorSetUpdateAfterBindSamplers,2),VK_SHADER_STAGE_ALL,nullptr};
        heap_bindings[1]={1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,count(2048,limits.maxPerStageDescriptorUpdateAfterBindSampledImages,limits.maxDescriptorSetUpdateAfterBindSampledImages,3),VK_SHADER_STAGE_ALL,nullptr};
        heap_bindings[2]={2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,count(256,limits.maxPerStageDescriptorUpdateAfterBindStorageBuffers,limits.maxDescriptorSetUpdateAfterBindStorageBuffers,2),VK_SHADER_STAGE_ALL,nullptr};
        heap_bindings[3]={3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,count(15,limits.maxPerStageDescriptorUpdateAfterBindUniformBuffers,limits.maxDescriptorSetUpdateAfterBindUniformBuffers,2),VK_SHADER_STAGE_ALL,nullptr};
        const uint32_t other=heap_bindings[2].descriptorCount+heap_bindings[3].descriptorCount+10;
        if (limits.maxPerStageUpdateAfterBindResources<=other) throw std::runtime_error("indexed graphics push resource limit unavailable");
        heap_bindings[1].descriptorCount=std::min(heap_bindings[1].descriptorCount,limits.maxPerStageUpdateAfterBindResources-other);
        uint32_t descriptors=10;
        for (const auto &binding:heap_bindings) descriptors+=binding.descriptorCount;
        if (descriptors>limits.maxUpdateAfterBindDescriptorsInAllPools)
            throw std::runtime_error("indexed graphics push descriptor pool limit unavailable");
        flags.bindingCount=4; flags.pBindingFlags=heap_flags;
        si.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT; si.pNext=&flags;
        si.bindingCount=4; si.pBindings=heap_bindings;
    }
    nativeprobe::check(create_set(p.device,&si,nullptr,&sets[0]),"graphics push heap layout");
    const auto heap_layout=sets[0]; gpu.objects.push_back([=,&p]{ destroy_set(p.device,heap_layout,nullptr); });
    si.flags=subject ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
    si.pNext=nullptr;
    si.bindingCount=7; si.pBindings=bindings;
    nativeprobe::check(create_set(p.device,&si,nullptr,&sets[1]),"graphics push data layout");
    const auto data_layout=sets[1]; gpu.objects.push_back([=,&p]{ destroy_set(p.device,data_layout,nullptr); });
    VkDescriptorSetLayoutBinding old_binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    si.bindingCount=1; si.pBindings=&old_binding;
    VkDescriptorSetLayout old_set_layout{};
    nativeprobe::check(create_set(p.device,&si,nullptr,&old_set_layout),"graphics previous push set layout");
    gpu.objects.push_back([=,&p]{ destroy_set(p.device,old_set_layout,nullptr); });
    VkPipelineLayout layouts[3]{};
    VkPushConstantRange color{VK_SHADER_STAGE_ALL,0,128};
    auto destroy_layout=p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    for (unsigned i=0;i<3;++i) {
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount=i; ci.pSetLayouts=sets; ci.pushConstantRangeCount=1; ci.pPushConstantRanges=&color;
        nativeprobe::check(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&ci,nullptr,&layouts[i]),"graphics push pipeline layout");
        const auto layout=layouts[i]; gpu.objects.push_back([=,&p]{ destroy_layout(p.device,layout,nullptr); });
    }
    VkPipelineLayout previous_push_layout{};
    VkPipelineLayoutCreateInfo previous{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    previous.setLayoutCount=1; previous.pSetLayouts=&old_set_layout;
    previous.pushConstantRangeCount=1; previous.pPushConstantRanges=&color;
    nativeprobe::check(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&previous,nullptr,&previous_push_layout),"graphics previous push layout");
    gpu.objects.push_back([=,&p]{ destroy_layout(p.device,previous_push_layout,nullptr); });
    VkAttachmentDescription attachment{};
    attachment.format=VK_FORMAT_R8G8B8A8_UNORM; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1; subpass.pColorAttachments=&ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount=rp.subpassCount=1; rp.pAttachments=&attachment; rp.pSubpasses=&subpass;
    VkRenderPass pass{};
    nativeprobe::check(p.dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(p.device,&rp,nullptr,&pass),"graphics push pass");
    auto destroy_pass=p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    gpu.objects.push_back([=,&p]{ destroy_pass(p.device,pass,nullptr); });
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass=pass; fb.attachmentCount=1; fb.pAttachments=&target.view; fb.width=fb.height=16; fb.layers=1;
    VkFramebuffer framebuffer{};
    nativeprobe::check(p.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device,&fb,nullptr,&framebuffer),"graphics push framebuffer");
    auto destroy_framebuffer=p.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
    gpu.objects.push_back([=,&p]{ destroy_framebuffer(p.device,framebuffer,nullptr); });
    VkShaderModule modules[4]{};
    const uint32_t *codes[]{nativeprobe::tiler_vert,nativeprobe::tiler_frag,
        vertex_resources ? nativeprobe::push_vertex_frag : nativeprobe::push_graphics_frag,
        vertex_resources ? nativeprobe::push_vertex_vert : nativeprobe::tiler_vert};
    const size_t sizes[]{sizeof nativeprobe::tiler_vert,sizeof nativeprobe::tiler_frag,
        vertex_resources ? sizeof nativeprobe::push_vertex_frag : sizeof nativeprobe::push_graphics_frag,
        vertex_resources ? sizeof nativeprobe::push_vertex_vert : sizeof nativeprobe::tiler_vert};
    for (unsigned i=0;i<4;++i) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize=sizes[i]; ci.pCode=codes[i];
        nativeprobe::check(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&ci,nullptr,&modules[i]),"graphics push shader");
        auto destroy=p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
        const auto module=modules[i]; gpu.objects.push_back([=,&p]{ destroy(p.device,module,nullptr); });
    }
    VkPipeline pipelines[3]{};
    for (unsigned i=0;i<3;++i) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (unsigned j=0;j<2;++j) {
            stages[j]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[j].stage=j ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[j].module=j ? modules[i==2 ? 2 : 1] : modules[i==2 ? 3 : 0]; stages[j].pName="main";
        }
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,16,16,0,1}; VkRect2D scissor{{0,0},{16,16}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount=vp.scissorCount=1; vp.pViewports=&viewport; vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState write{}; write.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=1; blend.pAttachments=&write;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount=2; ci.pStages=stages; ci.pVertexInputState=&vi; ci.pInputAssemblyState=&ia; ci.pViewportState=&vp;
        ci.pRasterizationState=&rs; ci.pMultisampleState=&ms; ci.pColorBlendState=&blend; ci.layout=layouts[i]; ci.renderPass=pass;
        const auto result=p.dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(p.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipelines[i]);
        auto destroy=p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        const auto pipeline=pipelines[i]; if (pipeline) gpu.objects.push_back([=,&p]{ destroy(p.device,pipeline,nullptr); });
        nativeprobe::check(result,"graphics push pipeline");
    }
    VkDescriptorSet set{},heap_set{},previous_set{};
    {
        VkDescriptorPoolSize pool_sizes[11];
        for (unsigned i=0;i<7;++i) pool_sizes[i]={bindings[i].descriptorType,bindings[i].descriptorCount};
        ++pool_sizes[2].descriptorCount;
        if (indexed_heap) for (unsigned i=0;i<4;++i) pool_sizes[i+7]={heap_bindings[i].descriptorType,heap_bindings[i].descriptorCount};
        else ++pool_sizes[1].descriptorCount;
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; ci.maxSets=3; ci.poolSizeCount=indexed_heap ? 11 : 7; ci.pPoolSizes=pool_sizes;
        if (indexed_heap) ci.flags=VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        VkDescriptorPool pool{};
        nativeprobe::check(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&ci,nullptr,&pool),"graphics push control pool");
        auto destroy=p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
        gpu.objects.push_back([=,&p]{ destroy(p.device,pool,nullptr); });
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=pool; ai.descriptorSetCount=1; ai.pSetLayouts=&sets[0];
        auto allocate=p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
        nativeprobe::check(allocate(p.device,&ai,&heap_set),"graphics push heap set");
        if (!subject) {
            ai.pSetLayouts=&sets[1];
            nativeprobe::check(allocate(p.device,&ai,&set),"graphics push control set");
            ai.pSetLayouts=&old_set_layout;
            nativeprobe::check(allocate(p.device,&ai,&previous_set),"graphics previous push control set");
        }
        VkDescriptorBufferInfo buffer{inputs[0].handle,offset,16};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstBinding=indexed_heap ? 3 : 0;
        write.dstSet=heap_set; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo=&buffer;
        p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,1,&write,0,nullptr);
        if (!subject) {
            write.dstBinding=0; write.dstSet=previous_set; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,1,&write,0,nullptr);
        }
    }
    auto push=subject ? p.dev<PFN_vkCmdPushDescriptorSetKHR>("vkCmdPushDescriptorSetKHR") : nullptr;
    std::puts("VKFIX_PROBE_READY"); std::fflush(stdout);
    bool matched=true;
    for (unsigned trial=0;trial<4;++trial) {
        VkDescriptorBufferInfo buffers[5];
        for (unsigned i=0;i<4;++i) buffers[i]={inputs[(i+trial)%4].handle,offset,16};
        buffers[4]={inputs[(trial+2)%4].handle,offset,16};
        VkDescriptorImageInfo image_infos[]{
            {sampler,images[trial%3].view,VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE,images[1].view,VK_IMAGE_LAYOUT_GENERAL},
            {sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED},
            {VK_NULL_HANDLE,images[2].view,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[7]{};
        for (unsigned i=0;i<7;++i) {
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=set;
            writes[i].dstBinding=bindings[i].binding; writes[i].descriptorCount=bindings[i].descriptorCount; writes[i].descriptorType=bindings[i].descriptorType;
        }
        writes[0].pImageInfo=&image_infos[0]; writes[1].pBufferInfo=&buffers[0]; writes[2].pBufferInfo=&buffers[2];
        writes[3].pImageInfo=&image_infos[1]; writes[4].pImageInfo=&image_infos[2]; writes[5].pImageInfo=&image_infos[3];
        writes[6].pTexelBufferView=&views[trial];
        // The descriptor-set control is updated before recording and is never
        // modified while pending; both arms execute the same live shader data.
        auto partial=writes[1]; partial.descriptorCount=1; partial.dstArrayElement=1; partial.pBufferInfo=&buffers[4];
        if (!subject) {
            auto update=p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
            update(p.device,7,writes,0,nullptr); update(p.device,1,&partial,0,nullptr);
        }
        auto cb=gpu.begin();
        auto barrier=p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        for (unsigned i=0;i<3;++i) {
            VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;
            ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; ib.image=images[i].handle; ib.subresourceRange=range;
            ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
            VkClearColorValue clear{}; clear.float32[i]=clear.float32[3]=1;
            p.dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cb,images[i].handle,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
            ib.oldLayout=VK_IMAGE_LAYOUT_GENERAL; ib.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&ib);
        }
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; ib.image=target.handle; ib.subresourceRange=range;
        ib.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&ib);
        VkClearValue clear{};
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass=pass; bi.framebuffer=framebuffer; bi.renderArea.extent={16,16}; bi.clearValueCount=1; bi.pClearValues=&clear;
        p.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cb,&bi,VK_SUBPASS_CONTENTS_INLINE);
        auto bind=p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
        // Push descriptors need no currently bound pipeline. In particular the
        // first draw of a newly recorded world pass starts with no pipeline at
        // all; binding even an empty pipeline here would mask that native path.
        if (trial) bind(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[trial-1]);
        // Prime the push-descriptor bank with set 0 of a one-set layout, then
        // move the bank to set 1 of a two-set layout. A prior pipeline bind or
        // ordinary-set bind alone does not initialize this native state.
        VkDescriptorBufferInfo prior_buffer{inputs[0].handle,offset,16};
        VkWriteDescriptorSet prior_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        prior_write.dstBinding=0; prior_write.descriptorCount=1;
        prior_write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; prior_write.pBufferInfo=&prior_buffer;
        auto prime=[&] {
            if (subject) push(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,previous_push_layout,0,1,&prior_write);
            else p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,previous_push_layout,0,1,&previous_set,0,nullptr);
        };
        if (!(trial&1)) prime();
        p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layouts[1],0,1,&heap_set,0,nullptr);
        if (trial&1) prime();
        if (subject) {
            // Mixed types, sparse binding numbers, arrays, repeated pushes and
            // partial array replacement while a different layout is bound.
            push(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layouts[2],1,7,writes);
            push(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layouts[2],1,1,&partial);
        } else p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layouts[2],1,1,&set,0,nullptr);
        bind(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[2]);
        p.dev<PFN_vkCmdDraw>("vkCmdDraw")(cb,3,1,0,0);
        p.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cb);
        ib.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ib.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={16,16,1};
        p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cb,target.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
        gpu.host_barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        gpu.submit(cb); gpu.invalidate(output);
        for (unsigned channel=0;channel<4;++channel) {
            float sum=values[trial][channel]; // uniform texel buffer
            for (unsigned i:{0u,2u,2u,3u}) sum+=values[(i+trial)%4][channel];
            sum+=float(channel==trial%3 || channel==3)+float(channel==1 || channel==3)+float(channel==2 || channel==3);
            const int expected=int(std::lround(sum*255.f/8.f));
            for (unsigned pixel=0;pixel<256;++pixel)
                matched &= std::abs(int(static_cast<unsigned char*>(output.mapped)[pixel*4+channel])-expected)<=1;
        }
    }
    return matched;
}

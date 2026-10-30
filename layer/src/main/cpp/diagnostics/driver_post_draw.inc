// Small deterministic offscreen draws. Native drivers use a core render pass;
// dynamic rendering is also exercised when available (required by vertex replay).
struct DrawProbe {
    GpuProbe gpu;
    GpuProbe::Image image;
    GpuProbe::Buffer output;
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};
    VkPipelineLayout layout{};
    bool dynamic;
    DrawProbe(Probe &p, VkPhysicalDevice pd, uint32_t family, bool use_dynamic)
        : gpu(p, pd, family), image(gpu.image()),
          output(gpu.buffer(1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT)), dynamic(use_dynamic) {
        if (dynamic) return;
        auto destroy_rp = p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &ref;
        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1; ci.pAttachments = &attachment; ci.subpassCount = 1; ci.pSubpasses = &subpass;
        require(p.dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(p.device, &ci, nullptr, &render_pass), "create draw render pass");
        const auto rp = render_pass;
        gpu.objects.push_back([=, &p] { destroy_rp(p.device, rp, nullptr); });
        auto destroy_fb = p.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp; fi.attachmentCount = 1; fi.pAttachments = &image.view;
        fi.width = fi.height = 16; fi.layers = 1;
        require(p.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device, &fi, nullptr, &framebuffer), "create draw framebuffer");
        const auto fb = framebuffer;
        gpu.objects.push_back([=, &p] { destroy_fb(p.device, fb, nullptr); });
    }
    void flush(const GpuProbe::Buffer &b) {
        auto &p = gpu.p;
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = b.memory; range.size = VK_WHOLE_SIZE;
        require(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device, 1, &range), "flush draw input");
    }
    VkShaderModule shader(const uint32_t *code, size_t bytes) {
        auto &p = gpu.p;
        auto destroy = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = bytes; ci.pCode = code;
        VkShaderModule module{};
        require(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device, &ci, nullptr, &module), "create draw shader");
        gpu.objects.push_back([=, &p] { destroy(p.device, module, nullptr); });
        return module;
    }
    VkPipeline pipeline(unsigned mode, VkDescriptorSetLayout set = VK_NULL_HANDLE) {
        auto &p = gpu.p;
        VkShaderModule vertex = mode == 5 ? shader(post_parameters_vert, sizeof post_parameters_vert) :
                                mode == 6 ? shader(post_stores_vert, sizeof post_stores_vert) :
                                            shader(post_draw_vert, sizeof post_draw_vert);
        VkShaderModule fragment = mode == 7 ? shader(post_sampler_frag, sizeof post_sampler_frag) :
                                             shader(post_draw_frag, sizeof post_draw_frag);
        auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &push;
        if (set) { li.setLayoutCount = 1; li.pSetLayouts = &set; }
        require(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device, &li, nullptr, &layout), "create draw pipeline layout");
        const auto pipeline_layout = layout;
        gpu.objects.push_back([=, &p] { destroy_layout(p.device, pipeline_layout, nullptr); });
        VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment; stages[1].pName = "main";
        VkVertexInputBindingDescription binding{0, 4, VK_VERTEX_INPUT_RATE_INSTANCE};
        VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (mode != 5 && mode != 6) {
            vi.vertexBindingDescriptionCount = vi.vertexAttributeDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &binding; vi.pVertexAttributeDescriptions = &attribute;
        }
        VkVertexInputBindingDivisorDescriptionEXT divisor{0, 2};
        VkPipelineVertexInputDivisorStateCreateInfoEXT di{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT};
        di.vertexBindingDivisorCount = 1; di.pVertexBindingDivisors = &divisor;
        if (mode == 3) vi.pNext = &di;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, 16, 16, 0, 1}; VkRect2D scissor{{0,0},{16,16}};
        VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vs.viewportCount = vs.scissorCount = 1; vs.pViewports = &viewport; vs.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = mode == 4 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color{}; color.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &color;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2; ci.pStages = stages; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vs; ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &blend;
        ci.layout = layout; ci.renderPass = render_pass;
        if (dynamic) ci.pNext = &rendering;
        auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        VkPipeline pipeline{};
        auto result = p.dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(p.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
        if (pipeline) gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });
        require(result, "create draw pipeline");
        return pipeline;
    }
    VkCommandBuffer begin(VkPipeline pipeline, uint32_t mode) {
        auto &p = gpu.p;
        auto cb = gpu.begin();
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ib.image = image.handle; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        VkClearValue clear{}; clear.color.float32[3] = 1;
        if (dynamic) {
            VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            attachment.imageView = image.view; attachment.imageLayout = ib.newLayout;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; attachment.clearValue = clear;
            VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO}; info.renderArea.extent = {16,16}; info.layerCount = 1;
            info.colorAttachmentCount = 1; info.pColorAttachments = &attachment;
            p.dev<PFN_vkCmdBeginRenderingKHR>("vkCmdBeginRenderingKHR")(cb, &info);
        } else {
            VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; info.renderPass = render_pass; info.framebuffer = framebuffer;
            info.renderArea.extent = {16,16}; info.clearValueCount = 1; info.pClearValues = &clear;
            p.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cb, &info, VK_SUBPASS_CONTENTS_INLINE);
        }
        p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        p.dev<PFN_vkCmdPushConstants>("vkCmdPushConstants")(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &mode);
        return cb;
    }
    void finish(VkCommandBuffer cb) {
        auto &p = gpu.p;
        if (dynamic) p.dev<PFN_vkCmdEndRenderingKHR>("vkCmdEndRenderingKHR")(cb);
        else p.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cb);
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = image.handle; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {16,16,1};
        p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cb, image.handle, ib.newLayout, output.handle, 1, &copy);
        gpu.host_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT);
        gpu.submit(cb); gpu.invalidate(output);
    }
    bool pixel(unsigned x, unsigned y, unsigned red, unsigned green, unsigned blue) {
        auto bytes = static_cast<const unsigned char *>(output.mapped) + (y*16+x)*4;
        return std::abs(int(bytes[0])-int(red)) <= 1 && std::abs(int(bytes[1])-int(green)) <= 1 &&
               std::abs(int(bytes[2])-int(blue)) <= 1 && bytes[3] == 255;
    }
};

void draw_smoke(Probe &p, VkPhysicalDevice pd, uint32_t family, bool dynamic, unsigned mode, bool multi_parameters = true) {
    DrawProbe draw(p, pd, family, dynamic);
    auto pipeline = draw.pipeline(mode);
    auto vertex = draw.gpu.buffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    float values[16]{0.25f, 0.75f, 0.125f, 0.5f};
    std::memcpy(vertex.mapped, values, sizeof values); draw.flush(vertex);
    VkDrawIndirectCommand commands[2]{{6,1,0,0},{6,1,6,0}};
    if (mode == 2) commands[0].firstInstance = 7;
    if (mode == 5) { commands[0].firstInstance = 3; commands[1].firstInstance = 7; }
    auto indirect = draw.gpu.buffer(sizeof commands, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    std::memcpy(indirect.mapped, commands, sizeof commands); draw.flush(indirect);
    auto cb = draw.begin(pipeline, mode == 5 ? (multi_parameters ? 1 : 0) : mode);
    VkDeviceSize offset = 0;
    p.dev<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")(cb, 0, 1, &vertex.handle, &offset);
    if (mode == 1 || mode == 2 || (mode == 5 && multi_parameters))
        p.dev<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect")(cb, indirect.handle, 0, mode == 2 ? 1 : 2, sizeof(VkDrawIndirectCommand));
    else p.dev<PFN_vkCmdDraw>("vkCmdDraw")(cb, mode == 4 ? 3 : 6, mode == 3 ? 4 : 1, 0, mode == 5 ? 7 : 0);
    draw.finish(cb);
    bool correct = false;
    if (mode == 1) correct = draw.pixel(4,8,255,0,0) && draw.pixel(12,8,0,255,0);
    if (mode == 2 || mode == 5 || mode == 0) correct = draw.pixel(4,8,0,255,0) && draw.pixel(12,8,0,255,0);
    if (mode == 3) correct = draw.pixel(4,8,64,0,0) && draw.pixel(12,8,191,0,0);
    if (mode == 4) {
        unsigned green = 0;
        for (unsigned y=0; y<16; ++y) for (unsigned x=0; x<16; ++x) green += draw.pixel(x,y,0,255,0);
        correct = green > 5 && green < 90 && draw.pixel(8,8,0,0,0);
    }
    const char *names[]{"graphics.draw", "multiDrawIndirect.draw", "drawIndirectFirstInstance.draw", "vertexAttributeInstanceRateDivisor.draw", "fillModeNonSolid.line", "shaderDrawParameters.draw"};
    if (!correct) throw ProbeMismatch(std::string(names[mode]) + " pixel readback mismatch");
    row(names[mode], "PASS", mode == 5 && !multi_parameters ?
        "direct nonzero BaseInstance/InstanceIndex and DrawID zero pixel check passed" :
        "offscreen draw pixel checks passed", "functional");
}

void vertex_stores_smoke(Probe &p, VkPhysicalDevice pd, uint32_t family, bool dynamic) {
    DrawProbe draw(p, pd, family, dynamic);
    auto output = draw.gpu.buffer(20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(output.mapped, 0, 20); draw.flush(output);
    auto destroy_set = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; si.bindingCount=1; si.pBindings=&binding;
    VkDescriptorSetLayout set{};
    require(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device,&si,nullptr,&set), "create vertex storage layout");
    draw.gpu.objects.push_back([=,&p]{ destroy_set(p.device,set,nullptr); });
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=1; pi.poolSizeCount=1; pi.pPoolSizes=&size;
    VkDescriptorPool pool{};
    require(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&pi,nullptr,&pool), "create vertex storage descriptor pool");
    draw.gpu.objects.push_back([=,&p]{ destroy_pool(p.device,pool,nullptr); });
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=pool; ai.descriptorSetCount=1; ai.pSetLayouts=&set;
    VkDescriptorSet descriptor{};
    require(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device,&ai,&descriptor), "allocate vertex storage descriptor");
    VkDescriptorBufferInfo buffer{output.handle,0,20};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=descriptor; write.descriptorCount=1;
    write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&buffer;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,1,&write,0,nullptr);
    auto pipeline = draw.pipeline(6,set);
    auto cb = draw.begin(pipeline,0);
    p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,draw.layout,0,1,&descriptor,0,nullptr);
    p.dev<PFN_vkCmdDraw>("vkCmdDraw")(cb,3,1,0,0);
    draw.finish(cb); draw.gpu.invalidate(output);
    const uint32_t expected[]{3,6,17,18,19};
    if (std::memcmp(output.mapped,expected,sizeof expected) || !draw.pixel(8,8,0,255,0))
        throw ProbeMismatch("vertex storage/atomic or pixel readback mismatch");
    row("vertexPipelineStoresAndAtomics.draw", "PASS", "vertex SSBO stores and atomic count/sum matched 3,6,17,18,19; fragment output green", "functional");
}

void sampler_smoke(Probe &p, VkPhysicalDevice pd, uint32_t family, bool dynamic, float max_anisotropy) {
    DrawProbe draw(p, pd, family, dynamic);
    auto texture = draw.gpu.image(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    auto source = draw.gpu.buffer(1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto bytes = static_cast<unsigned char *>(source.mapped);
    for (unsigned i=0; i<256; ++i) {
        bytes[i*4]=32; bytes[i*4+1]=128; bytes[i*4+2]=224; bytes[i*4+3]=255;
    }
    draw.flush(source);
    auto upload = draw.gpu.begin();
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    ib.image=texture.handle; ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    auto barrier = p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    barrier(upload,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={16,16,1};
    p.dev<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(upload,source.handle,texture.handle,ib.newLayout,1,&copy);
    ib.oldLayout=ib.newLayout; ib.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    barrier(upload,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&ib);
    draw.gpu.submit(upload);
    auto destroy_sampler = p.dev<PFN_vkDestroySampler>("vkDestroySampler");
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter=sci.minFilter=VK_FILTER_LINEAR; sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.anisotropyEnable=VK_TRUE; sci.maxAnisotropy=std::min(4.f,max_anisotropy);
    VkSampler sampler{};
    require(p.dev<PFN_vkCreateSampler>("vkCreateSampler")(p.device,&sci,nullptr,&sampler), "create anisotropic sampler");
    draw.gpu.objects.push_back([=,&p]{ destroy_sampler(p.device,sampler,nullptr); });
    auto destroy_set = p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; si.bindingCount=1; si.pBindings=&binding;
    VkDescriptorSetLayout set{};
    require(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device,&si,nullptr,&set), "create sampler layout");
    draw.gpu.objects.push_back([=,&p]{ destroy_set(p.device,set,nullptr); });
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=1; pi.poolSizeCount=1; pi.pPoolSizes=&size;
    VkDescriptorPool pool{};
    require(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&pi,nullptr,&pool), "create sampler descriptor pool");
    draw.gpu.objects.push_back([=,&p]{ destroy_pool(p.device,pool,nullptr); });
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=pool; ai.descriptorSetCount=1; ai.pSetLayouts=&set;
    VkDescriptorSet descriptor{};
    require(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device,&ai,&descriptor), "allocate sampler descriptor");
    VkDescriptorImageInfo image{sampler,texture.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=descriptor; write.descriptorCount=1;
    write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo=&image;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,1,&write,0,nullptr);
    auto pipeline = draw.pipeline(7,set);
    auto vertex = draw.gpu.buffer(64,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto cb = draw.begin(pipeline,0);
    VkDeviceSize offset=0;
    p.dev<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")(cb,0,1,&vertex.handle,&offset);
    p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,draw.layout,0,1,&descriptor,0,nullptr);
    p.dev<PFN_vkCmdDraw>("vkCmdDraw")(cb,6,1,0,0);
    draw.finish(cb);
    if (!draw.pixel(4,8,32,128,224) || !draw.pixel(12,8,32,128,224))
        throw ProbeMismatch("anisotropy-enabled sampler textureGrad pixel readback mismatch");
    row("samplerAnisotropy.sample", "PASS", "anisotropy-enabled sampler textureGrad readback matched uploaded texels; not a filtering-quality benchmark", "functional");
}

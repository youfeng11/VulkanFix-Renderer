// Included inside the POST implementation's private namespace.
// Resource lifetime is scoped to a disposable probe process and bounded fence.
#include "driver_post_shaders.h"
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

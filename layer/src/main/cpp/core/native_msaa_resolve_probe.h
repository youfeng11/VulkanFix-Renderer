#pragma once
#include "driver_trust.h"
#include "native_probe_resources.h"

namespace vkfix::nativeprobe {
// No shaders, extensions, renderer interception or application queues. The
// controls verify both the requested resolve and the retained multisample data.
template<class Context> DriverProbeAssessment probe_msaa_array_resolve(
    Context& p, VkPhysicalDevice physical, uint32_t family) {
    constexpr auto format = VK_FORMAT_R32G32B32A32_SFLOAT;
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties supported{};
    const auto query = p.template get<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
    if (query(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported) != VK_SUCCESS ||
        supported.maxArrayLayers < 8 || supported.maxExtent.width < 16 || supported.maxExtent.height < 16 ||
        (supported.sampleCounts & (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)) != (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT))
        return {DriverProbeVerdict::Inconclusive, "RGBA32F 4xMSAA array resolve control unsupported"};
    struct Readback { bool target, retained; };
    auto run = [&](unsigned mode, uint32_t layers = 8,
                   VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE) -> Readback {
        // 0 transfer; 1 attachment; 2 attachment then transfer repair.
        const uint32_t selected_layer = layers > 1 ? 3 : 0;
        Resources<Context> gpu(p, physical, family);
        const auto create_image = p.template dev<PFN_vkCreateImage>("vkCreateImage");
        const auto destroy_image = p.template dev<PFN_vkDestroyImage>("vkDestroyImage");
        const auto create_view = p.template dev<PFN_vkCreateImageView>("vkCreateImageView");
        const auto destroy_view = p.template dev<PFN_vkDestroyImageView>("vkDestroyImageView");
        VkImage images[3]{}; VkImageView views[3]{};
        for (unsigned i = 0; i < 3; ++i) {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = format; ci.extent = {16,16,1}; ci.mipLevels = 1; ci.arrayLayers = layers;
            ci.samples = i ? VK_SAMPLE_COUNT_1_BIT : VK_SAMPLE_COUNT_4_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
            check(create_image(p.device, &ci, nullptr, &images[i]), "MSAA probe image");
            const auto image = images[i]; gpu.objects.push_back([=,&p] { destroy_image(p.device, image, nullptr); });
            VkMemoryRequirements mr{};
            p.template dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(p.device, image, &mr);
            const auto memory = gpu.allocate(mr, 0);
            check(p.template dev<PFN_vkBindImageMemory>("vkBindImageMemory")(p.device, image, memory, 0), "MSAA probe memory");
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,selected_layer,1};
            check(create_view(p.device, &vi, nullptr, &views[i]), "MSAA probe view");
            const auto view = views[i]; gpu.objects.push_back([=,&p] { destroy_view(p.device, view, nullptr); });
        }
        VkAttachmentDescription attachments[2]{};
        for (auto& a : attachments) {
            a.format = format; a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        attachments[0].samples = VK_SAMPLE_COUNT_4_BIT; attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = store;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, resolve{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
        subpass.pResolveAttachments = mode ? &resolve : nullptr;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpci.attachmentCount = 2;
        rpci.pAttachments = attachments; rpci.subpassCount = 1; rpci.pSubpasses = &subpass;
        VkRenderPass pass{};
        check(p.template dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(p.device, &rpci, nullptr, &pass), "MSAA probe pass");
        const auto destroy_pass = p.template dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
        gpu.objects.push_back([=,&p] { destroy_pass(p.device, pass, nullptr); });
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fci.renderPass = pass;
        fci.attachmentCount = 2; fci.pAttachments = views; fci.width = fci.height = 16; fci.layers = 1;
        VkFramebuffer fb{};
        check(p.template dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device, &fci, nullptr, &fb), "MSAA probe framebuffer");
        const auto destroy_fb = p.template dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
        gpu.objects.push_back([=,&p] { destroy_fb(p.device, fb, nullptr); });
        constexpr VkDeviceSize bytes = 16 * 16 * 4 * sizeof(float);
        auto output = gpu.buffer(bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto cmd = gpu.begin();
        const auto barrier = p.template dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        auto transition = [&](VkImage image, VkImageLayout from, VkImageLayout to, uint32_t first, uint32_t count = 1) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = image;
            b.oldLayout = from; b.newLayout = to; b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,first,count};
            barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
        };
        transition(images[0],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,layers);
        VkClearColorValue zero{}; VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers};
        p.template dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&all);
        transition(images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,layers);
        transition(images[1],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,selected_layer);
        VkClearValue red{}; red.color.float32[0] = red.color.float32[3] = 1;
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = pass;
        begin.framebuffer = fb; begin.renderArea = {{0,0},{16,16}}; begin.clearValueCount = 1; begin.pClearValues = &red;
        p.template dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
        p.template dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cmd);
        VkImageResolve region{}; region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,selected_layer,1}; region.extent = {16,16,1};
        const auto transfer = p.template dev<PFN_vkCmdResolveImage>("vkCmdResolveImage");
        if (mode != 1) {
            transition(images[0],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,selected_layer);
            transition(images[1],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,selected_layer);
            transfer(cmd,images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            transition(images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,selected_layer);
        } else transition(images[1],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,selected_layer);
        const auto copy = p.template dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
        VkBufferImageCopy copy_region{}; copy_region.imageSubresource = region.dstSubresource; copy_region.imageExtent = region.extent;
        // Observe the attachment result before touching its source again. A
        // later source transfer can otherwise hide the defect from this read.
        copy(cmd,images[1],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy_region);
        if (store == VK_ATTACHMENT_STORE_OP_STORE) {
            if (mode == 1) transition(images[0],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,selected_layer);
            transition(images[2],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,selected_layer);
            transfer(cmd,images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[2],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            transition(images[2],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,selected_layer);
            copy_region.bufferOffset = bytes; copy(cmd,images[2],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy_region);
        }
        gpu.host_barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT); gpu.submit(cmd); gpu.invalidate(output);
        const auto* data = static_cast<const float*>(output.mapped);
        auto correct = [&](unsigned slot) {
            for (unsigned pixel = 0; pixel < 256; ++pixel) for (unsigned c = 0; c < 4; ++c)
                if (data[slot * 1024 + pixel * 4 + c] != ((c == 0 || c == 3) ? 1.f : 0.f)) return false;
            return true;
        };
        return {correct(0),store == VK_ATTACHMENT_STORE_OP_DONT_CARE || correct(1)};
    };
    // Measure the untouched attachment path before warming any explicit
    // transfer-resolve implementation on this native device.
    const auto prime = run(1,1,VK_ATTACHMENT_STORE_OP_DONT_CARE);
    const auto subject = run(1), before = run(0), after = run(2);
    if (!prime.target || !before.target || !before.retained || !subject.retained || !after.target || !after.retained)
        return {DriverProbeVerdict::Inconclusive,"MSAA array resolve/source preservation control failed"};
    return {subject.target ? DriverProbeVerdict::Healthy : DriverProbeVerdict::RepairConfirmed,
        subject.target ? "RGBA32F MSAA STORE and array resolve match transfer controls" :
            "RGBA32F MSAA STORE array resolve mismatches; explicit transfer preserves source and target"};
}
} // namespace vkfix::nativeprobe

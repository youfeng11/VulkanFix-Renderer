#pragma once
#include "../core/driver_trust_shaders.h"
#include "../core/native_probe_resources.h"
#include <array>

namespace vkfix::nativeprobe {
struct DepthResolveReadback {
    std::string detail;
    unsigned formats = 0;
    bool passed = true;
    bool target_unwritten = true;
};

namespace depth_resolve {
constexpr VkExtent2D extent{4, 4};
constexpr unsigned pixels = extent.width * extent.height;
constexpr VkClearDepthStencilValue source_value{0.25f, 0x59};
constexpr VkClearDepthStencilValue sentinel{0.75f, 0xa7};
constexpr auto depth = VK_IMAGE_ASPECT_DEPTH_BIT;
constexpr auto stencil = VK_IMAGE_ASPECT_STENCIL_BIT;
constexpr auto attachment_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
constexpr auto sampled_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
constexpr VkPipelineStageFlags tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags attachment_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
constexpr VkImageUsageFlags source_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags target_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

struct Format {
    VkFormat format;
    const char *name;
    VkImageAspectFlags aspects;
    uint32_t depth_bits;
};
constexpr Format formats[]{
    {VK_FORMAT_D16_UNORM, "D16", depth, 16},
    {VK_FORMAT_X8_D24_UNORM_PACK32, "D24", depth, 24},
    {VK_FORMAT_D32_SFLOAT, "D32F", depth, 32},
    {VK_FORMAT_D24_UNORM_S8_UINT, "D24S8", depth | stencil, 24},
    {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32FS8", depth | stencil, 32},
    {VK_FORMAT_S8_UINT, "S8", stencil, 0},
};

template<class Context>
VkRenderPass create_pass(Resources<Context> &gpu, PFN_vkCreateRenderPass2 create, const Format &format, bool resolve) {
    auto &p = gpu.p;
    VkAttachmentDescription2 attachments[2]{};
    const uint32_t count = resolve ? 2 : 1;
    for (uint32_t index = 0; index < count; ++index) {
        auto &attachment = attachments[index];
        attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachment.format = format.format;
        attachment.samples = resolve && index == 0 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (resolve && index == 1) attachment.loadOp = attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.initialLayout = attachment.finalLayout = attachment_layout;
    }
    VkAttachmentReference2 source{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    source.attachment = 0;
    source.layout = attachment_layout;
    source.aspectMask = format.aspects;
    auto target = source;
    target.attachment = 1;
    VkSubpassDescriptionDepthStencilResolve resolve_info{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    resolve_info.depthResolveMode = format.aspects & depth ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
    resolve_info.stencilResolveMode = format.aspects & stencil ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
    resolve_info.pDepthStencilResolveAttachment = &target;
    VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    subpass.pNext = resolve ? &resolve_info : nullptr;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &source;
    VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass{};
    auto destroy = p.template dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    check(create(p.device, &info, nullptr, &pass), "depth/stencil probe render pass");
    gpu.objects.push_back([=, &p] { destroy(p.device, pass, nullptr); });
    return pass;
}

struct ShaderPass {
    VkPipelineLayout layout{};
    struct Plane {
        VkPipeline pipeline{};
        VkDescriptorSet set{};
    };
    std::array<Plane, 2> planes{};
};

template<class Context>
ShaderPass create_shader_pass(Resources<Context> &gpu, const Format &format, VkImage source, VkRenderPass pass) {
    auto &p = gpu.p;
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                              VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    VkDescriptorSetLayout set_layout{};
    auto destroy_set_layout = p.template dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    check(p.template dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
        p.device, &set_info, nullptr, &set_layout), "depth/stencil control descriptor layout");
    gpu.objects.push_back([=, &p] { destroy_set_layout(p.device, set_layout, nullptr); });
    const VkPushConstantRange compare{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &compare;
    ShaderPass result;
    auto destroy_layout = p.template dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    check(p.template dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
        p.device, &layout_info, nullptr, &result.layout), "depth/stencil control pipeline layout");
    gpu.objects.push_back([layout = result.layout, destroy_layout, &p] { destroy_layout(p.device, layout, nullptr); });
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.maxLod = 0;
    VkSampler sampler{};
    auto destroy_sampler = p.template dev<PFN_vkDestroySampler>("vkDestroySampler");
    check(p.template dev<PFN_vkCreateSampler>("vkCreateSampler")(p.device, &sampler_info, nullptr, &sampler),
          "depth/stencil control sampler");
    gpu.objects.push_back([=, &p] { destroy_sampler(p.device, sampler, nullptr); });
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    VkDescriptorPool pool{};
    auto destroy_pool = p.template dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    check(p.template dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device, &pool_info, nullptr, &pool),
          "depth/stencil control descriptor pool");
    gpu.objects.push_back([=, &p] { destroy_pool(p.device, pool, nullptr); });

    VkPipelineShaderStageCreateInfo stages[2]{gpu.shader_stage(VK_SHADER_STAGE_VERTEX_BIT, tiler_vert, sizeof tiler_vert)};
    const VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    VkPipelineViewportStateCreateInfo viewport_info{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_info.viewportCount = viewport_info.scissorCount = 1;
    viewport_info.pViewports = &viewport;
    viewport_info.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.front = ds.back = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP,
        VK_COMPARE_OP_ALWAYS, 0xff, 0xff, source_value.stencil};
    const VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_info;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blend;
    info.layout = result.layout;
    info.renderPass = pass;
    for (unsigned index = 0; index < result.planes.size(); ++index) {
        const auto aspect = index == 0 ? depth : stencil;
        if (!(format.aspects & aspect)) continue;
        auto &plane = result.planes[index];
        const auto sample_view = gpu.view(source, format.format, aspect);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &set_layout;
        check(p.template dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(
            p.device, &allocate, &plane.set), "depth/stencil control descriptor");
        const VkDescriptorImageInfo image{sampler, sample_view, sampled_layout};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = plane.set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        p.template dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 1, &write, 0, nullptr);
        const bool depth_plane = aspect == depth;
        ds.depthTestEnable = ds.depthWriteEnable = depth_plane;
        ds.stencilTestEnable = !depth_plane;
        stages[1] = gpu.shader_stage(VK_SHADER_STAGE_FRAGMENT_BIT,
            depth_plane ? depth_resolve_depth : depth_resolve_stencil,
            depth_plane ? sizeof depth_resolve_depth : sizeof depth_resolve_stencil);
        auto destroy = p.template dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        auto status = p.template dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(
            p.device, VK_NULL_HANDLE, 1, &info, nullptr, &plane.pipeline);
        if (plane.pipeline)
            gpu.objects.push_back([pipeline = plane.pipeline, destroy, &p] { destroy(p.device, pipeline, nullptr); });
        check(status, "depth/stencil control graphics pipeline");
    }
    return result;
}

struct PlaneReading { unsigned correct = 0, untouched = 0; };
inline PlaneReading read_plane(const void *data, uint32_t bits) {
    PlaneReading result;
    for (unsigned i = 0; i < pixels; ++i) {
        if (bits == 8) {
            const auto value = static_cast<const uint8_t *>(data)[i];
            result.correct += value == source_value.stencil;
            result.untouched += value == sentinel.stencil;
        } else if (bits == 32) {
            const auto value = static_cast<const float *>(data)[i];
            result.correct += value == source_value.depth;
            result.untouched += value == sentinel.depth;
        } else {
            const auto mask = (1u << bits) - 1u;
            const auto value = bits == 16 ? static_cast<const uint16_t *>(data)[i] :
                                           static_cast<const uint32_t *>(data)[i] & mask;
            const auto near = [=](float expected) {
                const auto quantized = static_cast<uint32_t>(expected * mask);
                return value >= quantized - 1 && value <= quantized + 1;
            };
            result.correct += near(source_value.depth);
            result.untouched += near(sentinel.depth);
        }
    }
    return result;
}
} // namespace depth_resolve

template<class Context, class Ready>
DepthResolveReadback probe_depth_resolve(Context &p, VkPhysicalDevice physical, uint32_t family,
    bool core, bool subject, const VkPhysicalDeviceDepthStencilResolveProperties &modes, Ready ready) {
    using namespace depth_resolve;
    const auto create = p.template dev<PFN_vkCreateRenderPass2>(core ? "vkCreateRenderPass2" : "vkCreateRenderPass2KHR");
    auto query = p.template get<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
    auto supported = [&](VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples) {
        VkImageFormatProperties info{};
        const auto status = query(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &info);
        if (status == VK_ERROR_FORMAT_NOT_SUPPORTED) return false;
        check(status, "depth/stencil probe image format");
        return (info.sampleCounts & samples) && info.maxExtent.width >= extent.width && info.maxExtent.height >= extent.height;
    };
    auto barrier = p.template dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    auto clear = p.template dev<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage");
    auto begin = p.template dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    auto end = p.template dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    auto copy = p.template dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    DepthResolveReadback result;
    for (const auto &format : formats) {
        if ((format.aspects & depth && !(modes.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) ||
            (format.aspects & stencil && !(modes.supportedStencilResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) ||
            !supported(format.format, source_usage, VK_SAMPLE_COUNT_4_BIT) ||
            !supported(format.format, target_usage, VK_SAMPLE_COUNT_1_BIT)) continue;
        Resources<Context> gpu(p, physical, family);
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format.format;
        image.extent = {extent.width, extent.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_4_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = source_usage;
        const auto source = gpu.create_image(image);
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.usage = target_usage;
        const auto target = gpu.create_image(image);
        const auto target_view = gpu.view(target, format.format, format.aspects);
        VkImageView views[2]{subject ? gpu.view(source, format.format, format.aspects) : target_view, target_view};
        const auto pass = create_pass(gpu, create, format, subject);
        const auto framebuffer = gpu.framebuffer(pass, subject ? 2 : 1, views, extent);
        ShaderPass control;
        if (!subject) control = create_shader_pass(gpu, format, source, pass);
        const auto cmd = gpu.begin();
        const VkImageSubresourceRange all{format.aspects, 0, 1, 0, 1};
        auto transition = [&](VkImage handle, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage,
                              VkPipelineStageFlags dst_stage, VkAccessFlags src_access, VkAccessFlags dst_access) {
            VkImageMemoryBarrier change{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            change.oldLayout = from;
            change.newLayout = to;
            change.srcAccessMask = src_access;
            change.dstAccessMask = dst_access;
            change.srcQueueFamilyIndex = change.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            change.image = handle;
            change.subresourceRange = all;
            barrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &change);
        };
        constexpr auto upload = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        constexpr auto download = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        constexpr auto transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        constexpr auto upload_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        for (auto handle : {source, target}) transition(handle, VK_IMAGE_LAYOUT_UNDEFINED, upload,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, transfer, 0, upload_access);
        clear(cmd, source, upload, &source_value, 1, &all);
        clear(cmd, target, upload, &sentinel, 1, &all);
        transition(source, upload, subject ? attachment_layout : sampled_layout, transfer,
            subject ? tests : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, upload_access,
            subject ? attachment_access : VK_ACCESS_SHADER_READ_BIT);
        transition(target, upload, attachment_layout, transfer, tests, upload_access, attachment_access);
        if (!result.formats) ready();
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render.renderPass = pass;
        render.framebuffer = framebuffer;
        render.renderArea.extent = extent;
        begin(cmd, &render, VK_SUBPASS_CONTENTS_INLINE);
        if (!subject) {
            auto bind = p.template dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
            auto bind_set = p.template dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
            auto draw = p.template dev<PFN_vkCmdDraw>("vkCmdDraw");
            for (const auto &plane : control.planes) {
                if (!plane.pipeline) continue;
                bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, plane.pipeline);
                bind_set(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, control.layout, 0, 1, &plane.set, 0, nullptr);
                p.template dev<PFN_vkCmdPushConstants>("vkCmdPushConstants")(
                    cmd, control.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t), &source_value.stencil);
                draw(cmd, 3, 1, 0, 0);
            }
        }
        end(cmd);
        transition(target, attachment_layout, download, tests | transfer, transfer,
            attachment_access | upload_access, VK_ACCESS_TRANSFER_READ_BIT);
        std::array<typename Resources<Context>::Buffer, 2> outputs{};
        for (unsigned index = 0; index < outputs.size(); ++index) {
            const auto aspect = index == 0 ? depth : stencil;
            if (!(format.aspects & aspect)) continue;
            const unsigned bytes = aspect == stencil ? 1u : format.depth_bits == 16 ? 2u : 4u;
            outputs[index] = gpu.buffer(pixels * bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {aspect, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            copy(cmd, target, download, outputs[index].handle, 1, &region);
        }
        gpu.host_barrier(cmd, transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
        gpu.submit(cmd);
        if (result.formats++) result.detail += "; ";
        result.detail += format.name;
        for (unsigned index = 0; index < outputs.size(); ++index) {
            if (!outputs[index].handle) continue;
            gpu.invalidate(outputs[index]);
            const auto read = read_plane(outputs[index].mapped, index == 0 ? format.depth_bits : 8);
            result.detail += index == 0 ? " depth " : " stencil ";
            result.detail += std::to_string(read.correct) + "/" + std::to_string(pixels);
            if (read.correct != pixels) {
                result.detail += " (" + std::to_string(read.untouched) + " untouched)";
                result.passed = false;
                result.target_unwritten &= read.untouched == pixels;
            }
        }
    }
    return result;
}
} // namespace vkfix::nativeprobe

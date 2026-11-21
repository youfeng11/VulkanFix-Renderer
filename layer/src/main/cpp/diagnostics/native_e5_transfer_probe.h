#pragma once
#include "../core/driver_trust_shaders.h"
#include "../core/native_probe_resources.h"
#include "e5_probe_pattern.h"
#include <array>

namespace vkfix::nativeprobe {
enum class E5ReadRoute { Buffer, Image, IntegerView };
struct E5Readback {
    std::string detail;
    bool ran = false;
    bool passed = false;
    bool canonicalized = false;
};

template<class Context>
void copy_e5_integer_view(Resources<Context> &gpu, VkCommandBuffer cmd, VkImage source, VkImage target, uint32_t extent) {
    auto &p = gpu.p;
    const auto view = gpu.view(source, VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT);
    const auto output = gpu.view(target, VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler{};
    auto destroy_sampler = p.template dev<PFN_vkDestroySampler>("vkDestroySampler");
    check(p.template dev<PFN_vkCreateSampler>("vkCreateSampler")(p.device, &sampler_info, nullptr, &sampler),
          "RGB9_E5 integer sampler");
    gpu.objects.push_back([=, &p] { destroy_sampler(p.device, sampler, nullptr); });

    const VkDescriptorSetLayoutBinding bindings[]{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
    };
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    VkDescriptorSetLayout set_layout{};
    auto destroy_set_layout = p.template dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    check(p.template dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
        p.device, &set_info, nullptr, &set_layout), "RGB9_E5 integer set layout");
    gpu.objects.push_back([=, &p] { destroy_set_layout(p.device, set_layout, nullptr); });
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    auto destroy_pool = p.template dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    check(p.template dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device, &pool_info, nullptr, &pool),
          "RGB9_E5 integer descriptor pool");
    gpu.objects.push_back([=, &p] { destroy_pool(p.device, pool, nullptr); });
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout;
    VkDescriptorSet set{};
    check(p.template dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device, &allocate, &set),
          "RGB9_E5 integer descriptor set");
    const VkDescriptorImageInfo images[]{
        {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL}
    };
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType;
        writes[i].pImageInfo = &images[i];
    }
    p.template dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 2, writes, 0, nullptr);
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout layout{};
    auto destroy_layout = p.template dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    check(p.template dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device, &layout_info, nullptr, &layout),
          "RGB9_E5 integer pipeline layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, layout, nullptr); });
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.layout = layout;
    pipeline_info.stage = gpu.shader_stage(VK_SHADER_STAGE_COMPUTE_BIT, e5_integer_copy, sizeof e5_integer_copy);
    VkPipeline pipeline{};
    auto destroy_pipeline = p.template dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    const auto status = p.template dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
        p.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
    if (pipeline) gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });
    check(status, "RGB9_E5 integer compute pipeline");
    p.template dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    p.template dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    p.template dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cmd, (extent + 7) / 8, (extent + 7) / 8, 1);
}

template<class Context, class Ready>
E5Readback probe_e5_transfer(Context &p, VkPhysicalDevice physical, uint32_t family, E5ReadRoute route, Ready ready) {
    constexpr auto e5 = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    constexpr auto integer = VK_FORMAT_R32_UINT;
    constexpr auto transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    constexpr auto compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    constexpr auto upload = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    constexpr auto download = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    constexpr auto sampled = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr auto storage = VK_IMAGE_LAYOUT_GENERAL;
    const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    constexpr VkImageUsageFlags transfers = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatProperties e5_features{}, integer_features{};
    auto features = p.template get<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
    features(physical, e5, &e5_features);
    features(physical, integer, &integer_features);
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if ((integer_features.optimalTilingFeatures & required) != required)
        return {"R32_UINT sampled/storage image unsupported"};
    const auto shared_features = e5_features.optimalTilingFeatures & integer_features.optimalTilingFeatures;
    const VkImageUsageFlags e5_usage = transfers | VK_IMAGE_USAGE_SAMPLED_BIT |
        (shared_features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    constexpr VkImageUsageFlags integer_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const auto query = p.template get<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
    const auto supported = [&](VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags) {
        VkImageFormatProperties limits{};
        const auto status = query(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags, &limits);
        if (status == VK_ERROR_FORMAT_NOT_SUPPORTED) return false;
        check(status, "RGB9_E5 probe image format");
        return limits.maxExtent.width >= 30 && limits.maxExtent.height >= 30 &&
               (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT);
    };
    if (!supported(e5, e5_usage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) || !supported(integer, integer_usage, 0))
        return {"required mutable RGB9_E5 or R32_UINT image unsupported"};

    auto barrier = p.template dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    auto to_image = p.template dev<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    auto copy = p.template dev<PFN_vkCmdCopyImage>("vkCmdCopyImage");
    auto to_buffer = p.template dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    E5Readback result;
    unsigned large_wrong = 0, large_canonical = 0, small_wrong = 0;
    for (uint32_t extent : {30u, 16u}) {
        Resources<Context> gpu(p, physical, family);
        const auto make_image = [&](VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.flags = flags;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = {extent, extent, 1};
            info.mipLevels = info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            return gpu.create_image(info);
        };
        const auto source = make_image(e5, e5_usage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
        const auto intermediate = route == E5ReadRoute::Image ? make_image(e5, e5_usage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) : VK_NULL_HANDLE;
        const auto target = route == E5ReadRoute::Buffer ? source : make_image(integer, integer_usage, 0);
        const uint32_t pixels = extent * extent;
        const auto input = gpu.buffer(pixels * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        const auto output = gpu.buffer(pixels * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for (uint32_t i = 0; i < pixels; ++i) static_cast<uint32_t *>(input.mapped)[i] = e5_probe_word(i);
        VkMappedMemoryRange memory{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        memory.memory = input.memory;
        memory.size = VK_WHOLE_SIZE;
        check(p.template dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device, 1, &memory),
              "flush RGB9_E5 upload");
        const auto cmd = gpu.begin();
        const auto transition = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags before,
                                    VkPipelineStageFlags after, VkAccessFlags reads, VkAccessFlags writes) {
            VkImageMemoryBarrier change{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            change.oldLayout = from;
            change.newLayout = to;
            change.srcAccessMask = reads;
            change.dstAccessMask = writes;
            change.srcQueueFamilyIndex = change.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            change.image = image;
            change.subresourceRange = color;
            barrier(cmd, before, after, 0, 0, nullptr, 0, nullptr, 1, &change);
        };
        constexpr auto undefined = VK_IMAGE_LAYOUT_UNDEFINED;
        constexpr auto top = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        constexpr auto transfer_write = VK_ACCESS_TRANSFER_WRITE_BIT;
        constexpr auto transfer_read = VK_ACCESS_TRANSFER_READ_BIT;
        constexpr auto shader_read = VK_ACCESS_SHADER_READ_BIT;
        constexpr auto shader_write = VK_ACCESS_SHADER_WRITE_BIT;
        const bool integer_source = route == E5ReadRoute::IntegerView;
        transition(source, undefined, upload, top, transfer, 0, transfer_write);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent, extent, 1};
        to_image(cmd, input.handle, source, upload, 1, &region);
        transition(source, upload, integer_source ? sampled : download, transfer, integer_source ? compute : transfer,
                   transfer_write, integer_source ? shader_read : transfer_read);
        if (intermediate) {
            transition(intermediate, undefined, upload, top, transfer, 0, transfer_write);
            VkImageCopy copy_region{};
            copy_region.srcSubresource = copy_region.dstSubresource = region.imageSubresource;
            copy_region.extent = region.imageExtent;
            copy(cmd, source, download, intermediate, upload, 1, &copy_region);
            transition(intermediate, upload, sampled, transfer, compute, transfer_write, shader_read);
        }
        if (route != E5ReadRoute::Buffer) {
            transition(target, undefined, storage, top, compute, 0, shader_write);
            copy_e5_integer_view(gpu, cmd, intermediate ? intermediate : source, target, extent);
            transition(target, storage, download, compute, transfer, shader_write, transfer_read);
        }
        if (!result.ran) ready();
        to_buffer(cmd, target, download, output.handle, 1, &region);
        gpu.host_barrier(cmd, transfer, transfer_write);
        gpu.submit(cmd);
        gpu.invalidate(output);
        result.ran = true;
        const auto *values = static_cast<const uint32_t *>(output.mapped);
        for (uint32_t i = 0; i < pixels; ++i) {
            const auto expected = e5_probe_word(i);
            if (values[i] == expected) continue;
            if (extent == 16) { ++small_wrong; continue; }
            ++large_wrong;
            large_canonical += values[i] == canonical_e5_probe_word(expected);
        }
    }
    result.passed = !large_wrong && !small_wrong;
    result.canonicalized = large_wrong && large_wrong == large_canonical && !small_wrong;
    result.detail = "30x30: " + std::to_string(large_wrong) + "/900 words changed (" +
        std::to_string(large_canonical) + " canonicalized); 16x16: " + std::to_string(small_wrong) + "/256 words changed";
    return result;
}
} // namespace vkfix::nativeprobe

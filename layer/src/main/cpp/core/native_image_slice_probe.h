#pragma once
#include "image_slice_regions.h"
#include "native_probe_resources.h"
#include <array>

namespace vkfix::nativeprobe {
struct ImageSliceReadback {
    std::string error;
    bool only_first_slice_written = false;
};
// Reproduce the mutable packed-format array-to-volume copy used by texture
// backends. The control changes only the partition of the copy regions.
template<class Context, class Ready>
ImageSliceReadback probe_image_slices(Context &p, VkPhysicalDevice physical, uint32_t family, bool split, Ready ready) {
    constexpr uint32_t width = 7, height = 7, slices = 8;
    constexpr VkExtent3D volume{30, 30, 24};
    constexpr uint32_t left = 3, top = 5, front = 2;
    constexpr uint32_t slice_words = width * height;
    constexpr uint32_t upload_words = slice_words * slices;
    constexpr uint32_t readback_words = volume.width * volume.height * volume.depth;
    auto pattern = [](uint32_t index) { return 0x8c251b73u ^ ((index + 1) * 0x045d9f3bu); };

    Resources<Context> gpu(p, physical, family);
    auto create_image = [&](VkImageType type, VkFormat format, VkExtent3D extent, uint32_t layers) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (type == VK_IMAGE_TYPE_3D) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        ci.imageType = type;
        ci.format = format;
        ci.extent = extent;
        ci.mipLevels = 1;
        ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkFormatProperties format_properties{};
        p.template get<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
            physical, format, &format_properties);
        if (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
            ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkImageFormatProperties limits{};
        const auto result = p.template get<PFN_vkGetPhysicalDeviceImageFormatProperties>(
            "vkGetPhysicalDeviceImageFormatProperties")(physical, format, type, ci.tiling, ci.usage, ci.flags, &limits);
        check(result, "mutable packed image format support");
        if (extent.width > limits.maxExtent.width || extent.height > limits.maxExtent.height ||
            extent.depth > limits.maxExtent.depth || layers > limits.maxArrayLayers ||
            !(limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
            throw std::runtime_error("mutable packed image shape unsupported");
        return gpu.create_image(ci);
    };
    auto source = create_image(VK_IMAGE_TYPE_2D, VK_FORMAT_A2B10G10R10_UINT_PACK32, {width, height, 1}, slices);
    auto target = create_image(VK_IMAGE_TYPE_3D, VK_FORMAT_A2B10G10R10_UNORM_PACK32, volume, 1);
    const auto upload = gpu.buffer(upload_words * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const auto readback = gpu.buffer(readback_words * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto *data = static_cast<uint32_t *>(upload.mapped);
    for (uint32_t index = 0; index < upload_words; ++index) data[index] = pattern(index);
    VkMappedMemoryRange upload_range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    upload_range.memory = upload.memory;
    upload_range.size = VK_WHOLE_SIZE;
    check(p.template dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device, 1, &upload_range),
          "flush packed image source");

    const auto cb = gpu.begin();
    auto barrier = p.template dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    auto to_image = p.template dev<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    auto copy = p.template dev<PFN_vkCmdCopyImage>("vkCmdCopyImage");
    auto to_buffer = p.template dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    auto clear = p.template dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage");
    auto transition = [&](VkImage image, uint32_t layers, VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags access) {
        VkImageMemoryBarrier change{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        const bool undefined = old_layout == VK_IMAGE_LAYOUT_UNDEFINED;
        change.srcAccessMask = undefined ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT;
        change.dstAccessMask = access;
        change.oldLayout = old_layout;
        change.newLayout = new_layout;
        change.srcQueueFamilyIndex = change.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        change.image = image;
        change.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        barrier(cb, undefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &change);
    };
    constexpr auto undefined = VK_IMAGE_LAYOUT_UNDEFINED;
    constexpr auto dst = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    constexpr auto src = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    transition(source, slices, undefined, dst, VK_ACCESS_TRANSFER_WRITE_BIT);
    transition(target, VK_REMAINING_ARRAY_LAYERS, undefined, dst, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy input{};
    input.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, slices};
    input.imageExtent = {width, height, 1};
    to_image(cb, upload.handle, source, dst, 1, &input);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange target_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    clear(cb, target, dst, &zero, 1, &target_range);
    transition(source, slices, dst, src, VK_ACCESS_TRANSFER_READ_BIT);
    transition(target, VK_REMAINING_ARRAY_LAYERS, dst, dst, VK_ACCESS_TRANSFER_WRITE_BIT);

    ready();
    VkImageCopy region{};
    region.srcSubresource = input.imageSubresource;
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffset = {left, top, front};
    region.extent = {width, height, slices};
    std::array<VkImageCopy, slices> regions{};
    if (split)
        for (uint32_t slice = 0; slice < slices; ++slice) regions[slice] = array_to_volume_slice(region, slice);
    copy(cb, source, src, target, dst, split ? slices : 1, split ? regions.data() : &region);
    transition(target, VK_REMAINING_ARRAY_LAYERS, dst, src, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy output{};
    output.imageSubresource = region.dstSubresource;
    output.imageExtent = volume;
    to_buffer(cb, target, src, readback.handle, 1, &output);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    gpu.submit(cb);
    gpu.invalidate(readback);

    const auto *words = static_cast<const uint32_t *>(readback.mapped);
    uint32_t exact_slices = 0, wrong_texels = 0, outside_changes = 0;
    bool only_first_slice_written = true;
    for (uint32_t z = 0; z < volume.depth; ++z) {
        const bool copied_slice = z >= front && z < front + slices;
        uint32_t mismatches = 0;
        for (uint32_t y = 0; y < volume.height; ++y) {
            for (uint32_t x = 0; x < volume.width; ++x) {
                const auto word = words[(z * volume.height + y) * volume.width + x];
                const bool copied_texel = copied_slice && y >= top && y < top + height &&
                                          x >= left && x < left + width;
                if (copied_texel) {
                    const uint32_t index = (z - front) * slice_words + (y - top) * width + x - left;
                    mismatches += word != pattern(index);
                    only_first_slice_written &= word == (z == front ? pattern(index) : 0u);
                } else {
                    outside_changes += word != 0;
                }
            }
        }
        wrong_texels += mismatches;
        exact_slices += copied_slice && mismatches == 0;
    }
    if (!wrong_texels && !outside_changes) return {};
    return {std::to_string(exact_slices) + "/" + std::to_string(slices) + " copied slices exact, " +
            std::to_string(wrong_texels) + " wrong texels, " + std::to_string(outside_changes) + " changes outside region",
            !split && only_first_slice_written && !outside_changes};
}
} // namespace vkfix::nativeprobe

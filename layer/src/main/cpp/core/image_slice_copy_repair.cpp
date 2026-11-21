#include "image_slice_copy_repair.h"
#include "driver_trust.h"
#include "image_slice_regions.h"
#include "vk_common.h"
#include <algorithm>
#include <limits>
#include <mutex>

namespace vkfix {
ImageSliceCopyRepair &ImageSliceCopyRepair::get() {
    static ImageSliceCopyRepair repair;
    return repair;
}

ImageSliceCopyRepair::ImageKind ImageSliceCopyRepair::image_kind(const VkImageCreateInfo &info) {
    constexpr auto mutable_format = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    constexpr auto array_compatible = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    if (info.pNext || info.tiling != VK_IMAGE_TILING_OPTIMAL || info.samples != VK_SAMPLE_COUNT_1_BIT)
        return ImageKind::Other;
    if (info.imageType == VK_IMAGE_TYPE_2D && info.format == VK_FORMAT_A2B10G10R10_UINT_PACK32 &&
        info.arrayLayers > 1 && info.flags == mutable_format && (info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return ImageKind::Array;
    if (info.imageType == VK_IMAGE_TYPE_3D && info.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
        info.extent.depth > 1 && info.flags == (mutable_format | array_compatible) &&
        (info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return ImageKind::Volume;
    return ImageKind::Other;
}

void ImageSliceCopyRepair::remember_image(VkDevice device, VkImage image, ImageKind kind, uint32_t layers,
                                          VkInstance instance, VkPhysicalDevice physical,
                                          PFN_vkGetInstanceProcAddr gipa) {
    try {
        if (kind == ImageKind::Array) {
            std::unique_lock lock(mutex_);
            devices_[device].arrays.insert_or_assign(image, layers);
            return;
        }
        if (kind != ImageKind::Volume || !instance || !physical || !gipa) return;
        {
            std::unique_lock lock(mutex_);
            auto existing = devices_.find(device);
            if (existing != devices_.end() && !existing->second.families.empty()) {
                existing->second.volumes.insert(image);
                return;
            }
        }
        const NativeDriverProbes probes(instance, physical, gipa);
        const auto measurement = probes.measure(DriverProbe::CrossFormatImageSlices);
        if (measurement.verdict != DriverProbeVerdict::RepairConfirmed) return;
        auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        if (!query) return;
        uint32_t count = 0;
        query(physical, &count, nullptr);
        if (!count || count > 65536) return;
        std::vector<VkQueueFamilyProperties> families(count);
        query(physical, &count, families.data());
        if (!count || count > families.size()) return;
        families.resize(count);
        std::unique_lock lock(mutex_);
        auto &state = devices_[device];
        state.volumes.insert(image);
        if (state.families.empty()) {
            state.families = std::move(families);
            LOGI("Measured driver repair: cross-format image slice copy (%s)", measurement.detail.c_str());
        }
    } catch (const std::bad_alloc &) {
        // The native image remains usable if optional repair metadata cannot be stored.
    }
}

void ImageSliceCopyRepair::forget_image(VkDevice device, VkImage image) {
    std::unique_lock lock(mutex_);
    auto state = devices_.find(device);
    if (state == devices_.end()) return;
    state->second.arrays.erase(image);
    state->second.volumes.erase(image);
}

void ImageSliceCopyRepair::forget_device(VkDevice device) {
    std::unique_lock lock(mutex_);
    devices_.erase(device);
}

std::vector<VkImageCopy> ImageSliceCopyRepair::split_regions(VkDevice device, uint32_t queue_family, VkImage src,
                                                            VkImage dst, uint32_t count, const VkImageCopy *regions) const {
    if (!count || !regions || src == dst) return {};
    uint32_t layers = 0;
    {
        std::shared_lock lock(mutex_);
        auto state = devices_.find(device);
        if (state == devices_.end() || !state->second.volumes.count(dst) || queue_family >= state->second.families.size())
            return {};
        const auto &family = state->second.families[queue_family];
        if (!(family.queueFlags & VK_QUEUE_GRAPHICS_BIT) || family.minImageTransferGranularity.depth != 1) return {};
        auto source = state->second.arrays.find(src);
        if (source == state->second.arrays.end()) return {};
        layers = source->second;
    }

    auto can_split = [layers](const VkImageCopy &region) {
        const auto &source = region.srcSubresource;
        const auto &target = region.dstSubresource;
        return region.extent.depth > 1 && source.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
               target.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && source.layerCount == region.extent.depth &&
               source.baseArrayLayer < layers && source.layerCount <= layers - source.baseArrayLayer &&
               target.layerCount == 1 && target.baseArrayLayer == 0 && region.srcOffset.z == 0 &&
               region.dstOffset.z >= 0 &&
               uint64_t(region.dstOffset.z) + region.extent.depth - 1 <= std::numeric_limits<int32_t>::max();
    };
    uint64_t expanded = count;
    for (uint32_t i = 0; i < count; ++i)
        if (can_split(regions[i])) expanded += regions[i].extent.depth - 1;
    if (expanded == count || expanded > std::numeric_limits<uint32_t>::max()) return {};
    try {
        std::vector<VkImageCopy> result;
        if (expanded > result.max_size()) return {};
        result.reserve(static_cast<size_t>(expanded));
        for (uint32_t i = 0; i < count; ++i) {
            const auto &region = regions[i];
            if (!can_split(region)) {
                result.push_back(region);
                continue;
            }
            for (uint32_t slice = 0; slice < region.extent.depth; ++slice)
                result.push_back(array_to_volume_slice(region, slice));
        }
        return result;
    } catch (const std::bad_alloc &) {
        return {};
    }
}
} // namespace vkfix

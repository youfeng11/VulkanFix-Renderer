#pragma once
#include <vulkan/vulkan.h>

namespace vkfix {
// Prepare private native permissions before image creation. Public creation
// parameters remain unchanged; memory requirements come from the actual image.
// Extended image-format queries/chains still require their own lowering.
inline VkResult prepare_msaa_resolve_image(VkPhysicalDevice physical,
    PFN_vkGetPhysicalDeviceImageFormatProperties query, const VkImageCreateInfo& source,
    VkImageCreateInfo& native) {
    native = source;
    if (!query || source.pNext || source.imageType != VK_IMAGE_TYPE_2D ||
        source.format != VK_FORMAT_R32G32B32A32_SFLOAT ||
        !(source.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return VK_SUCCESS;
    VkImageUsageFlags required = 0;
    if (source.samples == VK_SAMPLE_COUNT_4_BIT && source.arrayLayers > 1)
        required = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    else if (source.samples == VK_SAMPLE_COUNT_1_BIT)
        required = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!required || (source.usage & required)) return VK_SUCCESS;
    auto candidate = source;
    // Native transfer images cannot retain transient usage. Creation below
    // checks the original memory-type class before accepting this candidate.
    candidate.usage = (candidate.usage & ~VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) | required;
    VkImageFormatProperties original{}, augmented{};
    auto result = query(physical,source.format,source.imageType,source.tiling,source.usage,source.flags,&original);
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) return result;
    if (result != VK_SUCCESS) return VK_SUCCESS;
    result = query(physical,candidate.format,candidate.imageType,candidate.tiling,candidate.usage,candidate.flags,&augmented);
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) return result;
    if (result != VK_SUCCESS || !(augmented.sampleCounts & source.samples) ||
        augmented.maxExtent.width < source.extent.width || augmented.maxExtent.height < source.extent.height ||
        augmented.maxExtent.depth < source.extent.depth || augmented.maxMipLevels < source.mipLevels ||
        augmented.maxArrayLayers < source.arrayLayers || augmented.maxResourceSize < original.maxResourceSize)
        return VK_SUCCESS;
    native = candidate;
    return VK_SUCCESS;
}

inline VkResult create_msaa_resolve_image(VkDevice device, const VkImageCreateInfo& source,
    VkImageCreateInfo& native, const VkAllocationCallbacks* allocator, VkImage* image,
    PFN_vkCreateImage create, PFN_vkDestroyImage destroy, PFN_vkGetImageMemoryRequirements requirements) {
    const bool transient_changed = (source.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) !=
        (native.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
    if (!transient_changed) return create(device,&native,allocator,image);
    // Color-image memoryTypeBits must remain identical within each public
    // tiling/sparse/protected/external/transient class. A lazy-memory class
    // cannot silently become a different native non-transient class.
    if (!destroy || !requirements) { native = source; return create(device,&native,allocator,image); }
    VkImage original = VK_NULL_HANDLE;
    auto result = create(device,&source,allocator,&original);
    if (result != VK_SUCCESS) { *image = VK_NULL_HANDLE; return result; }
    VkMemoryRequirements original_requirements{};
    requirements(device,original,&original_requirements);
    VkImage augmented = VK_NULL_HANDLE;
    result = create(device,&native,allocator,&augmented);
    if (result != VK_SUCCESS) {
        // Do not return the original after a transient allocation failure:
        // identical public creations must not alternate between native sizes.
        destroy(device,original,allocator); *image = VK_NULL_HANDLE; return result;
    }
    VkMemoryRequirements augmented_requirements{};
    requirements(device,augmented,&augmented_requirements);
    if (original_requirements.memoryTypeBits == augmented_requirements.memoryTypeBits) {
        destroy(device,original,allocator); *image = augmented;
    } else {
        destroy(device,augmented,allocator); native = source; *image = original;
    }
    return VK_SUCCESS;
}
} // namespace vkfix

#pragma once
#include "image_type_copy.h"
#include <tuple>

namespace vkfix {
inline VkImageLayout volume_native_layout(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: case VK_IMAGE_LAYOUT_PREINITIALIZED:
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return layout;
    default: return VK_IMAGE_LAYOUT_GENERAL;
    }
}
template<class Barrier> bool lower_volume_barrier(const MultiviewImage& image, Barrier& barrier) {
    if (!image.emulated_attachment) return false;
    barrier.oldLayout = volume_native_layout(barrier.oldLayout); barrier.newLayout = volume_native_layout(barrier.newLayout);
    // Core1.1 transitions the whole 3D mip. After removing the compatible
    // image flag the old backend has exactly one array layer, not depth layers.
    barrier.subresourceRange.baseArrayLayer = 0; barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask |= VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask |= VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    return true;
}
inline bool lower_volume_image(VkImageCreateInfo& info) {
    if (info.imageType != VK_IMAGE_TYPE_3D || !(info.flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) return false;
    info.flags &= ~VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    info.usage &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
    info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return true;
}
// A real native identity with no attachment storage. Its public parameters
// remain in the framebuffer registry; only private replay framebuffers render.
VkResult create_replay_framebuffer(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
// One recorded render-pass instance owns this backing, not the public view.
// Copying only the render area and selected slice runs preserves independent
// submissions that use other pixels/slices of the same public volume.
struct VolumeAttachmentPlan {
    MultiviewImage volume;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspects = 0;
    uint32_t mip = 0, base_slice = 0, layers = 0;
    VkExtent2D extent{};
    static VkResult capture(const MultiviewImage&, const VkImageViewCreateInfo&,
                            VkExtent2D, uint32_t layers, VolumeAttachmentPlan&);
    VkResult region(VkRect2D, uint32_t first, uint32_t count, VkImageAspectFlags,
                    bool importing, VkImageCopy&) const;
};

class VolumeAttachment {
public:
    static VkResult create(VkDevice, VkPhysicalDevice, const VolumeAttachmentPlan&,
                           std::unique_ptr<VolumeAttachment>&,
                           PFN_vkGetImageMemoryRequirements2 native_requirements2 = nullptr);
    ~VolumeAttachment();
    VolumeAttachment(const VolumeAttachment&) = delete;
    VolumeAttachment& operator=(const VolumeAttachment&) = delete;
    const MultiviewImage& image() const { return image_; }
    const VolumeAttachmentPlan& plan() const { return plan_; }
    VkResult view(uint32_t first, uint32_t count, VkImageAspectFlags, VkImageViewType, VkImageView&);
    // Native pass descriptions use GENERAL as the backing's initial/final
    // layout. Call once before the first use in this recording; the command
    // repeats normally when the same command buffer is resubmitted.
    VkResult initialize(VkCommandBuffer);
    VkResult import(VkCommandBuffer, VkImageLayout volume_layout, VkRect2D,
                    uint32_t first, uint32_t count, VkImageAspectFlags);
    VkResult store(VkCommandBuffer, VkImageLayout volume_layout, VkRect2D,
                   uint32_t first, uint32_t count, VkImageAspectFlags);
    // Framebuffers must be released before this resource. Teardown may call
    // release even while immutable command metadata retains the C++ object.
    void release();
private:
    VolumeAttachment(VkDevice, VkPhysicalDevice, const VolumeAttachmentPlan&);
    VkResult allocate(PFN_vkGetImageMemoryRequirements2);
    VkResult transfer(VkCommandBuffer, VkImageLayout, VkRect2D, uint32_t, uint32_t, VkImageAspectFlags, bool);
    VkDevice device_;
    VkPhysicalDevice physical_;
    VolumeAttachmentPlan plan_;
    MultiviewImage image_;
    VkDeviceMemory memory_{};
    bool initialized_ = false;
    PFN_vkDestroyImage destroy_image_{};
    PFN_vkFreeMemory free_{};
    PFN_vkCreateImageView create_view_{};
    PFN_vkDestroyImageView destroy_view_{};
    PFN_vkCmdPipelineBarrier barrier_{};
    std::map<std::tuple<uint32_t,uint32_t,VkImageAspectFlags,VkImageViewType>,VkImageView> views_;
};
}

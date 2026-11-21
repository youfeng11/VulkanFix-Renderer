#pragma once
#include "multiview_render_pass.h"
#include <atomic>

namespace vkfix {
class VolumeAttachment;
struct MultiviewImage {
    VkImage handle{};
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t mip_levels = 1, layers = 1, depth = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags native_usage = 0;
    uint32_t width = 1, height = 1;
    bool emulated_attachment = false;
};
struct MultiviewConversion {
    VkSamplerYcbcrConversionCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    bool core_only = true;
};

// Records are identities, independent of reusable native handle values.
// Only internal slices are owned; the application owns the original view.
class MultiviewViewResources {
public:
    MultiviewViewResources(VkDevice, const VkImageViewCreateInfo&,
        std::shared_ptr<MultiviewImage>, std::shared_ptr<const MultiviewConversion>,
        std::shared_ptr<VolumeAttachment> placeholder = {});
    ~MultiviewViewResources();
    VkResult slice(uint32_t view, VkImageAspectFlags aspects, VkImageView& result);
    VkResult whole(VkImageAspectFlags aspects, VkImageView& result);
    const std::shared_ptr<MultiviewImage>& image() const { return image_; }
    const VkImageViewCreateInfo& info() const { return info_; }
    bool transfer_subresource(uint32_t view, VkImageSubresourceLayers& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!device_ || !core_only_ || !image_ || image_->type != VK_IMAGE_TYPE_2D ||
            view >= info_.subresourceRange.layerCount || info_.format != image_->format ||
            info_.subresourceRange.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) return false;
        result = {VK_IMAGE_ASPECT_COLOR_BIT, info_.subresourceRange.baseMipLevel,
            info_.subresourceRange.baseArrayLayer + view, 1};
        return true;
    }
    bool has_slices() const { return slice_count_.load(std::memory_order_relaxed) != 0; }
    bool alive() { std::lock_guard<std::mutex> lock(mutex_); return device_ != VK_NULL_HANDLE; }
    bool covers_view(uint32_t view) {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_ && view < 32 && view < info_.subresourceRange.layerCount &&
            (info_.viewType == VK_IMAGE_VIEW_TYPE_2D || info_.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    }
    // Automatic render-pass transitions include active views even when a
    // particular attachment is never referenced by those views. For 3D images
    // the Core 1.1 range is the entire mip, not the sliced depth layers.
    VkResult layout_barriers(uint32_t unused_views, bool used_elsewhere,
        VkImageLayout initial, VkImageLayout final, std::vector<VkImageMemoryBarrier>& result, uint32_t ordinary_layers = 0);
    void release();
private:
    VkResult make_view(std::optional<uint32_t>, VkImageAspectFlags, VkImageView&);
    std::mutex mutex_;
    VkDevice device_;
    VkImageViewCreateInfo info_;
    std::shared_ptr<MultiviewImage> image_;
    std::shared_ptr<const MultiviewConversion> conversion_;
    std::shared_ptr<VolumeAttachment> placeholder_;
    std::optional<VkImageUsageFlags> usage_;
    bool core_only_ = true;
    PFN_vkCreateImageView create_{};
    PFN_vkDestroyImageView destroy_{};
    std::map<std::pair<uint32_t, VkImageAspectFlags>, VkImageView> slices_;
    std::atomic<uint32_t> slice_count_{0};
};

class MultiviewFramebufferResources {
public:
    using Views = std::vector<std::shared_ptr<MultiviewViewResources>>;
    MultiviewFramebufferResources(VkDevice, const VkFramebufferCreateInfo&, Views);
    ~MultiviewFramebufferResources();
    bool imageless() const { return imageless_; }
    uint32_t attachment_count() const { return count_; }
    VkExtent2D extent() const { return {width_,height_}; }
    uint32_t layers() const { return layers_; }
    const Views& attachments() const { return attachments_; }
    // active_pass may differ from the compatible pass used to create the FB.
    VkResult native_framebuffer(const std::shared_ptr<MultiviewPassResources>& active_pass,
        uint32_t subpass, uint32_t view, bool first, bool last, bool native_aspects,
        const Views& bound, VkFramebuffer& result, const std::vector<uint32_t>& manual_layouts = {},
        const std::vector<uint32_t>& general_attachments = {});
    void invalidate(const std::shared_ptr<MultiviewViewResources>& view);
    void release();
private:
    struct Native {
        std::shared_ptr<MultiviewPassResources> pass;
        uint32_t subpass, view;
        bool first, last, aspects;
        std::vector<uint32_t> manual_layouts;
        std::vector<uint32_t> general_attachments;
        Views attachments;
        VkFramebuffer handle;
    };
    std::mutex mutex_;
    VkDevice device_;
    uint32_t width_, height_, count_, layers_;
    bool imageless_, core_only_;
    Views attachments_;
    PFN_vkCreateFramebuffer create_{};
    PFN_vkDestroyFramebuffer destroy_{};
    std::vector<Native> native_;
};

class MultiviewAttachments {
public:
    static MultiviewAttachments& get();
    // Call only for devices without an enabled native multiview route.
    void remember_image(VkDevice, VkImage, const VkImageCreateInfo&, bool emulated_attachment = false);
    void remember_conversion(VkDevice, VkSamplerYcbcrConversion, const VkSamplerYcbcrConversionCreateInfo&);
    void remember_view(VkDevice, VkImageView, const VkImageViewCreateInfo&, std::shared_ptr<VolumeAttachment> placeholder = {});
    void remember_framebuffer(VkDevice, VkFramebuffer, const VkFramebufferCreateInfo&);
    void remember_swapchain(VkDevice, VkSwapchainKHR, const VkSwapchainCreateInfoKHR&);
    void remember_swapchain_images(VkDevice, VkSwapchainKHR, uint32_t, const VkImage*);
    std::shared_ptr<MultiviewViewResources> acquire_view(VkDevice, VkImageView);
    std::shared_ptr<const MultiviewImage> acquire_image(VkDevice, VkImage);
    std::shared_ptr<MultiviewFramebufferResources> acquire_framebuffer(VkDevice, VkFramebuffer);
    VkResult capture_framebuffer(VkDevice, const VkRenderPassBeginInfo&,
        std::shared_ptr<MultiviewFramebufferResources>&, MultiviewFramebufferResources::Views&);
    VkResult native_framebuffer(VkDevice, const VkRenderPassBeginInfo&, uint32_t subpass,
        uint32_t view, bool first, bool last, bool native_aspects, VkFramebuffer&);
    void forget_image(VkDevice, VkImage);
    void forget_conversion(VkDevice, VkSamplerYcbcrConversion);
    void forget_view(VkDevice, VkImageView);
    void forget_framebuffer(VkDevice, VkFramebuffer);
    void forget_swapchain(VkDevice, VkSwapchainKHR);
    void forget_device(VkDevice);
private:
    struct Swapchain {
        uint32_t layers;
        std::vector<std::shared_ptr<MultiviewImage>> images;
        VkFormat format;
        VkExtent2D extent;
    };
    void invalidate_view_locked(VkDevice, const std::shared_ptr<MultiviewViewResources>&);
    void invalidate_image_locked(VkDevice, const std::shared_ptr<MultiviewImage>&);
    std::mutex mutex_;
    std::unordered_map<DeviceObjectKey, std::shared_ptr<MultiviewImage>, DeviceObjectKeyHash> images_;
    std::unordered_map<DeviceObjectKey, std::shared_ptr<MultiviewConversion>, DeviceObjectKeyHash> conversions_;
    std::unordered_map<DeviceObjectKey, std::shared_ptr<MultiviewViewResources>, DeviceObjectKeyHash> views_;
    std::unordered_map<DeviceObjectKey, std::shared_ptr<MultiviewFramebufferResources>, DeviceObjectKeyHash> framebuffers_;
    std::unordered_map<DeviceObjectKey, Swapchain, DeviceObjectKeyHash> swapchains_;
    std::map<VkDevice, std::vector<std::weak_ptr<MultiviewViewResources>>> live_views_;
    std::map<VkDevice, std::vector<std::weak_ptr<MultiviewFramebufferResources>>> live_framebuffers_;
};
// A private pass may replace logical 3D views with recording-owned 2D arrays.
// Descriptor selections use the same mapping, including distinct public view
// handles selecting the same subresource. The scope also keys arena snapshots.
class AttachmentReplayScope {
public:
    using Binding = std::pair<std::shared_ptr<MultiviewViewResources>,std::shared_ptr<MultiviewViewResources>>;
    using Bindings = std::vector<Binding>;
    explicit AttachmentReplayScope(const Bindings*);
    ~AttachmentReplayScope();
    static const Bindings* current();
    static MultiviewViewResources* lookup(const MultiviewViewResources*);
private:
    const Bindings* previous_;
};
VkImageLayout attachment_descriptor_layout(VkDevice, VkImageView, VkImageLayout);
}

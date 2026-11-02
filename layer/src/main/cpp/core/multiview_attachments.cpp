#include "multiview_attachments.h"
#include "driver_loader.h"
#include <algorithm>
#include <stdexcept>

namespace vkfix {
namespace {
template<class T> T proc(VkDevice device, const char* name) {
    return reinterpret_cast<T>(get_real_proc(get_last_instance(), device, name));
}
template<class T> void prune(T& refs) {
    refs.erase(std::remove_if(refs.begin(), refs.end(), [](const auto& r) { return r.expired(); }), refs.end());
}
// One batch describes simultaneous transitions. Aliased attachment indices may
// contribute overlapping ranges with identical semantics; transition their
// union once, without joining holes or collapsing distinct layout changes.
void append_transition(std::vector<VkImageMemoryBarrier>& result, VkImageMemoryBarrier barrier) {
    for (size_t i = 0; i < result.size();) {
        const auto& other = result[i];
        const auto& a = barrier.subresourceRange; const auto& b = other.subresourceRange;
        const uint64_t end = uint64_t(a.baseArrayLayer) + a.layerCount;
        const uint64_t other_end = uint64_t(b.baseArrayLayer) + b.layerCount;
        if (barrier.image == other.image && barrier.oldLayout == other.oldLayout && barrier.newLayout == other.newLayout &&
            barrier.srcAccessMask == other.srcAccessMask && barrier.dstAccessMask == other.dstAccessMask &&
            a.aspectMask == b.aspectMask && a.baseMipLevel == b.baseMipLevel && a.levelCount == b.levelCount &&
            uint64_t(a.baseArrayLayer) <= other_end && uint64_t(b.baseArrayLayer) <= end) {
            const auto first = std::min(a.baseArrayLayer, b.baseArrayLayer);
            barrier.subresourceRange.baseArrayLayer = first;
            barrier.subresourceRange.layerCount = uint32_t(std::max(end, other_end) - first);
            result.erase(result.begin() + i);
            i = 0;
        } else ++i;
    }
    result.push_back(barrier);
}
}
MultiviewViewResources::MultiviewViewResources(VkDevice device, const VkImageViewCreateInfo& info,
    std::shared_ptr<MultiviewImage> image, std::shared_ptr<const MultiviewConversion> conversion)
    : device_(device), info_(info), image_(std::move(image)), conversion_(std::move(conversion)),
      create_(proc<PFN_vkCreateImageView>(device, "vkCreateImageView")),
      destroy_(proc<PFN_vkDestroyImageView>(device, "vkDestroyImageView")) {
    info_.pNext = nullptr;
    for (auto p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO)
            usage_ = reinterpret_cast<const VkImageViewUsageCreateInfo*>(p)->usage;
        else if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) {
            const auto handle = reinterpret_cast<const VkSamplerYcbcrConversionInfo*>(p)->conversion;
            if (handle && (!conversion_ || !conversion_->core_only)) core_only_ = false;
        } else core_only_ = false;
    }
    auto& range = info_.subresourceRange;
    if (range.levelCount == VK_REMAINING_MIP_LEVELS) {
        if (image_ && range.baseMipLevel < image_->mip_levels)
            range.levelCount = image_->mip_levels - range.baseMipLevel;
        else core_only_ = false;
    }
    if (range.layerCount == VK_REMAINING_ARRAY_LAYERS) {
        uint32_t layers = image_ ? image_->layers : 0;
        if (image_ && image_->type == VK_IMAGE_TYPE_3D && range.baseMipLevel < 32)
            layers = std::max(1u, image_->depth >> range.baseMipLevel);
        if (range.baseArrayLayer < layers) range.layerCount = layers - range.baseArrayLayer;
        else core_only_ = false;
    }
}
MultiviewViewResources::~MultiviewViewResources() { release(); }
VkResult MultiviewViewResources::layout_barriers(uint32_t mask, bool used_elsewhere,
    VkImageLayout initial, VkImageLayout final, std::vector<VkImageMemoryBarrier>& result) {
    if (!mask || initial == final) return VK_SUCCESS;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !image_ || !core_only_) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto range = info_.subresourceRange;
    // Framebuffer depth/stencil attachments transition both aspects in Core
    // 1.1, including when the view's aspectMask names just one of them.
    switch (info_.format) {
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; break;
    default: break;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    // Discarding the old contents does not remove WAW hazards with earlier
    // writes or layout transitions. Include availability even for UNDEFINED.
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = initial; barrier.newLayout = final; barrier.image = image_->handle;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = range;
    if (image_->type == VK_IMAGE_TYPE_3D) {
        // A referenced slice already causes an automatic whole-mip transition.
        // Only a wholly unused 3D attachment needs an extra transition here.
        if (used_elsewhere) return VK_SUCCESS;
        barrier.subresourceRange.baseArrayLayer = 0; barrier.subresourceRange.layerCount = 1;
        append_transition(result, barrier); return VK_SUCCESS;
    }
    if (info_.viewType != VK_IMAGE_VIEW_TYPE_2D && info_.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t view = 0; view < 32;) {
        if (!(mask & (uint32_t(1) << view))) { ++view; continue; }
        const auto first = view++;
        while (view < 32 && (mask & (uint32_t(1) << view))) ++view;
        if (view > range.layerCount) return VK_ERROR_FEATURE_NOT_PRESENT;
        barrier.subresourceRange.baseArrayLayer = range.baseArrayLayer + first;
        barrier.subresourceRange.layerCount = view - first;
        append_transition(result, barrier);
    }
    return VK_SUCCESS;
}
void MultiviewViewResources::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_ && destroy_) for (const auto& slice : slices_) destroy_(device_, slice.second, nullptr);
    slices_.clear(); slice_count_.store(0, std::memory_order_relaxed); device_ = VK_NULL_HANDLE;
}
VkResult MultiviewViewResources::slice(uint32_t view, VkImageAspectFlags aspects, VkImageView& result) {
    result = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !create_ || !destroy_ || !core_only_ || view >= 32 ||
        view >= info_.subresourceRange.layerCount ||
        (info_.viewType != VK_IMAGE_VIEW_TYPE_2D && info_.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!aspects) aspects = info_.subresourceRange.aspectMask;
    if (aspects & ~info_.subresourceRange.aspectMask) return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto key = std::make_pair(view, aspects);
    const auto found = slices_.find(key);
    if (found != slices_.end()) { result = found->second; return VK_SUCCESS; }
    VkImageViewCreateInfo ci = info_;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.subresourceRange.baseArrayLayer += view;
    ci.subresourceRange.layerCount = 1;
    ci.subresourceRange.aspectMask = aspects;
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    if (usage_) { usage.usage = *usage_; ci.pNext = &usage; }
    // A conversion may be destroyed immediately after the original view was
    // created. Recreate from owned parameters instead of borrowing that handle.
    VkSamplerYcbcrConversionInfo conversion{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    PFN_vkDestroySamplerYcbcrConversion destroy_conversion = nullptr;
    if (conversion_) {
        auto create_conversion = proc<PFN_vkCreateSamplerYcbcrConversion>(device_, "vkCreateSamplerYcbcrConversion");
        if (!create_conversion) create_conversion = proc<PFN_vkCreateSamplerYcbcrConversion>(device_, "vkCreateSamplerYcbcrConversionKHR");
        destroy_conversion = proc<PFN_vkDestroySamplerYcbcrConversion>(device_, "vkDestroySamplerYcbcrConversion");
        if (!destroy_conversion) destroy_conversion = proc<PFN_vkDestroySamplerYcbcrConversion>(device_, "vkDestroySamplerYcbcrConversionKHR");
        if (!create_conversion || !destroy_conversion) return VK_ERROR_FEATURE_NOT_PRESENT;
        const auto status = create_conversion(device_, &conversion_->info, nullptr, &conversion.conversion);
        if (status != VK_SUCCESS) return status;
        conversion.pNext = ci.pNext; ci.pNext = &conversion;
    }
    VkImageView native{};
    const auto status = create_(device_, &ci, nullptr, &native);
    if (conversion.conversion) destroy_conversion(device_, conversion.conversion, nullptr);
    if (status != VK_SUCCESS) return status;
    try { slices_.emplace(key, native); }
    catch (const std::bad_alloc&) { destroy_(device_, native, nullptr); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    slice_count_.store(uint32_t(slices_.size()), std::memory_order_relaxed);
    result = native; return VK_SUCCESS;
}

MultiviewFramebufferResources::MultiviewFramebufferResources(VkDevice device,
    const VkFramebufferCreateInfo& info, Views attachments)
    : device_(device), width_(info.width), height_(info.height), count_(info.attachmentCount),
      imageless_(bool(info.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)),
      core_only_(!(info.flags & ~VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)), attachments_(std::move(attachments)),
      create_(proc<PFN_vkCreateFramebuffer>(device, "vkCreateFramebuffer")),
      destroy_(proc<PFN_vkDestroyFramebuffer>(device, "vkDestroyFramebuffer")) {
    for (auto p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext)
        if (p->sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO) core_only_ = false;
}
MultiviewFramebufferResources::~MultiviewFramebufferResources() { release(); }
void MultiviewFramebufferResources::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_ && destroy_) for (const auto& fb : native_) destroy_(device_, fb.handle, nullptr);
    native_.clear(); device_ = VK_NULL_HANDLE;
}
void MultiviewFramebufferResources::invalidate(const std::shared_ptr<MultiviewViewResources>& view) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = native_.begin(); it != native_.end();) {
        if (std::find(it->attachments.begin(), it->attachments.end(), view) != it->attachments.end()) {
            if (device_ && destroy_) destroy_(device_, it->handle, nullptr);
            it = native_.erase(it);
        } else ++it;
    }
}
VkResult MultiviewFramebufferResources::native_framebuffer(
    const std::shared_ptr<MultiviewPassResources>& pass, uint32_t subpass, uint32_t view,
    bool first, bool last, bool aspects, const Views& bound, VkFramebuffer& result, const std::vector<uint32_t>& manual_layouts) {
    result = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !create_ || !destroy_ || !core_only_ || !pass || !pass->emulated() || bound.size() != count_)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        const auto chunk = pass->plan()->chunk(subpass, view, first, last, manual_layouts);
        Views sources;
        for (auto original : chunk.original_attachments) {
            if (original >= bound.size() || !bound[original]) return VK_ERROR_FEATURE_NOT_PRESENT;
            sources.push_back(bound[original]);
        }
        for (const auto& fb : native_)
            if (fb.pass == pass && fb.subpass == subpass && fb.view == view && fb.first == first &&
                fb.last == last && fb.aspects == aspects && fb.attachments == sources && fb.manual_layouts == manual_layouts) {
                result = fb.handle; return VK_SUCCESS;
            }
        VkRenderPass native_pass{};
        auto status = pass->native_pass(subpass, view, first, last, aspects, native_pass, manual_layouts);
        if (status != VK_SUCCESS) return status;
        std::vector<VkImageView> handles;
        for (const auto& source : sources) {
            VkImageView handle{};
            status = source->slice(view, 0, handle);
            if (status != VK_SUCCESS) return status;
            handles.push_back(handle);
        }
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass = native_pass; ci.width = width_; ci.height = height_; ci.layers = 1;
        ci.attachmentCount = uint32_t(handles.size()); ci.pAttachments = handles.data();
        VkFramebuffer native{};
        status = create_(device_, &ci, nullptr, &native);
        if (status != VK_SUCCESS) return status;
        try { native_.push_back({pass, subpass, view, first, last, aspects, manual_layouts, std::move(sources), native}); }
        catch (...) { destroy_(device_, native, nullptr); throw; }
        result = native; return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    catch (const std::invalid_argument&) { return VK_ERROR_FEATURE_NOT_PRESENT; }
    catch (const std::out_of_range&) { return VK_ERROR_FEATURE_NOT_PRESENT; }
}

MultiviewAttachments& MultiviewAttachments::get() { static MultiviewAttachments registry; return registry; }
void MultiviewAttachments::remember_image(VkDevice device, VkImage image, const VkImageCreateInfo& info) {
    auto record = std::make_shared<MultiviewImage>(MultiviewImage{image, info.imageType, info.mipLevels, info.arrayLayers, info.extent.depth});
    std::lock_guard<std::mutex> lock(mutex_);
    images_[DeviceObjectKey::make(device, image)] = std::move(record);
}
void MultiviewAttachments::remember_conversion(VkDevice device, VkSamplerYcbcrConversion conversion,
    const VkSamplerYcbcrConversionCreateInfo& info) {
    auto record = std::make_shared<MultiviewConversion>();
    record->info = info; record->info.pNext = nullptr; record->core_only = !info.pNext;
    std::lock_guard<std::mutex> lock(mutex_);
    conversions_[DeviceObjectKey::make(device, conversion)] = std::move(record);
}
void MultiviewAttachments::remember_view(VkDevice device, VkImageView view, const VkImageViewCreateInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto image = images_.find(DeviceObjectKey::make(device, info.image));
    std::shared_ptr<const MultiviewConversion> conversion;
    for (auto p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) {
            const auto handle = reinterpret_cast<const VkSamplerYcbcrConversionInfo*>(p)->conversion;
            const auto found = conversions_.find(DeviceObjectKey::make(device, handle));
            if (found != conversions_.end()) conversion = found->second;
        }
    auto record = std::make_shared<MultiviewViewResources>(device, info,
        image == images_.end() ? nullptr : image->second, std::move(conversion));
    auto& live = live_views_[device]; prune(live); live.push_back(record);
    views_[DeviceObjectKey::make(device, view)] = std::move(record);
}
void MultiviewAttachments::remember_framebuffer(VkDevice device, VkFramebuffer fb, const VkFramebufferCreateInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    MultiviewFramebufferResources::Views sources;
    if (!(info.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))
        for (uint32_t i = 0; i < info.attachmentCount; ++i) {
            const auto found = views_.find(DeviceObjectKey::make(device, info.pAttachments[i]));
            sources.push_back(found == views_.end() ? nullptr : found->second);
        }
    auto record = std::make_shared<MultiviewFramebufferResources>(device, info, std::move(sources));
    auto& live = live_framebuffers_[device]; prune(live); live.push_back(record);
    framebuffers_[DeviceObjectKey::make(device, fb)] = std::move(record);
}
void MultiviewAttachments::remember_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    swapchains_[DeviceObjectKey::make(device, swapchain)] = {info.imageArrayLayers, {}};
}
void MultiviewAttachments::remember_swapchain_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t count, const VkImage* images) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = swapchains_.find(DeviceObjectKey::make(device, swapchain));
    if (found == swapchains_.end()) return;
    auto& chain = found->second;
    for (uint32_t i = 0; i < count; ++i) {
        if (std::any_of(chain.images.begin(), chain.images.end(), [&](const auto& image) { return image->handle == images[i]; })) continue;
        auto record = std::make_shared<MultiviewImage>(MultiviewImage{images[i], VK_IMAGE_TYPE_2D, 1, chain.layers, 1});
        chain.images.push_back(record);
        try { images_[DeviceObjectKey::make(device, images[i])] = std::move(record); }
        catch (...) { chain.images.pop_back(); throw; }
    }
}
std::shared_ptr<MultiviewViewResources> MultiviewAttachments::acquire_view(VkDevice device, VkImageView view) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = views_.find(DeviceObjectKey::make(device, view));
    return found == views_.end() ? nullptr : found->second;
}
std::shared_ptr<MultiviewFramebufferResources> MultiviewAttachments::acquire_framebuffer(VkDevice device, VkFramebuffer fb) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = framebuffers_.find(DeviceObjectKey::make(device, fb));
    return found == framebuffers_.end() ? nullptr : found->second;
}
VkResult MultiviewAttachments::native_framebuffer(VkDevice device, const VkRenderPassBeginInfo& begin,
    uint32_t subpass, uint32_t view, bool first, bool last, bool aspects, VkFramebuffer& result) {
    result = VK_NULL_HANDLE;
    try {
        auto pass = MultiviewRenderPasses::get().acquire(device, begin.renderPass);
        std::shared_ptr<MultiviewFramebufferResources> fb;
        MultiviewFramebufferResources::Views sources;
        const auto status = capture_framebuffer(device, begin, fb, sources);
        if (status != VK_SUCCESS) return status;
        return fb->native_framebuffer(pass, subpass, view, first, last, aspects, sources, result);
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult MultiviewAttachments::capture_framebuffer(VkDevice device, const VkRenderPassBeginInfo& begin,
    std::shared_ptr<MultiviewFramebufferResources>& fb, MultiviewFramebufferResources::Views& sources) {
    try {
        sources.clear(); fb.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = framebuffers_.find(DeviceObjectKey::make(device, begin.framebuffer));
            if (found == framebuffers_.end()) return VK_ERROR_FEATURE_NOT_PRESENT;
            fb = found->second;
            if (!fb->imageless()) sources = fb->attachments();
            else {
                const VkRenderPassAttachmentBeginInfo* attachments = nullptr;
                for (auto p = static_cast<const VkBaseInStructure*>(begin.pNext); p; p = p->pNext)
                    if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                        attachments = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(p);
                if (fb->attachment_count() && (!attachments || attachments->attachmentCount != fb->attachment_count()))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                if (attachments) for (uint32_t i = 0; i < attachments->attachmentCount; ++i) {
                    const auto v = views_.find(DeviceObjectKey::make(device, attachments->pAttachments[i]));
                    sources.push_back(v == views_.end() ? nullptr : v->second);
                }
            }
        }
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
void MultiviewAttachments::invalidate_view_locked(VkDevice device, const std::shared_ptr<MultiviewViewResources>& view) {
    const auto found = live_framebuffers_.find(device);
    // Ordinary legacy rendering creates no slices. Avoid walking every live
    // framebuffer whenever such an application destroys an ordinary view.
    if (view->has_slices() && found != live_framebuffers_.end())
        for (auto& weak : found->second) if (auto fb = weak.lock()) fb->invalidate(view);
    view->release();
}
void MultiviewAttachments::invalidate_image_locked(VkDevice device, const std::shared_ptr<MultiviewImage>& image) {
    const auto found = live_views_.find(device);
    if (found != live_views_.end())
        for (auto& weak : found->second) if (auto view = weak.lock())
            if (view->image() == image) invalidate_view_locked(device, view);
}
void MultiviewAttachments::forget_view(VkDevice device, VkImageView view) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = views_.find(DeviceObjectKey::make(device, view));
    if (found != views_.end()) { invalidate_view_locked(device, found->second); views_.erase(found); }
}
void MultiviewAttachments::forget_image(VkDevice device, VkImage image) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = images_.find(DeviceObjectKey::make(device, image));
    if (found != images_.end()) { invalidate_image_locked(device, found->second); images_.erase(found); }
}
void MultiviewAttachments::forget_conversion(VkDevice device, VkSamplerYcbcrConversion conversion) {
    std::lock_guard<std::mutex> lock(mutex_);
    conversions_.erase(DeviceObjectKey::make(device, conversion));
}
void MultiviewAttachments::forget_framebuffer(VkDevice device, VkFramebuffer fb) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = framebuffers_.find(DeviceObjectKey::make(device, fb));
    if (found != framebuffers_.end()) { found->second->release(); framebuffers_.erase(found); }
}
void MultiviewAttachments::forget_swapchain(VkDevice device, VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = swapchains_.find(DeviceObjectKey::make(device, swapchain));
    if (found == swapchains_.end()) return;
    for (const auto& image : found->second.images) {
        invalidate_image_locked(device, image);
        const auto current = images_.find(DeviceObjectKey::make(device, image->handle));
        if (current != images_.end() && current->second == image) images_.erase(current);
    }
    swapchains_.erase(found);
}
void MultiviewAttachments::forget_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto frames = live_framebuffers_.find(device);
    if (frames != live_framebuffers_.end()) {
        for (auto& weak : frames->second) if (auto fb = weak.lock()) fb->release();
        live_framebuffers_.erase(frames);
    }
    const auto views = live_views_.find(device);
    if (views != live_views_.end()) {
        for (auto& weak : views->second) if (auto view = weak.lock()) view->release();
        live_views_.erase(views);
    }
    auto erase = [device](auto& map) {
        for (auto it = map.begin(); it != map.end();) if (it->first.device == device) it = map.erase(it); else ++it;
    };
    erase(framebuffers_); erase(views_); erase(images_); erase(conversions_); erase(swapchains_);
}
}

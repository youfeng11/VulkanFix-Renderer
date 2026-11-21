#include "volume_attachment.h"
#include "driver_loader.h"
#include "native_instance_state.h"
#include <algorithm>
#include <new>

namespace vkfix {
namespace {
template<class T> T proc(VkDevice device, const char* name) {
    return reinterpret_cast<T>(get_real_proc(VK_NULL_HANDLE,device,name));
}
uint32_t mip_size(uint32_t size, uint32_t mip) { return mip < 32 ? std::max(1u,size >> mip) : 1u; }
VkImageAspectFlags barrier_aspects(VkFormat format, VkImageAspectFlags aspects) {
    switch (format) {
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return aspects;
    }
}
}
VkResult create_replay_framebuffer(VkDevice device, const VkFramebufferCreateInfo* original,
    const VkAllocationCallbacks* allocator, VkFramebuffer* output) {
    if (!original || !output) return VK_ERROR_INITIALIZATION_FAILED;
    *output = VK_NULL_HANDLE;
    const auto create_pass = proc<PFN_vkCreateRenderPass>(device,"vkCreateRenderPass");
    const auto destroy_pass = proc<PFN_vkDestroyRenderPass>(device,"vkDestroyRenderPass");
    const auto create_frame = proc<PFN_vkCreateFramebuffer>(device,"vkCreateFramebuffer");
    if (!create_pass || !destroy_pass || !create_frame) return VK_ERROR_INITIALIZATION_FAILED;
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; info.subpassCount = 1; info.pSubpasses = &subpass;
    VkRenderPass pass{}; auto status = create_pass(device,&info,nullptr,&pass);
    if (status != VK_SUCCESS) return status;
    if (!pass) return VK_ERROR_INITIALIZATION_FAILED;
    VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; frame.renderPass = pass;
    frame.width = original->width; frame.height = original->height; frame.layers = original->layers;
    status = create_frame(device,&frame,allocator,output);
    destroy_pass(device,pass,nullptr);
    if (status != VK_SUCCESS) *output = VK_NULL_HANDLE;
    return status;
}
VkResult VolumeAttachmentPlan::capture(const MultiviewImage& image, const VkImageViewCreateInfo& view,
    VkExtent2D extent, uint32_t layers, VolumeAttachmentPlan& output) {
    const auto& range = view.subresourceRange;
    if (image.type != VK_IMAGE_TYPE_3D || image.samples != VK_SAMPLE_COUNT_1_BIT || view.image != image.handle ||
        (view.viewType != VK_IMAGE_VIEW_TYPE_2D && view.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY) ||
        range.baseMipLevel >= image.mip_levels ||
        (range.levelCount != 1 && !(range.levelCount == VK_REMAINING_MIP_LEVELS && range.baseMipLevel + 1 == image.mip_levels)) ||
        !layers || !extent.width || !extent.height || !range.aspectMask || view.flags ||
        extent.width > mip_size(image.width,range.baseMipLevel) || extent.height > mip_size(image.height,range.baseMipLevel))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto depth = mip_size(image.depth,range.baseMipLevel);
    if (range.baseArrayLayer >= depth || layers > depth - range.baseArrayLayer ||
        (range.layerCount != VK_REMAINING_ARRAY_LAYERS && layers > range.layerCount) ||
        (view.viewType == VK_IMAGE_VIEW_TYPE_2D && layers != 1)) return VK_ERROR_FEATURE_NOT_PRESENT;
    for (auto node = static_cast<const VkBaseInStructure*>(view.pNext); node; node = node->pNext)
        if (node->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) return VK_ERROR_FEATURE_NOT_PRESENT;
    VolumeAttachmentPlan plan;
    plan.volume = image; plan.format = view.format; plan.aspects = range.aspectMask;
    plan.aspects = barrier_aspects(plan.format,plan.aspects);
    plan.mip = range.baseMipLevel; plan.base_slice = range.baseArrayLayer;
    plan.layers = layers; plan.extent = extent;
    output = plan;
    return VK_SUCCESS;
}
VkResult VolumeAttachmentPlan::region(VkRect2D area, uint32_t first, uint32_t count,
    VkImageAspectFlags aspects, bool importing, VkImageCopy& output) const {
    if (first > layers || count > layers - first || !aspects || (aspects & ~this->aspects) ||
        area.offset.x < 0 || area.offset.y < 0 || uint32_t(area.offset.x) > extent.width ||
        uint32_t(area.offset.y) > extent.height || area.extent.width > extent.width - uint32_t(area.offset.x) ||
        area.extent.height > extent.height - uint32_t(area.offset.y)) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImageCopy copy{};
    copy.srcSubresource = {aspects,mip,0,1}; copy.dstSubresource = {aspects,0,first,count};
    copy.srcOffset = {area.offset.x,area.offset.y,int32_t(base_slice + first)};
    copy.dstOffset = {area.offset.x,area.offset.y,0};
    copy.extent = {area.extent.width,area.extent.height,count};
    if (!importing) { std::swap(copy.srcSubresource,copy.dstSubresource); std::swap(copy.srcOffset,copy.dstOffset); }
    output = copy; return VK_SUCCESS;
}
VolumeAttachment::VolumeAttachment(VkDevice device, VkPhysicalDevice physical, const VolumeAttachmentPlan& plan)
    : device_(device), physical_(physical), plan_(plan) {
    destroy_image_ = proc<PFN_vkDestroyImage>(device,"vkDestroyImage");
    free_ = proc<PFN_vkFreeMemory>(device,"vkFreeMemory");
    create_view_ = proc<PFN_vkCreateImageView>(device,"vkCreateImageView");
    destroy_view_ = proc<PFN_vkDestroyImageView>(device,"vkDestroyImageView");
    barrier_ = proc<PFN_vkCmdPipelineBarrier>(device,"vkCmdPipelineBarrier");
}
VolumeAttachment::~VolumeAttachment() { release(); }
void VolumeAttachment::release() {
    if (!device_) return;
    for (const auto& view : views_) destroy_view_(device_,view.second,nullptr);
    views_.clear();
    if (image_.handle) destroy_image_(device_,image_.handle,nullptr);
    if (memory_) free_(device_,memory_,nullptr);
    image_.handle = VK_NULL_HANDLE; memory_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE;
}
VkResult VolumeAttachment::create(VkDevice device, VkPhysicalDevice physical, const VolumeAttachmentPlan& plan,
    std::unique_ptr<VolumeAttachment>& output, PFN_vkGetImageMemoryRequirements2 requirements2) {
    try {
        auto backing = std::unique_ptr<VolumeAttachment>(new VolumeAttachment(device,physical,plan));
        const auto status = backing->allocate(requirements2);
        if (status != VK_SUCCESS) return status;
        output = std::move(backing); return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult VolumeAttachment::allocate(PFN_vkGetImageMemoryRequirements2 requirements2) {
    const auto create = proc<PFN_vkCreateImage>(device_,"vkCreateImage");
    const auto requirements = proc<PFN_vkGetImageMemoryRequirements>(device_,"vkGetImageMemoryRequirements");
    const auto allocate = proc<PFN_vkAllocateMemory>(device_,"vkAllocateMemory");
    const auto bind = proc<PFN_vkBindImageMemory>(device_,"vkBindImageMemory");
    auto instance = native_instance_for_physical(physical_); if (!instance) instance = get_last_instance();
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_real_proc(instance,VK_NULL_HANDLE,"vkGetPhysicalDeviceMemoryProperties"));
    if (!create || !requirements || !allocate || !bind || !properties || !destroy_image_ || !free_ ||
        !create_view_ || !destroy_view_ || !barrier_) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = plan_.format; info.extent = {plan_.extent.width,plan_.extent.height,1};
    info.arrayLayers = plan_.layers; info.mipLevels = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    const bool color = (plan_.aspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
        (color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    VkImage candidate{}; auto status = create(device_,&info,nullptr,&candidate);
    if (status != VK_SUCCESS) return status;
    if (!candidate) return VK_ERROR_INITIALIZATION_FAILED;
    image_ = {candidate,VK_IMAGE_TYPE_2D,1,plan_.layers,1,plan_.format,VK_SAMPLE_COUNT_1_BIT,info.usage,plan_.extent.width,plan_.extent.height};
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 needed{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,&dedicated};
    if (requirements2) {
        VkImageMemoryRequirementsInfo2 query{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,nullptr,candidate};
        requirements2(device_,&query,&needed);
    } else requirements(device_,candidate,&needed.memoryRequirements);
    VkPhysicalDeviceMemoryProperties types{}; properties(physical_,&types);
    uint32_t selected = UINT32_MAX;
    for (uint32_t i = 0; i < types.memoryTypeCount; ++i) if (needed.memoryRequirements.memoryTypeBits & (1u << i)) {
        if (selected == UINT32_MAX) selected = i;
        if (types.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { selected = i; break; }
    }
    if (selected == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryDedicatedAllocateInfo identity{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; identity.image = candidate;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = needed.memoryRequirements.size; allocation.memoryTypeIndex = selected;
    if (dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation) allocation.pNext = &identity;
    VkDeviceMemory candidate_memory{}; status = allocate(device_,&allocation,nullptr,&candidate_memory);
    if (status != VK_SUCCESS) return status;
    if (!candidate_memory) return VK_ERROR_INITIALIZATION_FAILED;
    memory_ = candidate_memory;
    return bind(device_,candidate,memory_,0);
}
VkResult VolumeAttachment::view(uint32_t first, uint32_t count, VkImageAspectFlags aspects, VkImageViewType type, VkImageView& output) {
    output = VK_NULL_HANDLE;
    if (!device_ || !count || first >= plan_.layers || count > plan_.layers - first || !aspects || (aspects & ~plan_.aspects) ||
        (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) || (type == VK_IMAGE_VIEW_TYPE_2D && count != 1))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto key = std::make_tuple(first,count,aspects,type);
    const auto found = views_.find(key); if (found != views_.end()) { output = found->second; return VK_SUCCESS; }
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; info.image = image_.handle;
    info.format = plan_.format; info.viewType = type; info.subresourceRange = {aspects,0,1,first,count};
    VkImageView native{}; const auto status = create_view_(device_,&info,nullptr,&native);
    if (status != VK_SUCCESS) return status;
    if (!native) return VK_ERROR_INITIALIZATION_FAILED;
    try { views_.emplace(key,native); }
    catch (const std::bad_alloc&) { destroy_view_(device_,native,nullptr); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    output = native; return VK_SUCCESS;
}
VkResult VolumeAttachment::initialize(VkCommandBuffer cmd) {
    if (!device_ || initialized_) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    image.srcAccessMask = image.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    image.image = image_.handle; image.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; image.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.subresourceRange = {barrier_aspects(plan_.format,plan_.aspects),0,1,0,plan_.layers};
    barrier_(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&image);
    initialized_ = true; return VK_SUCCESS;
}
VkResult VolumeAttachment::transfer(VkCommandBuffer cmd, VkImageLayout layout, VkRect2D area,
    uint32_t first, uint32_t count, VkImageAspectFlags aspects, bool importing) {
    if (!device_ || !initialized_) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageCopy region{}; auto status = plan_.region(area,first,count,aspects,importing,region);
    if (status != VK_SUCCESS || !count || !area.extent.width || !area.extent.height) return status;
    VkImageMemoryBarrier images[2]{};
    for (auto& image : images) {
        image.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        image.srcAccessMask = image.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    images[0].image = plan_.volume.handle; images[0].oldLayout = images[0].newLayout = layout;
    images[0].subresourceRange = {barrier_aspects(plan_.volume.format,plan_.aspects),plan_.mip,1,0,1};
    images[1].image = image_.handle; images[1].oldLayout = images[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[1].subresourceRange = {barrier_aspects(plan_.format,plan_.aspects),0,1,first,count};
    barrier_(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,images);
    status = importing ? ImageTypeCopy::get().copy(device_,physical_,cmd,plan_.volume,layout,image_,VK_IMAGE_LAYOUT_GENERAL,1,&region) :
        ImageTypeCopy::get().copy(device_,physical_,cmd,image_,VK_IMAGE_LAYOUT_GENERAL,plan_.volume,layout,1,&region);
    if (status == VK_SUCCESS)
        barrier_(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,images);
    return status;
}
VkResult VolumeAttachment::import(VkCommandBuffer cmd, VkImageLayout layout, VkRect2D area,
    uint32_t first, uint32_t count, VkImageAspectFlags aspects) { return transfer(cmd,layout,area,first,count,aspects,true); }
VkResult VolumeAttachment::store(VkCommandBuffer cmd, VkImageLayout layout, VkRect2D area,
    uint32_t first, uint32_t count, VkImageAspectFlags aspects) { return transfer(cmd,layout,area,first,count,aspects,false); }
}

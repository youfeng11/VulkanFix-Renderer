#include "core/volume_attachment.h"
#include "core/driver_loader.h"
#include <cstdio>
#include <cstring>
#include <set>

using namespace vkfix;
namespace {
template<class T> T handle(uint64_t n) { return (T)(uintptr_t)n; }
unsigned failures = 0, checks = 0, serial = 1000, invalid_destroy = 0;
VkResult create_status = VK_SUCCESS, allocate_status = VK_SUCCESS, bind_status = VK_SUCCESS, view_status = VK_SUCCESS;
VkImageCreateInfo last_image{};
VkImageViewCreateInfo last_view{};
VkImageAspectFlags last_transition_aspects{};
std::set<VkImage> images;
std::set<VkDeviceMemory> memory;
std::map<VkImageView,VkImage> views;
bool require_dedicated = false, saw_dedicated = false;
unsigned commands = 0;
void check(bool okay, const char* text) { ++checks; printf("%s: %s\n", okay ? "PASS" : "FAIL", text); failures += !okay; }
VkResult VKAPI_CALL create_image(VkDevice, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* out) {
    check(!allocator && !info->pNext && !info->flags && info->imageType == VK_IMAGE_TYPE_2D && info->arrayLayers == 40 &&
        info->extent.width == 7 && info->extent.height == 5 && info->extent.depth == 1 && info->mipLevels == 1 &&
        (info->usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
        (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT), "backing is an ordinary native2D array bounded by the framebuffer");
    last_image = *info;
    if (create_status != VK_SUCCESS) { *out = handle<VkImage>(0xdead); return create_status; }
    *out = handle<VkImage>(++serial); images.insert(*out); return VK_SUCCESS;
}
void VKAPI_CALL destroy_image(VkDevice, VkImage image, const VkAllocationCallbacks*) {
    for (const auto& view : views) invalid_destroy += view.second == image;
    invalid_destroy += images.erase(image) != 1;
}
void VKAPI_CALL requirements(VkDevice, VkImage, VkMemoryRequirements* out) { *out = {8192,256,3}; }
void VKAPI_CALL requirements2(VkDevice device, const VkImageMemoryRequirementsInfo2* info, VkMemoryRequirements2* out) {
    requirements(device,info->image,&out->memoryRequirements);
    auto dedicated = static_cast<VkMemoryDedicatedRequirements*>(out->pNext);
    dedicated->requiresDedicatedAllocation = require_dedicated;
}
void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    *out = {}; out->memoryTypeCount = 2; out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
VkResult VKAPI_CALL allocate(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator, VkDeviceMemory* out) {
    check(!allocator && info->allocationSize == 8192 && info->memoryTypeIndex == 1, "private allocation chooses a permitted device-local memory type");
    saw_dedicated = false;
    if (info->pNext) {
        const auto* dedicated = static_cast<const VkMemoryDedicatedAllocateInfo*>(info->pNext);
        saw_dedicated = dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO && images.count(dedicated->image) && !dedicated->buffer;
    }
    if (allocate_status != VK_SUCCESS) { *out = handle<VkDeviceMemory>(0xbeef); return allocate_status; }
    *out = handle<VkDeviceMemory>(++serial); memory.insert(*out); return VK_SUCCESS;
}
void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory value, const VkAllocationCallbacks*) { invalid_destroy += memory.erase(value) != 1; }
VkResult VKAPI_CALL bind(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return bind_status; }
VkResult VKAPI_CALL create_view(VkDevice, const VkImageViewCreateInfo* info, const VkAllocationCallbacks*, VkImageView* out) {
    last_view = *info;
    if (view_status != VK_SUCCESS) { *out = handle<VkImageView>(0xfeed); return view_status; }
    *out = handle<VkImageView>(++serial); views[*out] = info->image; return VK_SUCCESS;
}
void VKAPI_CALL destroy_view(VkDevice, VkImageView view, const VkAllocationCallbacks*) { invalid_destroy += views.erase(view) != 1; }
void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
    uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t count, const VkImageMemoryBarrier* images) {
    ++commands; if (count) last_transition_aspects = images[0].subresourceRange.aspectMask;
}
}
extern "C" VkInstance get_last_instance() { return {}; }
extern "C" void* get_real_proc(VkInstance, VkDevice, const char* name) {
#define PROC(n,f) if (!strcmp(name,#n)) return (void*)f
    PROC(vkCreateImage,create_image); PROC(vkDestroyImage,destroy_image); PROC(vkGetImageMemoryRequirements,requirements);
    PROC(vkGetPhysicalDeviceMemoryProperties,properties); PROC(vkAllocateMemory,allocate); PROC(vkFreeMemory,free_memory);
    PROC(vkBindImageMemory,bind); PROC(vkCreateImageView,create_view); PROC(vkDestroyImageView,destroy_view);
    PROC(vkCmdPipelineBarrier,barrier);
#undef PROC
    return nullptr;
}
int main() {
    const auto device = handle<VkDevice>(10), other_device = handle<VkDevice>(11);
    const auto physical = handle<VkPhysicalDevice>(12);
    const auto cmd = handle<VkCommandBuffer>(13);
    MultiviewImage volume{handle<VkImage>(42),VK_IMAGE_TYPE_3D,2,1,96,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,16,12};
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = volume.handle;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; view.format = volume.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,1,1,5,VK_REMAINING_ARRAY_LAYERS};
    VolumeAttachmentPlan plan;
    check(VolumeAttachmentPlan::capture(volume,view,{7,5},40,plan) == VK_SUCCESS && plan.base_slice == 5 && plan.mip == 1 && plan.layers == 40,
        "capture nonzero mip/base slice, remaining layers, and more than32 ordinary layers");
    auto denied = plan;
    check(VolumeAttachmentPlan::capture(volume,view,{9,5},40,denied) == VK_ERROR_FEATURE_NOT_PRESENT && denied.extent.width == 7,
        "failed capture leaves the previous plan intact");
    check(VolumeAttachmentPlan::capture(volume,view,{7,5},44,denied) == VK_ERROR_FEATURE_NOT_PRESENT,
        "requested framebuffer layers cannot exceed the selected mip depth");
    VkImageCopy copy{}; const VkRect2D area{{2,1},{3,2}};
    check(plan.region(area,37,3,VK_IMAGE_ASPECT_COLOR_BIT,true,copy) == VK_SUCCESS && copy.srcSubresource.mipLevel == 1 &&
        copy.srcOffset.z == 42 && copy.dstSubresource.baseArrayLayer == 37 && copy.dstSubresource.layerCount == 3 &&
        copy.srcSubresource.layerCount == 1 && copy.extent.depth == 3 && copy.srcOffset.x == 2 && copy.dstOffset.y == 1,
        "imports preserve rectangle, volume mip/depth and private array slice offsets");
    check(plan.region(area,37,3,VK_IMAGE_ASPECT_COLOR_BIT,false,copy) == VK_SUCCESS && copy.dstSubresource.mipLevel == 1 &&
        copy.dstOffset.z == 42 && copy.srcSubresource.baseArrayLayer == 37 && copy.srcSubresource.layerCount == 3,
        "stores reverse the copy mapping without broadening its extent");
    const auto saved = copy;
    check(plan.region(area,39,2,VK_IMAGE_ASPECT_COLOR_BIT,false,copy) == VK_ERROR_FEATURE_NOT_PRESENT && !memcmp(&saved,&copy,sizeof copy),
        "invalid slice run is rejected without publishing a partial region");
    check(plan.region({{-1,0},{1,1}},0,1,VK_IMAGE_ASPECT_COLOR_BIT,true,copy) == VK_ERROR_FEATURE_NOT_PRESENT &&
        plan.region({{6,4},{2,1}},0,1,VK_IMAGE_ASPECT_COLOR_BIT,true,copy) == VK_ERROR_FEATURE_NOT_PRESENT &&
        plan.region(area,0,1,VK_IMAGE_ASPECT_DEPTH_BIT,true,copy) == VK_ERROR_FEATURE_NOT_PRESENT,
        "negative/out-of-bounds render areas and unsupported aspects cannot produce copies");
    VkImageMemoryBarrier logical_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    logical_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; logical_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    logical_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,1,1,0,VK_REMAINING_ARRAY_LAYERS};
    auto native_barrier = logical_barrier;
    auto emulated_volume = volume; emulated_volume.emulated_attachment = true;
    check(lower_volume_barrier(emulated_volume,native_barrier) && native_barrier.oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
        native_barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL && native_barrier.subresourceRange.baseArrayLayer == 0 &&
        native_barrier.subresourceRange.layerCount == 1 && logical_barrier.subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS,
        "legacy volume barriers use native GENERAL and its single array layer while retaining the logical caller range");
    native_barrier = logical_barrier;
    check(!lower_volume_barrier(volume,native_barrier) && !memcmp(&native_barrier,&logical_barrier,sizeof native_barrier),
        "native images retain their original barrier semantics");
    std::unique_ptr<VolumeAttachment> first, second;
    check(VolumeAttachment::create(device,physical,plan,first) == VK_SUCCESS && first && !saw_dedicated,
        "legacy creation uses core1.0 memory requirements");
    check(VolumeAttachment::create(device,physical,plan,second) == VK_SUCCESS && second->image().handle != first->image().handle,
        "recordings of the same public volume own separate native backings");
    VkImageView whole{}, slice{}, again{};
    check(first->view(0,40,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_VIEW_TYPE_2D_ARRAY,whole) == VK_SUCCESS &&
        last_view.subresourceRange.layerCount == 40 && last_view.subresourceRange.baseMipLevel == 0,
        "ordinary layered framebuffer receives the whole private array");
    check(first->view(39,1,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_VIEW_TYPE_2D,slice) == VK_SUCCESS &&
        last_view.subresourceRange.baseArrayLayer == 39 && first->view(39,1,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_VIEW_TYPE_2D,again) == VK_SUCCESS && slice == again,
        "sparse multiview slices retain their index and reuse only the same private view");
    check(first->view(39,2,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_VIEW_TYPE_2D_ARRAY,again) == VK_ERROR_FEATURE_NOT_PRESENT && !again,
        "view creation never exposes layers beyond this framebuffer's backing");
    check(first->initialize(cmd) == VK_SUCCESS && commands == 1 && first->initialize(cmd) == VK_ERROR_INITIALIZATION_FAILED,
        "backing initialization records one transition per recording");
    check(first->import(cmd,VK_IMAGE_LAYOUT_GENERAL,{{0,0},{0,5}},0,40,VK_IMAGE_ASPECT_COLOR_BIT) == VK_SUCCESS && commands == 1,
        "empty render area emits no transfer or additional dependency");
    first.reset(); second.reset();
    check(images.empty() && memory.empty() && views.empty() && !invalid_destroy, "retire all views before their image, then free its memory exactly once");
    for (unsigned failure = 0; failure < 3; ++failure) {
        create_status = failure == 0 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
        allocate_status = failure == 1 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
        bind_status = failure == 2 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
        check(VolumeAttachment::create(device,physical,plan,first) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !first &&
            images.empty() && memory.empty() && !invalid_destroy, "native create/allocate/bind failure does not own garbage output handles or leak partial resources");
    }
    create_status = allocate_status = bind_status = VK_SUCCESS;
    require_dedicated = true;
    check(VolumeAttachment::create(device,physical,plan,first,requirements2) == VK_SUCCESS && saw_dedicated,
        "enabled native dedicated allocation requirements name the private image in allocation pNext");
    view_status = VK_ERROR_OUT_OF_HOST_MEMORY;
    check(first->view(0,1,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_VIEW_TYPE_2D,again) == VK_ERROR_OUT_OF_HOST_MEMORY && !again && views.empty(),
        "failed native view output is never cached or destroyed");
    view_status = VK_SUCCESS;
    first.reset();
    auto depth_plan = plan; depth_plan.format = depth_plan.volume.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    depth_plan.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
    check(VolumeAttachment::create(device,physical,depth_plan,first) == VK_SUCCESS &&
        first->initialize(cmd) == VK_SUCCESS && last_transition_aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) &&
        (last_image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
        "core1.1 combined depth/stencil backing transitions both aspects even for a depth-only view");
    first->release(); first->release(); first.reset();
    check(images.empty() && memory.empty() && views.empty() && !invalid_destroy, "device teardown releases retained resources idempotently");
    // Internal copies cannot silently miss their command-buffer lifetime owner.
    check(ImageTypeCopy::get().copy(device,physical,cmd,volume,VK_IMAGE_LAYOUT_GENERAL,volume,VK_IMAGE_LAYOUT_GENERAL,0,nullptr) == VK_ERROR_INITIALIZATION_FAILED,
        "internal copy requires a live recording");
    check(ImageTypeCopy::get().begin(device,cmd,{}) == VK_SUCCESS &&
        ImageTypeCopy::get().copy(other_device,physical,cmd,volume,VK_IMAGE_LAYOUT_GENERAL,volume,VK_IMAGE_LAYOUT_GENERAL,0,nullptr) == VK_ERROR_INITIALIZATION_FAILED,
        "internal metadata cannot move a scratch buffer to another logical device");
    ImageTypeCopy::get().forget(cmd);
    printf("%u checks, %u failures\n",checks,failures); return failures ? 1 : 0;
}

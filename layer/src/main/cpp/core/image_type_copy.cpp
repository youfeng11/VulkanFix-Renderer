#include "image_type_copy.h"
#include "driver_loader.h"
#include "native_instance_state.h"
#include <algorithm>
#include <limits>
#include <numeric>
#include <new>
#include <vulkan/vulkan_format_traits.hpp>

namespace vkfix {
namespace {
VkFormat aspect_format(VkFormat format, VkImageAspectFlags aspect) {
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) return VK_FORMAT_S8_UINT;
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
        if (format == VK_FORMAT_D16_UNORM_S8_UINT) return VK_FORMAT_D16_UNORM;
        if (format == VK_FORMAT_D24_UNORM_S8_UINT) return VK_FORMAT_X8_D24_UNORM_PACK32;
        if (format == VK_FORMAT_D32_SFLOAT_S8_UINT) return VK_FORMAT_D32_SFLOAT;
    }
    if (aspect & (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) {
        const uint8_t plane = aspect == VK_IMAGE_ASPECT_PLANE_0_BIT ? 0 : aspect == VK_IMAGE_ASPECT_PLANE_1_BIT ? 1 : 2;
        return VkFormat(vk::planeCompatibleFormat(vk::Format(format), plane));
    }
    return format;
}
uint32_t mip_size(uint32_t base, uint32_t mip) { return mip < 32 ? std::max(1u, base >> mip) : 1u; }
uint64_t ceil_div(uint64_t value, uint32_t divisor) { return value / divisor + (value % divisor != 0); }
bool multiply(uint64_t& value, uint64_t factor) {
    if (factor && value > UINT64_MAX / factor) return false;
    value *= factor; return true;
}
template<class T> T proc(VkDevice device, const char* name) {
    return reinterpret_cast<T>(get_real_proc(VK_NULL_HANDLE,device,name));
}
struct Scratch {
    VkDevice device;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    PFN_vkDestroyBuffer destroy{};
    PFN_vkFreeMemory free{};
    explicit Scratch(VkDevice value) : device(value), destroy(proc<PFN_vkDestroyBuffer>(value,"vkDestroyBuffer")),
        free(proc<PFN_vkFreeMemory>(value,"vkFreeMemory")) {}
    ~Scratch() {
        if (buffer && destroy) destroy(device,buffer,nullptr);
        if (memory && free) free(device,memory,nullptr);
    }
    VkResult create(VkPhysicalDevice physical, VkDeviceSize bytes) {
        const auto create_buffer = proc<PFN_vkCreateBuffer>(device,"vkCreateBuffer");
        const auto requirements = proc<PFN_vkGetBufferMemoryRequirements>(device,"vkGetBufferMemoryRequirements");
        const auto allocate = proc<PFN_vkAllocateMemory>(device,"vkAllocateMemory");
        const auto bind = proc<PFN_vkBindBufferMemory>(device,"vkBindBufferMemory");
        auto instance = native_instance_for_physical(physical); if (!instance) instance = get_last_instance();
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_real_proc(instance,VK_NULL_HANDLE,"vkGetPhysicalDeviceMemoryProperties"));
        if (!destroy || !free || !create_buffer || !requirements || !allocate || !bind || !properties) return VK_ERROR_INITIALIZATION_FAILED;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer candidate{}; auto status = create_buffer(device,&info,nullptr,&candidate);
        if (status != VK_SUCCESS) return status;
        if (!candidate) return VK_ERROR_INITIALIZATION_FAILED;
        buffer = candidate;
        VkMemoryRequirements needed{}; requirements(device,buffer,&needed);
        VkPhysicalDeviceMemoryProperties types{}; properties(physical,&types);
        uint32_t selected = UINT32_MAX;
        for (uint32_t i = 0; i < types.memoryTypeCount; ++i) if (needed.memoryTypeBits & (1u << i)) {
            if (selected == UINT32_MAX) selected = i;
            if (types.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { selected = i; break; }
        }
        if (selected == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = needed.size;
        allocation.memoryTypeIndex = selected;
        VkDeviceMemory candidate_memory{}; status = allocate(device,&allocation,nullptr,&candidate_memory);
        if (status != VK_SUCCESS) return status;
        if (!candidate_memory) return VK_ERROR_INITIALIZATION_FAILED;
        memory = candidate_memory;
        return bind(device,buffer,memory,0);
    }
};
}

VkResult plan_image_type_copy(const MultiviewImage& source, const MultiviewImage& target,
                             uint32_t count, const VkImageCopy* regions, ImageTypeCopyPlan& output) {
    try {
        ImageTypeCopyPlan plan;
        for (uint32_t r = 0; r < count; ++r) {
            const auto& region = regions[r];
            VkImageAspectFlags aspects[]{region.srcSubresource.aspectMask,0};
            if (aspects[0] == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
                if (region.dstSubresource.aspectMask != aspects[0]) return VK_ERROR_FEATURE_NOT_PRESENT;
                aspects[0] = VK_IMAGE_ASPECT_DEPTH_BIT; aspects[1] = VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            for (const auto aspect : aspects) if (aspect) {
                const auto dst_aspect = aspects[1] ? aspect : region.dstSubresource.aspectMask;
                const auto src_format = vk::Format(aspect_format(source.format,aspect));
                const auto dst_format = vk::Format(aspect_format(target.format,dst_aspect));
                const auto src_block = vk::blockExtent(src_format), dst_block = vk::blockExtent(dst_format);
                const auto src_bytes = vk::blockSize(src_format), dst_bytes = vk::blockSize(dst_format);
                if (!src_bytes || src_bytes != dst_bytes || !src_block[0] || !src_block[1] || !src_block[2] ||
                    !dst_block[0] || !dst_block[1] || !dst_block[2]) return VK_ERROR_FORMAT_NOT_SUPPORTED;
                VkBufferImageCopy read{}, write{};
                read.imageSubresource = region.srcSubresource; read.imageSubresource.aspectMask = aspect;
                write.imageSubresource = region.dstSubresource; write.imageSubresource.aspectMask = dst_aspect;
                read.imageOffset = region.srcOffset; write.imageOffset = region.dstOffset;
                read.imageExtent = region.extent; if (source.type == VK_IMAGE_TYPE_2D) read.imageExtent.depth = 1;
                write.imageExtent = region.extent; if (target.type == VK_IMAGE_TYPE_2D) write.imageExtent.depth = 1;
                const uint32_t target_size[]{mip_size(target.width,region.dstSubresource.mipLevel),
                    mip_size(target.height,region.dstSubresource.mipLevel), mip_size(target.depth,region.dstSubresource.mipLevel)};
                const uint32_t src_extent[]{region.extent.width, region.extent.height, read.imageExtent.depth};
                const int32_t dst_offset[]{region.dstOffset.x, region.dstOffset.y, region.dstOffset.z};
                uint32_t* dst_extent[]{&write.imageExtent.width,&write.imageExtent.height,&write.imageExtent.depth};
                for (unsigned axis = 0; axis < 3; ++axis) if (src_block[axis] != dst_block[axis]) {
                    if (axis == 2 && source.type != target.type) return VK_ERROR_FORMAT_NOT_SUPPORTED;
                    const auto mapped = ceil_div(src_extent[axis],src_block[axis]) * dst_block[axis];
                    if (dst_offset[axis] < 0 || uint32_t(dst_offset[axis]) >= target_size[axis]) return VK_ERROR_FORMAT_NOT_SUPPORTED;
                    *dst_extent[axis] = uint32_t(std::min<uint64_t>(mapped, target_size[axis] - dst_offset[axis]));
                }
                uint64_t bytes = src_bytes;
                if (!multiply(bytes,ceil_div(read.imageExtent.width,src_block[0])) ||
                    !multiply(bytes,ceil_div(read.imageExtent.height,src_block[1])) ||
                    !multiply(bytes,ceil_div(read.imageExtent.depth,src_block[2])) ||
                    !multiply(bytes,read.imageSubresource.layerCount)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                uint64_t target_bytes = dst_bytes;
                if (!multiply(target_bytes,ceil_div(write.imageExtent.width,dst_block[0])) ||
                    !multiply(target_bytes,ceil_div(write.imageExtent.height,dst_block[1])) ||
                    !multiply(target_bytes,ceil_div(write.imageExtent.depth,dst_block[2])) ||
                    !multiply(target_bytes,write.imageSubresource.layerCount) || target_bytes != bytes) return VK_ERROR_FORMAT_NOT_SUPPORTED;
                const uint64_t alignment = std::lcm(uint64_t(4),uint64_t(src_bytes));
                const auto padding = (alignment - plan.bytes % alignment) % alignment;
                if (plan.bytes > UINT64_MAX - padding || plan.bytes + padding > UINT64_MAX - bytes) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                read.bufferOffset = write.bufferOffset = plan.bytes + padding; plan.bytes = read.bufferOffset + bytes;
                plan.reads.push_back(read); plan.writes.push_back(write);
            }
        }
        output = std::move(plan); return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}

struct ImageTypeCopy::Recording {
    VkDevice device;
    VkCommandPool pool;
    VkResult error = VK_SUCCESS;
    std::mutex mutex;
    std::vector<std::unique_ptr<Scratch>> buffers;
};
ImageTypeCopy& ImageTypeCopy::get() { static ImageTypeCopy value; return value; }
std::shared_ptr<ImageTypeCopy::Recording> ImageTypeCopy::acquire(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = commands_.find(cmd); return found == commands_.end() ? nullptr : found->second;
}
VkResult ImageTypeCopy::begin(VkDevice device, VkCommandBuffer cmd, VkCommandPool pool) {
    try {
        auto record = std::make_shared<Recording>(); record->device = device; record->pool = pool;
        std::shared_ptr<Recording> retired;
        { std::lock_guard<std::mutex> lock(mutex_); auto& slot = commands_[cmd]; retired = std::move(slot); slot = std::move(record); }
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
bool ImageTypeCopy::copy(VkDevice device, VkPhysicalDevice physical, VkCommandBuffer cmd,
    VkImage source, VkImageLayout source_layout, VkImage target, VkImageLayout target_layout,
    uint32_t count, const VkImageCopy* regions) {
    auto src = MultiviewAttachments::get().acquire_image(device,source);
    auto dst = MultiviewAttachments::get().acquire_image(device,target);
    if (!src || !dst || !((src->type == VK_IMAGE_TYPE_2D && dst->type == VK_IMAGE_TYPE_3D) ||
        (src->type == VK_IMAGE_TYPE_3D && dst->type == VK_IMAGE_TYPE_2D))) return false;
    if (!acquire(cmd)) return false;
    copy(device,physical,cmd,*src,source_layout,*dst,target_layout,count,regions);
    return true;
}
VkResult ImageTypeCopy::copy(VkDevice device, VkPhysicalDevice physical, VkCommandBuffer cmd,
    const MultiviewImage& source, VkImageLayout source_layout, const MultiviewImage& target, VkImageLayout target_layout,
    uint32_t count, const VkImageCopy* regions) {
    auto record = acquire(cmd); if (!record || record->device != device) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (record->error != VK_SUCCESS || !count) return record->error;
    try {
        ImageTypeCopyPlan plan;
        record->error = plan_image_type_copy(source,target,count,regions,plan);
        if (record->error != VK_SUCCESS) return record->error;
        const auto read = proc<PFN_vkCmdCopyImageToBuffer>(device,"vkCmdCopyImageToBuffer");
        const auto write = proc<PFN_vkCmdCopyBufferToImage>(device,"vkCmdCopyBufferToImage");
        const auto barrier = proc<PFN_vkCmdPipelineBarrier>(device,"vkCmdPipelineBarrier");
        if (!read || !write || !barrier) return record->error = VK_ERROR_INITIALIZATION_FAILED;
        auto scratch = std::make_unique<Scratch>(device);
        record->error = scratch->create(physical,plan.bytes);
        if (record->error != VK_SUCCESS) return record->error;
        const auto buffer = scratch->buffer;
        record->buffers.push_back(std::move(scratch)); // Publish ownership before recording any native command.
        read(cmd,source.handle,source_layout,buffer,uint32_t(plan.reads.size()),plan.reads.data());
        VkBufferMemoryBarrier memory{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; memory.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        memory.srcQueueFamilyIndex = memory.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memory.buffer = buffer; memory.size = VK_WHOLE_SIZE;
        barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&memory,0,nullptr);
        write(cmd,buffer,target.handle,target_layout,uint32_t(plan.writes.size()),plan.writes.data());
    } catch (const std::bad_alloc&) { record->error = VK_ERROR_OUT_OF_HOST_MEMORY; }
    return record->error;
}
VkResult ImageTypeCopy::error(VkCommandBuffer cmd) {
    auto record = acquire(cmd); if (!record) return VK_SUCCESS;
    std::lock_guard<std::mutex> lock(record->mutex); return record->error;
}
void ImageTypeCopy::forget(VkCommandBuffer cmd) {
    std::shared_ptr<Recording> retired;
    { std::lock_guard<std::mutex> lock(mutex_); const auto found = commands_.find(cmd);
      if (found != commands_.end()) { retired = std::move(found->second); commands_.erase(found); } }
}
void ImageTypeCopy::forget_matching(VkDevice device, VkCommandPool pool, bool whole_device) {
    for (;;) {
        std::shared_ptr<Recording> retired;
        { std::lock_guard<std::mutex> lock(mutex_);
          auto found = std::find_if(commands_.begin(),commands_.end(),[&](const auto& item) {
              return item.second->device == device && (whole_device || item.second->pool == pool); });
          if (found == commands_.end()) return;
          retired = std::move(found->second); commands_.erase(found); }
    }
}
void ImageTypeCopy::forget_pool(VkDevice device, VkCommandPool pool) { forget_matching(device,pool,false); }
void ImageTypeCopy::forget_device(VkDevice device) { forget_matching(device,{},true); }
}

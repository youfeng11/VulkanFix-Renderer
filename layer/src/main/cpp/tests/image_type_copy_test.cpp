#include "core/image_type_copy.h"
#include "core/driver_loader.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>

static std::atomic<bool> fail_new{false};
void* operator new(size_t bytes) {
    if (fail_new.exchange(false)) throw std::bad_alloc();
    if (auto* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
using namespace vkfix;
namespace {
template<class T> T handle(uint64_t value) { return (T)(uintptr_t)value; }
unsigned failures = 0, next_handle = 1000, reads = 0, writes = 0, barriers = 0, invalid_destroy = 0;
VkResult create_status = VK_SUCCESS, allocate_status = VK_SUCCESS, bind_status = VK_SUCCESS;
std::map<VkBuffer,VkDeviceSize> buffers;
std::map<VkDeviceMemory,VkDeviceSize> memories;
void check(bool okay, const char* text) { printf("%s: %s\n", okay ? "PASS" : "FAIL", text); failures += !okay; }
VkResult VKAPI_CALL create_buffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkBuffer* out) {
    check(!allocator && info->usage == (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT), "private buffer has transfer-only usage and its own allocator");
    if (create_status != VK_SUCCESS) { *out = handle<VkBuffer>(0xdead); return create_status; }
    *out = handle<VkBuffer>(++next_handle); buffers[*out] = info->size; return VK_SUCCESS;
}
void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) { invalid_destroy += buffers.erase(buffer) != 1; }
void VKAPI_CALL requirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* out) { *out = {buffers.at(buffer), 4, 1}; }
void VKAPI_CALL memory_properties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    *out = {}; out->memoryTypeCount = 1; out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
VkResult VKAPI_CALL allocate(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* out) {
    if (allocate_status != VK_SUCCESS) { *out = handle<VkDeviceMemory>(0xbeef); return allocate_status; }
    *out = handle<VkDeviceMemory>(++next_handle); memories[*out] = info->allocationSize; return VK_SUCCESS;
}
void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) { invalid_destroy += memories.erase(memory) != 1; }
VkResult VKAPI_CALL bind(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return bind_status; }
void VKAPI_CALL read_image(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer buffer, uint32_t count, const VkBufferImageCopy* regions) {
    check(buffers.count(buffer) && !memories.empty() && count == 1 && regions[0].imageSubresource.layerCount == 3 && regions[0].imageExtent.depth == 1,
          "array read uses all layers with an effective depth of one"); ++reads;
}
void VKAPI_CALL write_image(VkCommandBuffer, VkBuffer buffer, VkImage, VkImageLayout, uint32_t count, const VkBufferImageCopy* regions) {
    check(buffers.count(buffer) && count == 1 && regions[0].imageSubresource.layerCount == 1 && regions[0].imageExtent.depth == 3,
          "volume write uses three depth slices and one array layer"); ++writes;
}
void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags,
    uint32_t, const VkMemoryBarrier*, uint32_t count, const VkBufferMemoryBarrier* memory, uint32_t, const VkImageMemoryBarrier*) {
    check(src == VK_PIPELINE_STAGE_TRANSFER_BIT && dst == VK_PIPELINE_STAGE_TRANSFER_BIT && count == 1 &&
          memory[0].srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT && memory[0].dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT &&
          memory[0].size == VK_WHOLE_SIZE, "private staging write is made visible before the image write"); ++barriers;
}
MultiviewImage image(VkImageType type, VkFormat format, uint32_t width, uint32_t height, uint32_t slices) {
    MultiviewImage value{}; value.type = type; value.format = format; value.width = width; value.height = height;
    value.layers = type == VK_IMAGE_TYPE_2D ? slices : 1; value.depth = type == VK_IMAGE_TYPE_3D ? slices : 1; value.mip_levels = 3;
    return value;
}
}
extern "C" VkInstance get_last_instance() { return {}; }
extern "C" void* get_real_proc(VkInstance, VkDevice, const char* name) {
#define PROC(command, function) if (!strcmp(name,#command)) return (void*)function
    PROC(vkCreateBuffer,create_buffer); PROC(vkDestroyBuffer,destroy_buffer);
    PROC(vkGetBufferMemoryRequirements,requirements); PROC(vkGetPhysicalDeviceMemoryProperties,memory_properties);
    PROC(vkAllocateMemory,allocate); PROC(vkFreeMemory,free_memory); PROC(vkBindBufferMemory,bind);
    PROC(vkCmdCopyImageToBuffer,read_image); PROC(vkCmdCopyBufferToImage,write_image); PROC(vkCmdPipelineBarrier,barrier);
#undef PROC
    return nullptr;
}
int main() {
    auto array = image(VK_IMAGE_TYPE_2D,VK_FORMAT_R8G8B8A8_UNORM,64,64,8);
    auto volume = image(VK_IMAGE_TYPE_3D,VK_FORMAT_R8G8B8A8_UNORM,64,64,16);
    VkImageCopy region{}; region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,1,2,3};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,1,0,1};
    region.srcOffset = {2,3,0}; region.dstOffset = {5,7,2}; region.extent = {4,3,3};
    const auto original = region;
    ImageTypeCopyPlan plan;
    check(plan_image_type_copy(array,volume,1,&region,plan) == VK_SUCCESS && plan.bytes == 144 && plan.reads.size() == 1 &&
          plan.reads[0].imageSubresource.baseArrayLayer == 2 && plan.reads[0].imageSubresource.mipLevel == 1 &&
          plan.reads[0].imageExtent.depth == 1 && plan.writes[0].imageOffset.z == 2 && plan.writes[0].imageExtent.depth == 3 &&
          !memcmp(&region,&original,sizeof region), "array-to-volume plan preserves mip levels, nonzero offsets and all source bytes");
    std::swap(region.srcSubresource,region.dstSubresource); std::swap(region.srcOffset,region.dstOffset);
    check(plan_image_type_copy(volume,array,1,&region,plan) == VK_SUCCESS && plan.bytes == 144 &&
          plan.reads[0].imageExtent.depth == 3 && plan.writes[0].imageExtent.depth == 1 &&
          plan.writes[0].imageSubresource.layerCount == 3, "volume-to-array plan preserves the inverse layer/slice mapping");
    auto compressed = image(VK_IMAGE_TYPE_2D,VK_FORMAT_BC3_UNORM_BLOCK,9,9,2);
    auto texels = image(VK_IMAGE_TYPE_3D,VK_FORMAT_R32G32B32A32_UINT,3,3,2);
    VkImageCopy blocks{}; blocks.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,2};
    blocks.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; blocks.extent = {9,9,2};
    check(plan_image_type_copy(compressed,texels,1,&blocks,plan) == VK_SUCCESS && plan.bytes == 288 &&
          plan.writes[0].imageExtent.width == 3 && plan.writes[0].imageExtent.height == 3,
          "compressed edge blocks map to size-compatible uncompressed texels");
    auto unpacked = image(VK_IMAGE_TYPE_2D,VK_FORMAT_R32G32B32A32_UINT,3,3,2);
    auto packed = image(VK_IMAGE_TYPE_3D,VK_FORMAT_BC3_UNORM_BLOCK,9,9,2); blocks.extent = {3,3,2};
    check(plan_image_type_copy(unpacked,packed,1,&blocks,plan) == VK_SUCCESS && plan.bytes == 288 &&
          plan.writes[0].imageExtent.width == 9 && plan.writes[0].imageExtent.height == 9,
          "uncompressed-to-compressed copies retain a partial final block at the destination mip edge");
    auto depth_array = image(VK_IMAGE_TYPE_2D,VK_FORMAT_D32_SFLOAT_S8_UINT,4,3,2);
    auto depth_volume = image(VK_IMAGE_TYPE_3D,VK_FORMAT_D32_SFLOAT_S8_UINT,4,3,2);
    VkImageCopy depth{}; depth.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,0,0,2};
    depth.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1}; depth.extent = {4,3,2};
    check(plan_image_type_copy(depth_array,depth_volume,1,&depth,plan) == VK_SUCCESS && plan.bytes == 120 && plan.reads.size() == 2 &&
          plan.reads[0].imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT &&
          plan.reads[1].imageSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT && plan.reads[1].bufferOffset == 96,
          "combined depth/stencil copies use distinct tightly packed aspect transfers");
    auto bytes_array = image(VK_IMAGE_TYPE_2D,VK_FORMAT_R8_UNORM,3,1,1), bytes_volume = image(VK_IMAGE_TYPE_3D,VK_FORMAT_R8_UNORM,3,1,1);
    VkImageCopy small[2]{};
    for (auto& item : small) { item.srcSubresource = item.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; item.extent = {3,1,1}; }
    bytes_volume.width = 6; small[1].dstOffset.x = 3;
    check(plan_image_type_copy(bytes_array,bytes_volume,2,small,plan) == VK_SUCCESS && plan.bytes == 7 &&
          plan.reads[0].bufferOffset == 0 && plan.reads[1].bufferOffset == 4, "each native buffer-image region meets four-byte offset alignment");
    plan.bytes = 123; auto overflow = original; overflow.extent = {UINT32_MAX,UINT32_MAX,UINT32_MAX}; overflow.srcSubresource.layerCount = UINT32_MAX;
    check(plan_image_type_copy(array,volume,1,&overflow,plan) == VK_ERROR_OUT_OF_DEVICE_MEMORY && plan.bytes == 123,
          "size overflow cannot publish a truncated staging plan");

    const auto device = handle<VkDevice>(1);
    const auto physical = handle<VkPhysicalDevice>(2);
    const auto source = handle<VkImage>(10), target = handle<VkImage>(11);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D;
    info.format = array.format; info.extent = {64,64,1}; info.arrayLayers = 8; info.mipLevels = 3;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    MultiviewAttachments::get().remember_image(device,source,info);
    info.imageType = VK_IMAGE_TYPE_3D; info.extent.depth = 16; info.arrayLayers = 1;
    MultiviewAttachments::get().remember_image(device,target,info);
    auto& copies = ImageTypeCopy::get(); const auto cmd = handle<VkCommandBuffer>(20), second = handle<VkCommandBuffer>(21);
    const auto pool = handle<VkCommandPool>(30), other_pool = handle<VkCommandPool>(31);
    auto copy = [&](VkCommandBuffer command) { return copies.copy(device,physical,command,source,VK_IMAGE_LAYOUT_GENERAL,target,VK_IMAGE_LAYOUT_GENERAL,1,&original); };
    check(copies.begin(device,cmd,pool) == VK_SUCCESS && copy(cmd) && copies.error(cmd) == VK_SUCCESS && buffers.size() == 1 && memories.size() == 1 &&
          reads == 1 && writes == 1 && barriers == 1, "recording owns complete private staging resources and emits the ordered native transfers");
    check(copies.error(cmd) == VK_SUCCESS && buffers.size() == 1, "ending/querying a recording retains its resources for resubmission");
    check(copies.begin(device,cmd,pool) == VK_SUCCESS && buffers.empty() && memories.empty(), "rerecording releases the previous staging allocation");
    check(copy(cmd) && copies.begin(device,second,other_pool) == VK_SUCCESS && copy(second) && buffers.size() == 2,
          "independent command pools retain independent staging allocations");
    copies.forget_pool(device,pool);
    check(buffers.size() == 1 && memories.size() == 1, "pool reset retires only its own command resources");
    copies.forget_device(device);
    check(buffers.empty() && memories.empty() && !invalid_destroy, "device teardown retires every remaining buffer before its memory");
    for (unsigned fault = 0; fault < 4; ++fault) {
        check(copies.begin(device,cmd,pool) == VK_SUCCESS, "begin a fresh recording after a previous failure");
        const auto before = reads;
        if (fault == 0) create_status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (fault == 1) allocate_status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (fault == 2) bind_status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (fault == 3) fail_new = true;
        const auto expected = fault == 3 ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_OUT_OF_DEVICE_MEMORY;
        check(copy(cmd) && copies.error(cmd) == expected && reads == before && buffers.empty() && memories.empty() && !invalid_destroy,
              "recording/allocation/bind failure returns its error before any copy and frees only successful handles");
        create_status = allocate_status = bind_status = VK_SUCCESS;
        copies.forget(cmd);
    }
    fail_new = true;
    check(copies.begin(device,cmd,pool) == VK_ERROR_OUT_OF_HOST_MEMORY && !fail_new,
          "command-record setup OOM is returned by begin instead of losing error tracking");
    MultiviewAttachments::get().forget_device(device);
    printf("Image type copy failures: %u\n",failures); return failures ? 1 : 0;
}

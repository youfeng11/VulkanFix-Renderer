// Shared raw Vulkan resource harness for POST and runtime driver probes.
#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
namespace vkfix::nativeprobe {
inline void check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
template<class Context> struct Resources {
    Context &p;
    VkPhysicalDevice physical;
    uint32_t family;
    std::vector<std::function<void()>> objects, allocations;
    struct Buffer { VkBuffer handle; VkDeviceMemory memory; void *mapped; };
    struct Image { VkImage handle; VkImageView view; };
    Resources(Context &probe, VkPhysicalDevice pd, uint32_t q) : p(probe), physical(pd), family(q) {}
    ~Resources() {
        if (p.pending || p.retain_device) return;
        for (auto i = objects.rbegin(); i != objects.rend(); ++i) (*i)();
        for (auto i = allocations.rbegin(); i != allocations.rend(); ++i) (*i)();
    }
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties memory{};
        p.template get<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("no suitable probe memory type");
    }
    VkDeviceMemory allocate(VkMemoryRequirements mr, VkMemoryPropertyFlags flags) {
        auto free = p.template dev<PFN_vkFreeMemory>("vkFreeMemory");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size; ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, flags);
        VkDeviceMemory memory{};
        check(p.template dev<PFN_vkAllocateMemory>("vkAllocateMemory")(p.device, &ai, nullptr, &memory), "allocate probe memory");
        allocations.push_back([=] { free(p.device, memory, nullptr); });
        return memory;
    }
    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        auto destroy = p.template dev<PFN_vkDestroyBuffer>("vkDestroyBuffer");
        auto unmap = p.template dev<PFN_vkUnmapMemory>("vkUnmapMemory");
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage;
        Buffer b{};
        check(p.template dev<PFN_vkCreateBuffer>("vkCreateBuffer")(p.device, &ci, nullptr, &b.handle), "create probe buffer");
        const auto handle = b.handle;
        objects.push_back([=] { destroy(p.device, handle, nullptr); });
        VkMemoryRequirements mr{};
        p.template dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(p.device, b.handle, &mr);
        b.memory = allocate(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        check(p.template dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(p.device, b.handle, b.memory, 0), "bind probe buffer");
        check(p.template dev<PFN_vkMapMemory>("vkMapMemory")(p.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "map probe buffer");
        const auto memory = b.memory;
        allocations.push_back([=] { unmap(p.device, memory); });
        std::memset(b.mapped, 0xa5, size_t(size));
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = memory; range.size = VK_WHOLE_SIZE;
        check(p.template dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device, 1, &range), "flush probe buffer");
        return b;
    }
    void invalidate(const Buffer &b) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = b.memory; range.size = VK_WHOLE_SIZE;
        check(p.template dev<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(p.device, 1, &range), "invalidate probe buffer");
    }
    Image image(VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        auto destroy_image = p.template dev<PFN_vkDestroyImage>("vkDestroyImage");
        auto destroy_view = p.template dev<PFN_vkDestroyImageView>("vkDestroyImageView");
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {16,16,1};
        ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
        Image image{};
        check(p.template dev<PFN_vkCreateImage>("vkCreateImage")(p.device, &ci, nullptr, &image.handle), "create probe image");
        auto handle = image.handle;
        objects.push_back([=] { destroy_image(p.device, handle, nullptr); });
        VkMemoryRequirements mr{};
        p.template dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(p.device, handle, &mr);
        auto memory = allocate(mr, 0);
        check(p.template dev<PFN_vkBindImageMemory>("vkBindImageMemory")(p.device, handle, memory, 0), "bind probe image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = handle; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(p.template dev<PFN_vkCreateImageView>("vkCreateImageView")(p.device, &vi, nullptr, &image.view), "create probe view");
        auto view = image.view;
        objects.push_back([=] { destroy_view(p.device, view, nullptr); });
        return image;
    }
    VkCommandBuffer begin() {
        auto destroy = p.template dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool");
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; ci.queueFamilyIndex = family;
        VkCommandPool pool{};
        check(p.template dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(p.device, &ci, nullptr, &pool), "create probe command pool");
        objects.push_back([=] { destroy(p.device, pool, nullptr); });
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer cb{};
        check(p.template dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(p.device, &ai, &cb), "allocate probe command buffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(p.template dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cb, &bi), "begin probe commands");
        return cb;
    }
    void submit(VkCommandBuffer cb, bool sync2 = false) {
        auto destroy = p.template dev<PFN_vkDestroyFence>("vkDestroyFence");
        check(p.template dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cb), "end probe commands");
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{};
        check(p.template dev<PFN_vkCreateFence>("vkCreateFence")(p.device, &ci, nullptr, &fence), "create probe fence");
        objects.push_back([=] { destroy(p.device, fence, nullptr); });
        VkQueue queue{}; p.template dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(p.device, family, 0, &queue);
        if (sync2) {
            VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer = cb;
            VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; info.commandBufferInfoCount = 1; info.pCommandBufferInfos = &command;
            check(p.template dev<PFN_vkQueueSubmit2KHR>("vkQueueSubmit2KHR")(queue, 1, &info, fence), "submit2 probe");
        } else {
            VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO}; info.commandBufferCount = 1; info.pCommandBuffers = &cb;
            check(p.template dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &info, fence), "submit probe");
        }
        p.pending = true;
        check(p.template dev<PFN_vkWaitForFences>("vkWaitForFences")(p.device, 1, &fence, VK_TRUE, 3000000000ull), "probe fence (3 seconds)");
        p.pending = false;
    }
    void host_barrier(VkCommandBuffer cb, VkPipelineStageFlags source, VkAccessFlags access) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = access; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        p.template dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, source, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
};

} // namespace vkfix::nativeprobe

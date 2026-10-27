#include "core/tbdr_barrier_optimizer.h"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <thread>
static unsigned failures = 0, checks = 0;
static void check(bool good, const char *name) {
    ++checks;
    printf("%s: %s\n", good ? "PASS" : "FAIL", name);
    if (!good)
        ++failures;
}
int main() {
    setenv("VULKAN_FIX_LOG_PATH", "/dev/null", 1);
    setenv("VULKAN_FIX_OPTIMIZE_BARRIER", "1", 1);
    auto &opt = TBDRBarrierOptimizer::get();
    opt.set_enabled(true);
    auto cmd = (VkCommandBuffer)(uintptr_t)0x100;
    TBDRBarrierOptimizer::ScratchStorage2 storage;
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    info.memoryBarrierCount = 1;
    info.pMemoryBarriers = &memory;
    VkDependencyInfo out{};
    auto send = [&] { return opt.optimize_dependency_info(cmd, &info, out, storage); };
    check(send() && out.pMemoryBarriers == &memory &&
              memory.srcStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT &&
              memory.dstStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          "access masks never narrow ALL_COMMANDS execution scopes");
    check(!send(), "exact adjacent duplicate barriers are eliminated");
    opt.on_cmd_action(cmd);
    check(send(), "an intervening GPU action retains a repeated barrier");
    opt.on_cmd_reset(cmd);
    check(send(), "recording reset discards the previous barrier signature");
    info.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    check(send(), "dependency flags participate in exact equality");
    opt.on_cmd_begin(cmd);
    memory.srcAccessMask = memory.dstAccessMask = 0;
    check(send() && out.memoryBarrierCount == 1 &&
              out.pMemoryBarriers[0].srcStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          "zero-access same-stage barriers retain execution dependencies");
    VkBufferMemoryBarrier2 buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    buffer.srcStageMask = buffer.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    buffer.srcAccessMask = buffer.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    buffer.srcQueueFamilyIndex = buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer.buffer = (VkBuffer)0x100001234ULL;
    buffer.size = 256;
    info.memoryBarrierCount = 0;
    info.bufferMemoryBarrierCount = 1;
    info.pBufferMemoryBarriers = &buffer;
    check(send() && out.bufferMemoryBarrierCount == 1 && out.pBufferMemoryBarriers == &buffer,
          "read-only buffer barriers are preserved");
    buffer.buffer = (VkBuffer)0x200001234ULL;
    check(send(), "full 64-bit resource handles participate in equality");
    buffer.offset = 64;
    check(send(), "buffer byte ranges participate in equality");
    buffer.srcQueueFamilyIndex = 0;
    buffer.dstQueueFamilyIndex = 1;
    check(send() && send() && out.pBufferMemoryBarriers[0].dstQueueFamilyIndex == 1,
          "ownership transfers are never stripped or deduplicated");
    VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    image.srcStageMask = image.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    image.srcAccessMask = image.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.image = (VkImage)0x100003456ULL;
    image.oldLayout = image.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    info.bufferMemoryBarrierCount = 0;
    info.imageMemoryBarrierCount = 1;
    info.pImageMemoryBarriers = &image;
    check(send() && out.imageMemoryBarrierCount == 1 && out.pImageMemoryBarriers == &image,
          "read-only same-layout image barriers are preserved");
    image.subresourceRange.baseMipLevel = 1;
    check(send(), "image subresource ranges participate in equality");
    image.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(send() && send(), "image layout transitions always reach the driver");
    image.oldLayout = image.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    check(send() && send(), "presentation barriers always reach the driver");
    info.imageMemoryBarrierCount = 0;
    info.memoryBarrierCount = 1;
    memory.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    check(send() && out.pMemoryBarriers[0].srcStageMask == memory.srcStageMask,
          "synchronization2 high stage bits are preserved");
    VkBaseInStructure extension{VK_STRUCTURE_TYPE_MAX_ENUM, nullptr};
    memory.pNext = &extension;
    check(send() && send() && out.pMemoryBarriers[0].pNext == &extension,
          "barrier extension chains bypass dedup without mutation");
    memory.pNext = nullptr;
    check(send(), "an opaque barrier invalidates duplicate history");
    info.pNext = &extension;
    check(send() && send() && out.pNext == &extension,
          "dependency extension chains remain untouched");
    info.pNext = nullptr;
    opt.set_enabled(false);
    check(send(), "disabled optimizer passes original barriers through");
    opt.on_cmd_action(cmd);
    opt.set_enabled(true);
    check(send(), "reenabling optimization cannot reuse stale history");
    TBDRBarrierOptimizer::ScratchStorage1 legacy;
    VkPipelineStageFlags src = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst = src;
    VkDependencyFlags flags = 0;
    uint32_t mc = 0, bc = 0, ic = 0;
    const VkMemoryBarrier *m = nullptr;
    const VkBufferMemoryBarrier *b = nullptr;
    const VkImageMemoryBarrier *i = nullptr;
    auto old = [&] {
        return opt.optimize_pipeline_barrier1(cmd, src, dst, flags, mc, m, bc, b, ic, i, legacy);
    };
    check(old() && src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == src,
          "legacy barriers with no memory arrays retain their full execution scope");
    check(!old(), "exact adjacent legacy execution barriers can deduplicate");
    VkMemoryBarrier legacy_memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m = &legacy_memory;
    mc = 1;
    check(old() && mc == 1 && m == &legacy_memory,
          "legacy zero-access memory barriers are retained");
    VkBufferMemoryBarrier legacy_buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    legacy_buffer.srcQueueFamilyIndex = legacy_buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    legacy_buffer.srcAccessMask = legacy_buffer.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    legacy_buffer.buffer = (VkBuffer)0x100005678ULL;
    legacy_buffer.size = 128;
    mc = 0;
    bc = 1;
    b = &legacy_buffer;
    check(old() && bc == 1 && b == &legacy_buffer,
          "legacy read-only resource barriers are retained");
    legacy_buffer.buffer = (VkBuffer)0x200005678ULL;
    check(old(), "legacy 64-bit handles do not truncate on 32-bit ABIs");
    legacy_buffer.pNext = &extension;
    check(old() && old(), "legacy extension chains bypass dedup");
    legacy_buffer.pNext = nullptr;
    legacy_buffer.srcQueueFamilyIndex = 2;
    legacy_buffer.dstQueueFamilyIndex = 3;
    check(old() && old(), "legacy ownership transfers always reach the driver");
    std::mutex mutex;
    std::condition_variable cv;
    bool cached = false, retired = false, thread_ok = false;
    std::thread worker([&] {
        opt.on_cmd_action(cmd);
        {
            std::unique_lock<std::mutex> lock(mutex);
            cached = true;
            cv.notify_all();
            cv.wait(lock, [&] { return retired; });
        }
        opt.on_cmd_action(cmd);
        TBDRBarrierOptimizer::ScratchStorage2 s;
        VkDependencyInfo result{};
        thread_ok = opt.optimize_dependency_info(cmd, &info, result, s) &&
                    !opt.optimize_dependency_info(cmd, &info, result, s);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return cached; });
    }
    opt.on_cmd_free(cmd);
    for (uintptr_t n = 0; n < 512; ++n) {
        auto temp = (VkCommandBuffer)(n + 0x1000);
        opt.on_cmd_action(temp);
        opt.on_cmd_free(temp);
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        retired = true;
        cv.notify_all();
    }
    worker.join();
    check(thread_ok, "thread-local trackers survive command retirement and handle reuse");
    auto stats = opt.get_stats();
    check(stats.narrowed_stages == 0 && stats.stripped_image_barriers == 0 &&
              stats.stripped_buffer_barriers == 0,
          "optimizer reports no scope narrowing or resource barrier stripping");
    opt.on_cmd_free(cmd);
    opt.on_cmd_reset(cmd);
    send();
    for (const char* name : {"vkCmdSetViewport", "vkCmdSetScissor", "vkCmdSetLineWidth",
                            "vkCmdSetDepthBias", "vkCmdSetBlendConstants", "vkCmdSetDepthBounds",
                            "vkCmdSetStencilCompareMask", "vkCmdSetStencilWriteMask",
                            "vkCmdSetStencilReference", "vkCmdSetViewportWithCount",
                            "vkCmdSetViewportWithCountEXT", "vkCmdSetScissorWithCount",
                            "vkCmdSetScissorWithCountEXT"})
        opt.on_untracked_command(name);
    check(opt.is_enabled() && !send(),
          "native fixed-function state lookups preserve exact adjacent barrier dedup");
    opt.on_cmd_action(cmd);
    check(send(), "draws after native state changes still require the following barrier");
    opt.on_cmd_free(cmd);
    opt.on_untracked_command("vkCmdSetEvent");
    opt.set_enabled(true);
    check(!opt.is_enabled() && send() && send(),
          "native commands outside action tracking permanently bypass barrier dedup");
    printf("Barrier optimizer checks: %u failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}

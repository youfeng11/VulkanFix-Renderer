#pragma once
#include "native_probe_resources.h"

namespace vkfix::nativeprobe {
struct EventProbeReadback {
    std::string error;
    bool reset_left_event_set = false;
};

// Both paths use the same event and transfer workload. Check event state even
// on devices where coherent memory can hide a missing dependency in readback.
template<class Context, class Ready>
EventProbeReadback probe_sync2_events(Context &p, VkPhysicalDevice physical, uint32_t family,
                                     Sync2Route route, bool legacy_reset, Ready ready) {
    Resources<Context> gpu(p, physical, family);
    constexpr uint32_t words = 64;
    constexpr VkDeviceSize bytes = words * sizeof(uint32_t);
    const auto source = gpu.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto target = gpu.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkEventCreateInfo ci{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent event{};
    auto destroy = p.template dev<PFN_vkDestroyEvent>("vkDestroyEvent");
    check(p.template dev<PFN_vkCreateEvent>("vkCreateEvent")(p.device, &ci, nullptr, &event), "create probe event");
    gpu.objects.push_back([=, &p] { destroy(p.device, event, nullptr); });
    auto status = p.template dev<PFN_vkGetEventStatus>("vkGetEventStatus");
    const bool modern = route != Sync2Route::Legacy;
    const bool core = route == Sync2Route::Core;
    auto set2 = modern ? p.template dev<PFN_vkCmdSetEvent2>(core ? "vkCmdSetEvent2" : "vkCmdSetEvent2KHR") : nullptr;
    auto wait2 = modern ? p.template dev<PFN_vkCmdWaitEvents2>(core ? "vkCmdWaitEvents2" : "vkCmdWaitEvents2KHR") : nullptr;
    auto reset2 = modern && !legacy_reset ?
        p.template dev<PFN_vkCmdResetEvent2>(core ? "vkCmdResetEvent2" : "vkCmdResetEvent2KHR") : nullptr;
    auto set1 = !modern ? p.template dev<PFN_vkCmdSetEvent>("vkCmdSetEvent") : nullptr;
    auto wait1 = !modern ? p.template dev<PFN_vkCmdWaitEvents>("vkCmdWaitEvents") : nullptr;
    auto reset1 = reset2 ? nullptr : p.template dev<PFN_vkCmdResetEvent>("vkCmdResetEvent");
    auto fill = p.template dev<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    auto copy = p.template dev<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");

    VkBufferMemoryBarrier2 buffer2{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    buffer2.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    buffer2.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    buffer2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    buffer2.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    buffer2.srcQueueFamilyIndex = buffer2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer2.buffer = source.handle;
    buffer2.size = bytes;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &buffer2;
    VkBufferMemoryBarrier buffer1{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    buffer1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    buffer1.srcQueueFamilyIndex = buffer1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer1.buffer = source.handle;
    buffer1.size = bytes;

    ready();
    if (status(p.device, event) != VK_EVENT_RESET) return {"new event was not reset"};
    for (uint32_t value : {0x3526ace1u, 0x94bec8d7u}) {
        auto cb = gpu.begin();
        fill(cb, source.handle, 0, bytes, value);
        if (modern) {
            set2(cb, event, &dependency);
            wait2(cb, 1, &event, &dependency);
        } else {
            set1(cb, event, VK_PIPELINE_STAGE_TRANSFER_BIT);
            wait1(cb, 1, &event, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  0, nullptr, 1, &buffer1, 0, nullptr);
        }
        const VkBufferCopy region{0, 0, bytes};
        copy(cb, source.handle, target.handle, 1, &region);
        gpu.host_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        gpu.submit(cb);
        if (status(p.device, event) != VK_EVENT_SET) return {"device event signal did not become visible to host"};
        gpu.invalidate(target);
        const auto *data = static_cast<const uint32_t *>(target.mapped);
        if (!std::all_of(data, data + words, [=](uint32_t word) { return word == value; }))
            return {"event-dependent transfer readback mismatch"};

        cb = gpu.begin();
        // The prior fence orders this reset after the completed event wait.
        if (reset2) reset2(cb, event, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        else reset1(cb, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        gpu.submit(cb);
        const auto reset_status = status(p.device, event);
        if (reset_status != VK_EVENT_RESET)
            return {"device event reset did not become visible to host", reset_status == VK_EVENT_SET};
    }
    return {};
}
} // namespace vkfix::nativeprobe

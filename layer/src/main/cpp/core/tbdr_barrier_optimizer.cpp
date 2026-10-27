#include "tbdr_barrier_optimizer.h"
#include "vk_common.h"
#include <initializer_list>
#include <new>
namespace {
using Signature = std::vector<uint64_t>;
void append(Signature &s, std::initializer_list<uint64_t> v) {
    s.insert(s.end(), v.begin(), v.end());
}
template <class B> void scope2(Signature &s, const B &b) {
    append(s, {b.srcStageMask, b.srcAccessMask, b.dstStageMask, b.dstAccessMask});
}
template <class B> void buffer_range(Signature &s, const B &b) {
    append(s, {b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, (uint64_t)b.buffer, b.offset, b.size});
}
template <class B> void image_range(Signature &s, const B &b) {
    const auto &r = b.subresourceRange;
    append(s, {b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, (uint64_t)b.image,
               uint64_t(b.oldLayout), uint64_t(b.newLayout), r.aspectMask, r.baseMipLevel,
               r.levelCount, r.baseArrayLayer, r.layerCount});
}
template <class B> bool ordinary_image(const B &b) {
    return !b.pNext && b.srcQueueFamilyIndex == b.dstQueueFamilyIndex &&
           b.oldLayout == b.newLayout && b.oldLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
           b.oldLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
           b.oldLayout != VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR;
}
bool option(const char *v, bool fallback) {
    return v && *v ? !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on") : fallback;
}
bool state_only_command(const char* name) {
    // Exact names only: vkCmdSetEvent, for example, is synchronization rather
    // than state. These commands affect later draws, whose action hooks close
    // the dedup window; they cannot access resources between two barriers.
    static constexpr const char* commands[] = {
        "vkCmdSetViewport", "vkCmdSetScissor", "vkCmdSetLineWidth", "vkCmdSetDepthBias",
        "vkCmdSetBlendConstants", "vkCmdSetDepthBounds", "vkCmdSetStencilCompareMask",
        "vkCmdSetStencilWriteMask", "vkCmdSetStencilReference", "vkCmdSetViewportWithCount",
        "vkCmdSetViewportWithCountEXT", "vkCmdSetScissorWithCount", "vkCmdSetScissorWithCountEXT"
    };
    if (name)
        for (const auto* command : commands)
            if (!strcmp(name, command)) return true;
    return false;
}
} // namespace
TBDRBarrierOptimizer &TBDRBarrierOptimizer::get() {
    static TBDRBarrierOptimizer value;
    return value;
}
TBDRBarrierOptimizer::TBDRBarrierOptimizer() {
    const char *v = getenv("VULKAN_FIX_OPTIMIZE_BARRIER");
    if (!v || !*v)
        v = getenv("OPTIMIZE_BARRIER");
    if (!v || !*v)
        v = getenv("optimize_barrier");
    m_enabled.store(option(v, true), std::memory_order_relaxed);
    m_dedup_consecutive.store(option(getenv("VULKAN_FIX_BARRIER_DEDUP"), true),
                              std::memory_order_relaxed);
    LOGI("TBDRBarrierOptimizer: %s (exact adjacent dedup; synchronization scopes preserved)",
         is_enabled() ? "ENABLED" : "disabled");
}
void TBDRBarrierOptimizer::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    for (auto &e : m_trackers) {
        e.second->hasLastBarrier = false;
        e.second->signature.clear();
    }
    m_enabled.store(enabled, std::memory_order_relaxed);
}
void TBDRBarrierOptimizer::on_untracked_command(const char *name) {
    if (state_only_command(name)) return;
    if (m_tracking_complete.exchange(false, std::memory_order_relaxed))
        LOGI("TBDRBarrierOptimizer: bypassed after native command lookup %s; action tracking is "
             "incomplete",
             name ? name : "<unknown>");
}
void TBDRBarrierOptimizer::maybe_log_stats() {
    auto total = m_stat_total.load(std::memory_order_relaxed);
    static std::atomic<uint64_t> last{0};
    auto previous = last.load(std::memory_order_relaxed);
    if (total < previous + 1024 ||
        !last.compare_exchange_strong(previous, total, std::memory_order_relaxed))
        return;
    LOGI("TBDR stats: total=%llu exactDuplicates=%llu (stages/access/ownership preserved)",
         (unsigned long long)total,
         (unsigned long long)m_stat_eliminated.load(std::memory_order_relaxed));
}
TBDRBarrierOptimizer::CmdTracker &TBDRBarrierOptimizer::get_or_create_tracker(VkCommandBuffer cmd) {
    auto key = (uint64_t)(uintptr_t)cmd;
    auto it = m_trackers.find(key);
    if (it != m_trackers.end())
        return *it->second;
    auto tracker = std::make_shared<CmdTracker>();
    m_trackers.emplace(key, tracker);
    return *tracker;
}
void TBDRBarrierOptimizer::notify_action_slow(VkCommandBuffer cmd, VkCommandBuffer &cached_cmd,
                                              std::shared_ptr<CmdTracker> &cached_tracker,
                                              uint32_t &cached_gen) {
    try {
        std::lock_guard<std::mutex> lock(m_tracker_mutex);
        auto &tracker = get_or_create_tracker(cmd);
        tracker.hadAction.store(true, std::memory_order_relaxed);
        cached_cmd = cmd;
        cached_tracker = m_trackers.at((uint64_t)(uintptr_t)cmd);
        cached_gen = tracker.generation.load(std::memory_order_relaxed);
    } catch (const std::bad_alloc &) {
        m_enabled.store(false, std::memory_order_relaxed);
        cached_tracker.reset();
    }
}
void TBDRBarrierOptimizer::on_cmd_begin(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    auto it = m_trackers.find((uint64_t)(uintptr_t)cmd);
    if (it != m_trackers.end()) {
        it->second->hasLastBarrier = false;
        it->second->signature.clear();
        it->second->hadAction.store(false, std::memory_order_relaxed);
        it->second->generation.fetch_add(1, std::memory_order_relaxed);
    }
}
void TBDRBarrierOptimizer::on_cmd_reset(VkCommandBuffer cmd) {
    on_cmd_begin(cmd);
}
void TBDRBarrierOptimizer::on_cmd_free(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    auto it = m_trackers.find((uint64_t)(uintptr_t)cmd);
    if (it != m_trackers.end()) {
        it->second->valid.store(false, std::memory_order_relaxed);
        it->second->hasLastBarrier = false;
        // TLS holds ownership until it observes invalidation; raw pointers into
        // a bounded retirement list can become dangling after command churn.
        m_trackers.erase(it);
    }
}
void TBDRBarrierOptimizer::invalidate(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    auto it = m_trackers.find((uint64_t)(uintptr_t)cmd);
    if (it != m_trackers.end())
        it->second->hasLastBarrier = false;
}
bool TBDRBarrierOptimizer::retain_barrier(VkCommandBuffer cmd, Signature &signature) {
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    auto &t = get_or_create_tracker(cmd);
    bool duplicate = t.hasLastBarrier && !t.hadAction.load(std::memory_order_relaxed) &&
                     t.signature == signature;
    t.signature.swap(signature);
    t.hasLastBarrier = true;
    t.hadAction.store(false, std::memory_order_relaxed);
    if (duplicate)
        m_stat_eliminated.fetch_add(1, std::memory_order_relaxed);
    return !duplicate;
}
bool TBDRBarrierOptimizer::optimize_dependency_info(VkCommandBuffer cmd,
                                                    const VkDependencyInfo *src,
                                                    VkDependencyInfo &out,
                                                    ScratchStorage2 &storage) {
    if (!src)
        return false;
    out = *src; // Preserve the original arrays, execution scopes and extension chains.
    if (!is_enabled())
        return true;
    m_stat_total.fetch_add(1, std::memory_order_relaxed);
    auto pass = [&] {
        invalidate(cmd);
        return true;
    };
    if (!m_dedup_consecutive.load(std::memory_order_relaxed) || src->pNext ||
        (src->memoryBarrierCount && !src->pMemoryBarriers) ||
        (src->bufferMemoryBarrierCount && !src->pBufferMemoryBarriers) ||
        (src->imageMemoryBarrierCount && !src->pImageMemoryBarriers))
        return pass();
    try {
        auto &s = storage.signature;
        s.clear();
        append(s, {2, src->dependencyFlags, src->memoryBarrierCount, src->bufferMemoryBarrierCount,
                   src->imageMemoryBarrierCount});
        for (uint32_t i = 0; i < src->memoryBarrierCount; ++i) {
            auto &b = src->pMemoryBarriers[i];
            if (b.pNext)
                return pass();
            scope2(s, b);
        }
        for (uint32_t i = 0; i < src->bufferMemoryBarrierCount; ++i) {
            auto &b = src->pBufferMemoryBarriers[i];
            if (b.pNext || b.srcQueueFamilyIndex != b.dstQueueFamilyIndex)
                return pass();
            scope2(s, b);
            buffer_range(s, b);
        }
        for (uint32_t i = 0; i < src->imageMemoryBarrierCount; ++i) {
            auto &b = src->pImageMemoryBarriers[i];
            if (!ordinary_image(b))
                return pass();
            scope2(s, b);
            image_range(s, b);
        }
        return retain_barrier(cmd, s);
    } catch (const std::bad_alloc &) {
        return pass();
    }
}
bool TBDRBarrierOptimizer::optimize_pipeline_barrier1(
    VkCommandBuffer cmd, VkPipelineStageFlags &src, VkPipelineStageFlags &dst,
    VkDependencyFlags &flags, uint32_t &mc, const VkMemoryBarrier *&memory, uint32_t &bc,
    const VkBufferMemoryBarrier *&buffers, uint32_t &ic, const VkImageMemoryBarrier *&images,
    ScratchStorage1 &storage) {
    if (!is_enabled())
        return true;
    m_stat_total.fetch_add(1, std::memory_order_relaxed);
    auto pass = [&] {
        invalidate(cmd);
        return true;
    };
    if (!m_dedup_consecutive.load(std::memory_order_relaxed) || (mc && !memory) ||
        (bc && !buffers) || (ic && !images))
        return pass();
    try {
        auto &s = storage.signature;
        s.clear();
        // Legacy barriers with no memory arrays still define execution dependencies.
        append(s, {1, src, dst, flags, mc, bc, ic});
        for (uint32_t i = 0; i < mc; ++i) {
            if (memory[i].pNext)
                return pass();
            append(s, {memory[i].srcAccessMask, memory[i].dstAccessMask});
        }
        for (uint32_t i = 0; i < bc; ++i) {
            auto &b = buffers[i];
            if (b.pNext || b.srcQueueFamilyIndex != b.dstQueueFamilyIndex)
                return pass();
            append(s, {b.srcAccessMask, b.dstAccessMask});
            buffer_range(s, b);
        }
        for (uint32_t i = 0; i < ic; ++i) {
            auto &b = images[i];
            if (!ordinary_image(b))
                return pass();
            append(s, {b.srcAccessMask, b.dstAccessMask});
            image_range(s, b);
        }
        return retain_barrier(cmd, s);
    } catch (const std::bad_alloc &) {
        return pass();
    }
}
TBDRBarrierOptimizer::Stats TBDRBarrierOptimizer::get_stats() const {
    return {m_stat_total.load(std::memory_order_relaxed),
            m_stat_eliminated.load(std::memory_order_relaxed), 0, 0, 0};
}

#ifndef TBDR_BARRIER_OPTIMIZER_H
#define TBDR_BARRIER_OPTIMIZER_H

#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>

/**
 * TBDR Mobile Architecture Pipeline Barrier Optimizer & Eliminator
 *
 * Problem on Mobile TBDR GPUs (Qualcomm Adreno, ARM Mali, PowerVR):
 * - Desktop-style engines (like Minecraft VulkanMod) frequently emit redundant, identical,
 *   or overly-broad pipeline barriers (e.g. ALL_COMMANDS_BIT -> ALL_COMMANDS_BIT with MEMORY_READ/WRITE).
 * - On mobile TBDR architectures, broad barriers force the GPU to evict on-chip Tile SRAM
 *   to external LPDDR RAM (Tile Resolve / Tile Flushes), causing extreme memory bandwidth spikes,
 *   battery drain, rapid thermal throttling, and severe framerate stutter.
 *
 * Solution:
 * 1. Zero-Cost No-Op Elimination: Detect and drop barriers with no layout transitions, no hazard,
 *    and identical stage/queue configurations.
 * 2. Consecutive Duplicate Debouncing: Track last barrier fingerprint on each command buffer.
 *    If back-to-back identical barriers are recorded without any interleaved draws/dispatches, drop them.
 * 3. TBDR Stage Narrowing: Narrow ALL_COMMANDS_BIT to exact producer/consumer stages
 *    (e.g., COLOR_ATTACHMENT_OUTPUT -> FRAGMENT_SHADER), avoiding full GPU tile drains.
 * 4. Subpass BY_REGION Promotion: Promote in-pass dependencies to BY_REGION for on-chip tile locality.
 */
class TBDRBarrierOptimizer {
public:
    static TBDRBarrierOptimizer& get();

    struct ScratchStorage2 {
        std::vector<VkMemoryBarrier2> memoryBarriers;
        std::vector<VkBufferMemoryBarrier2> bufferBarriers;
        std::vector<VkImageMemoryBarrier2> imageBarriers;
    };

    struct ScratchStorage1 {
        std::vector<VkMemoryBarrier> memoryBarriers;
        std::vector<VkBufferMemoryBarrier> bufferBarriers;
        std::vector<VkImageMemoryBarrier> imageBarriers;
    };

    // Optimize VkDependencyInfo for vkCmdPipelineBarrier2.
    // Returns false if the barrier is completely redundant or empty and should be DROPPED entirely.
    // Returns true if the barrier should proceed, populating outOptimized and storage.
    bool optimize_dependency_info(
        VkCommandBuffer cmd,
        const VkDependencyInfo* pSrc,
        VkDependencyInfo& outOptimized,
        ScratchStorage2& storage
    );

    // Optimize parameters for Vulkan 1.0 vkCmdPipelineBarrier.
    // Returns false if the barrier should be DROPPED entirely.
    bool optimize_pipeline_barrier1(
        VkCommandBuffer cmd,
        VkPipelineStageFlags& inOutSrcStageMask,
        VkPipelineStageFlags& inOutDstStageMask,
        VkDependencyFlags& inOutDependencyFlags,
        uint32_t& inOutMemCount,
        const VkMemoryBarrier*& pInOutMemBarriers,
        uint32_t& inOutBufCount,
        const VkBufferMemoryBarrier*& pInOutBufBarriers,
        uint32_t& inOutImgCount,
        const VkImageMemoryBarrier*& pInOutImgBarriers,
        ScratchStorage1& storage
    );

    // High-frequency action notification (called on draw, dispatch, copy, clear, blit)
    // Marks that rendering/state commands occurred on this command buffer since last barrier.
    inline void on_cmd_action(VkCommandBuffer cmd) {
        if (!m_enabled.load(std::memory_order_relaxed)) return;
        if (__builtin_expect(cmd == m_primary_cmd.load(std::memory_order_relaxed), 1)) {
            if (m_primary_had_action) {
                m_primary_had_action->store(true, std::memory_order_relaxed);
            }
            return;
        }
        notify_action_slow(cmd);
    }

    // Command buffer lifecycle hooks
    void on_cmd_begin(VkCommandBuffer cmd);
    void on_cmd_reset(VkCommandBuffer cmd);
    void on_cmd_free(VkCommandBuffer cmd);

    // Configuration
    inline bool is_enabled() const { return m_enabled.load(std::memory_order_relaxed); }
    void set_enabled(bool enabled) { m_enabled.store(enabled, std::memory_order_relaxed); }

    // Statistics
    struct Stats {
        uint64_t total_barriers;
        uint64_t eliminated_barriers;
        uint64_t narrowed_stages;
        uint64_t stripped_image_barriers;
        uint64_t stripped_buffer_barriers;
    };
    Stats get_stats() const;

private:
    TBDRBarrierOptimizer();
    ~TBDRBarrierOptimizer() = default;

    struct BarrierFingerprint {
        uint64_t hash = 0;
        VkPipelineStageFlags2 srcStageMask = 0;
        VkPipelineStageFlags2 dstStageMask = 0;
        VkDependencyFlags dependencyFlags = 0;
        uint32_t memCount = 0;
        uint32_t bufCount = 0;
        uint32_t imgCount = 0;
    };

    struct CmdTracker {
        BarrierFingerprint lastBarrier{};
        std::atomic<bool> hadAction{false};
        bool hasLastBarrier = false;
    };

    void notify_action_slow(VkCommandBuffer cmd);
    CmdTracker& get_or_create_tracker(VkCommandBuffer cmd);

    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_narrow_stages{true};
    std::atomic<bool> m_dedup_consecutive{true};
    std::atomic<bool> m_strip_noops{true};

    // Primary command buffer fast-path cache (eliminates map lookups on hot draw calls)
    std::atomic<VkCommandBuffer> m_primary_cmd{VK_NULL_HANDLE};
    std::atomic<bool>* m_primary_had_action = nullptr;

    mutable std::mutex m_tracker_mutex;
    std::unordered_map<uint64_t, std::unique_ptr<CmdTracker>> m_trackers;

    std::atomic<uint64_t> m_stat_total{0};
    std::atomic<uint64_t> m_stat_eliminated{0};
    std::atomic<uint64_t> m_stat_narrowed{0};
    std::atomic<uint64_t> m_stat_stripped_img{0};
    std::atomic<uint64_t> m_stat_stripped_buf{0};
};

#endif // TBDR_BARRIER_OPTIMIZER_H

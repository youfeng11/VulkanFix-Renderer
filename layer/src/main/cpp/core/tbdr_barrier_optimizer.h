#ifndef TBDR_BARRIER_OPTIMIZER_H
#define TBDR_BARRIER_OPTIMIZER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

/**
 * Removes exact adjacent duplicate barriers when no action intervenes.
 * Execution scopes, access masks, ownership transfers, layout transitions and
 * extension chains are preserved. Access masks alone cannot prove that a
 * barrier has no execution effect, or justify narrowing its stage masks.
 */
class TBDRBarrierOptimizer {
  public:
    static TBDRBarrierOptimizer &get();

    struct ScratchStorage2 {
        std::vector<uint64_t> signature;
        std::vector<VkMemoryBarrier2> memoryBarriers;
        std::vector<VkBufferMemoryBarrier2> bufferBarriers;
        std::vector<VkImageMemoryBarrier2> imageBarriers;
    };

    struct ScratchStorage1 {
        std::vector<uint64_t> signature;
        std::vector<VkMemoryBarrier> memoryBarriers;
        std::vector<VkBufferMemoryBarrier> bufferBarriers;
        std::vector<VkImageMemoryBarrier> imageBarriers;
    };

    // Optimize VkDependencyInfo for vkCmdPipelineBarrier2.
    // Returns false only for an exact adjacent duplicate with no intervening action.
    // Returns true if the barrier should proceed, populating outOptimized and storage.
    bool optimize_dependency_info(VkCommandBuffer cmd, const VkDependencyInfo *pSrc,
                                  VkDependencyInfo &outOptimized, ScratchStorage2 &storage);

    // Optimize parameters for Vulkan 1.0 vkCmdPipelineBarrier.
    // Returns false if the barrier should be DROPPED entirely.
    bool optimize_pipeline_barrier1(
        VkCommandBuffer cmd, VkPipelineStageFlags &inOutSrcStageMask,
        VkPipelineStageFlags &inOutDstStageMask, VkDependencyFlags &inOutDependencyFlags,
        uint32_t &inOutMemCount, const VkMemoryBarrier *&pInOutMemBarriers, uint32_t &inOutBufCount,
        const VkBufferMemoryBarrier *&pInOutBufBarriers, uint32_t &inOutImgCount,
        const VkImageMemoryBarrier *&pInOutImgBarriers, ScratchStorage1 &storage);

    struct CmdTracker {
        std::vector<uint64_t> signature;
        std::atomic<bool> hadAction{false};
        std::atomic<bool> valid{true};
        std::atomic<uint32_t> generation{1};
        bool hasLastBarrier = false;
    };

    // High-frequency action notification (called on draw, dispatch, copy, clear, blit)
    // Marks that rendering/state commands occurred on this command buffer since last barrier.
    inline void on_cmd_action(VkCommandBuffer cmd) {
        if (!is_enabled() || cmd == VK_NULL_HANDLE)
            return;
        thread_local VkCommandBuffer t_cached_cmd = VK_NULL_HANDLE;
        thread_local std::shared_ptr<CmdTracker> t_cached_tracker;
        thread_local uint32_t t_cached_gen = 0;

        if (__builtin_expect(cmd == t_cached_cmd && t_cached_tracker &&
                                 t_cached_tracker->valid.load(std::memory_order_relaxed) &&
                                 t_cached_tracker->generation.load(std::memory_order_relaxed) ==
                                     t_cached_gen,
                             1)) {
            t_cached_tracker->hadAction.store(true, std::memory_order_relaxed);
            return;
        }

        notify_action_slow(cmd, t_cached_cmd, t_cached_tracker, t_cached_gen);
    }

    // Command buffer lifecycle hooks
    void on_cmd_begin(VkCommandBuffer cmd);
    void on_cmd_reset(VkCommandBuffer cmd);
    void on_cmd_free(VkCommandBuffer cmd);

    // Configuration
    inline bool is_enabled() const {
        return m_enabled.load(std::memory_order_relaxed) &&
               m_tracking_complete.load(std::memory_order_relaxed);
    }
    void set_enabled(bool enabled);
    void on_untracked_command(const char *name);

    // Statistics
    struct Stats {
        uint64_t total_barriers;
        uint64_t eliminated_barriers;
        uint64_t narrowed_stages;
        uint64_t stripped_image_barriers;
        uint64_t stripped_buffer_barriers;
    };
    Stats get_stats() const;

    // Logs cumulative optimization counts every 1024 processed barriers.
    void maybe_log_stats();

  private:
    TBDRBarrierOptimizer();
    ~TBDRBarrierOptimizer() = default;

    void notify_action_slow(VkCommandBuffer cmd, VkCommandBuffer &out_cached_cmd,
                            std::shared_ptr<CmdTracker> &out_cached_tracker,
                            uint32_t &out_cached_gen);
    CmdTracker &get_or_create_tracker(VkCommandBuffer cmd);
    void invalidate(VkCommandBuffer cmd);
    bool retain_barrier(VkCommandBuffer cmd, std::vector<uint64_t> &signature);

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_tracking_complete{true};
    std::atomic<bool> m_dedup_consecutive{false};

    mutable std::mutex m_tracker_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<CmdTracker>> m_trackers;

    std::atomic<uint64_t> m_stat_total{0};
    std::atomic<uint64_t> m_stat_eliminated{0};
};

#endif // TBDR_BARRIER_OPTIMIZER_H

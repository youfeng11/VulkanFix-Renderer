#include "tbdr_barrier_optimizer.h"
#include "driver_loader.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <algorithm>

static inline void hash_combine_64(uint64_t& seed, uint64_t v) {
    seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

static inline bool is_read_only_access2(VkAccessFlags2 access) {
    const VkAccessFlags2 write_mask =
        VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT |
        VK_ACCESS_2_HOST_WRITE_BIT |
        VK_ACCESS_2_MEMORY_WRITE_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    return (access & write_mask) == 0;
}

static inline bool is_read_only_access1(VkAccessFlags access) {
    const VkAccessFlags write_mask =
        VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_MEMORY_WRITE_BIT;
    return (access & write_mask) == 0;
}

// ALL_COMMANDS narrowing target. HOST, conditional rendering and transform
// feedback must stay in the narrowed set: ALL_COMMANDS includes them, and
// dropping them would lose the host-write -> GPU-read and transform-feedback
// dependencies (which caused stale/black chunk data on some drivers).
static constexpr VkPipelineStageFlags2 kBroadStages2 =
    VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
    VK_PIPELINE_STAGE_2_HOST_BIT |
    VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT |
    VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT;

static constexpr VkPipelineStageFlags kBroadStages1 =
    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
    VK_PIPELINE_STAGE_TRANSFER_BIT |
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
    VK_PIPELINE_STAGE_HOST_BIT |
    VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT |
    VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;

// Exact stage narrowing: derive the producer/consumer stages from the access
// masks instead of replacing ALL_COMMANDS with another broad mask. On TBDR this
// lets the driver keep tiles on-chip (e.g. COLOR_ATTACHMENT_OUTPUT <->
// FRAGMENT_SHADER) instead of treating the barrier as a full pipeline drain.
static constexpr VkPipelineStageFlags2 kShaderStages2 =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
    VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
    VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

static constexpr VkPipelineStageFlags kShaderStages1 =
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
    VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
    VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

static bool narrow_stages2(VkPipelineStageFlags2& mask, VkAccessFlags2 access) {
    if (!(mask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) return false;
    VkPipelineStageFlags2 derived = 0;
    if (access == 0 ||
        (access & (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT))) {
        derived = kBroadStages2;
    } else {
        if (access & (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT))
            derived |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (access & (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))
            derived |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        if (access & VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT)
            derived |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        if (access & (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                      VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT))
            derived |= kShaderStages2;
        if (access & (VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT))
            derived |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        if (access & (VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT))
            derived |= VK_PIPELINE_STAGE_2_HOST_BIT;
        if (access & VK_ACCESS_2_INDEX_READ_BIT)
            derived |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        if (access & VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT)
            derived |= VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
        if (access & VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT)
            derived |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        if (access & VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT)
            derived |= VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT;
        if (access & VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT)
            derived |= VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
        if (derived == 0) derived = kBroadStages2;
    }
    mask = (mask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) | derived;
    return true;
}

static bool narrow_stages1(VkPipelineStageFlags& mask, VkAccessFlags access) {
    if (!(mask & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) return false;
    VkPipelineStageFlags derived = 0;
    if (access == 0 ||
        (access & (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT))) {
        derived = kBroadStages1;
    } else {
        if (access & (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT))
            derived |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (access & (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))
            derived |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        if (access & VK_ACCESS_INPUT_ATTACHMENT_READ_BIT)
            derived |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        if (access & (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT))
            derived |= kShaderStages1;
        if (access & (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT))
            derived |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (access & (VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT))
            derived |= VK_PIPELINE_STAGE_HOST_BIT;
        if (access & VK_ACCESS_INDEX_READ_BIT)
            derived |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        if (access & VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT)
            derived |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        if (access & VK_ACCESS_INDIRECT_COMMAND_READ_BIT)
            derived |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        if (access & VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT)
            derived |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
        if (access & VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT)
            derived |= VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT;
        if (derived == 0) derived = kBroadStages1;
    }
    mask = (mask & ~VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) | derived;
    return true;
}

TBDRBarrierOptimizer& TBDRBarrierOptimizer::get() {
    static TBDRBarrierOptimizer s_instance;
    return s_instance;
}

TBDRBarrierOptimizer::TBDRBarrierOptimizer() {
    const char* env_opt = getenv("VULKAN_FIX_OPTIMIZE_BARRIER");
    if (!env_opt || env_opt[0] == '\0') env_opt = getenv("OPTIMIZE_BARRIER");
    if (!env_opt || env_opt[0] == '\0') env_opt = getenv("optimize_barrier");
    // Enabled by default. Set VULKAN_FIX_OPTIMIZE_BARRIER=0 to opt out.
    bool opt_enabled = true;
    if (env_opt && env_opt[0] != '\0') {
        opt_enabled = (strcmp(env_opt, "1") == 0 ||
                       strcasecmp(env_opt, "true") == 0 ||
                       strcasecmp(env_opt, "on") == 0);
    }
    m_enabled.store(opt_enabled, std::memory_order_relaxed);
    if (opt_enabled) {
        LOGI("TBDRBarrierOptimizer: ENABLED by default (stage narrowing, deduplication, read-only strip); set VULKAN_FIX_OPTIMIZE_BARRIER=0 to disable");
    } else {
        LOGI("TBDRBarrierOptimizer: disabled by VULKAN_FIX_OPTIMIZE_BARRIER");
    }

    const char* env_narrow = getenv("VULKAN_FIX_BARRIER_NARROW");
    if (env_narrow) {
        m_narrow_stages.store(strcmp(env_narrow, "1") == 0 || strcasecmp(env_narrow, "true") == 0, std::memory_order_relaxed);
    } else {
        m_narrow_stages.store(opt_enabled, std::memory_order_relaxed);
    }

    const char* env_dedup = getenv("VULKAN_FIX_BARRIER_DEDUP");
    if (env_dedup) {
        m_dedup_consecutive.store(strcmp(env_dedup, "1") == 0 || strcasecmp(env_dedup, "true") == 0, std::memory_order_relaxed);
    } else {
        m_dedup_consecutive.store(opt_enabled, std::memory_order_relaxed);
    }

    const char* env_strip = getenv("VULKAN_FIX_BARRIER_STRIP");
    if (env_strip) {
        m_strip_noops.store(strcmp(env_strip, "1") == 0 || strcasecmp(env_strip, "true") == 0, std::memory_order_relaxed);
    } else {
        m_strip_noops.store(opt_enabled, std::memory_order_relaxed);
    }
}

void TBDRBarrierOptimizer::maybe_log_stats() {
    const uint64_t total = m_stat_total.load(std::memory_order_relaxed);
    static std::atomic<uint64_t> s_last_reported{0};
    uint64_t last = s_last_reported.load(std::memory_order_relaxed);
    if (total < last + 1024) return;
    if (!s_last_reported.compare_exchange_strong(last, total, std::memory_order_relaxed)) return;
    LOGI("TBDR stats: total=%llu eliminated=%llu narrowed=%llu strippedImage=%llu strippedBuffer=%llu",
         (unsigned long long)total,
         (unsigned long long)m_stat_eliminated.load(std::memory_order_relaxed),
         (unsigned long long)m_stat_narrowed.load(std::memory_order_relaxed),
         (unsigned long long)m_stat_stripped_img.load(std::memory_order_relaxed),
         (unsigned long long)m_stat_stripped_buf.load(std::memory_order_relaxed));
}
TBDRBarrierOptimizer::CmdTracker& TBDRBarrierOptimizer::get_or_create_tracker(VkCommandBuffer cmd) {
    auto it = m_trackers.find((uint64_t)(uintptr_t)cmd);
    if (it != m_trackers.end()) {
        return *it->second;
    }
    static std::atomic<uint32_t> s_global_gen{1};
    auto tracker = std::make_shared<CmdTracker>();
    tracker->generation.store(s_global_gen.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    tracker->valid.store(true, std::memory_order_relaxed);
    CmdTracker& ref = *tracker;
    m_trackers[(uint64_t)(uintptr_t)cmd] = std::move(tracker);
    return ref;
}

void TBDRBarrierOptimizer::notify_action_slow(
    VkCommandBuffer cmd,
    VkCommandBuffer& out_cached_cmd,
    CmdTracker*& out_cached_tracker,
    uint32_t& out_cached_gen
) {
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    CmdTracker& tracker = get_or_create_tracker(cmd);
    tracker.hadAction.store(true, std::memory_order_relaxed);
    out_cached_cmd = cmd;
    out_cached_tracker = &tracker;
    out_cached_gen = tracker.generation.load(std::memory_order_relaxed);
}

void TBDRBarrierOptimizer::on_cmd_begin(VkCommandBuffer cmd) {
    if (!m_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    auto it = m_trackers.find((uint64_t)(uintptr_t)cmd);
    if (it != m_trackers.end()) {
        it->second->hasLastBarrier = false;
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
        m_graveyard.push_back(it->second);
        if (m_graveyard.size() > 64) {
            m_graveyard.erase(m_graveyard.begin(), m_graveyard.begin() + 16);
        }
        m_trackers.erase(it);
    }
}

bool TBDRBarrierOptimizer::optimize_dependency_info(
    VkCommandBuffer cmd,
    const VkDependencyInfo* pSrc,
    VkDependencyInfo& outOptimized,
    ScratchStorage2& storage
) {
    if (!m_enabled.load(std::memory_order_relaxed)) {
        if (pSrc) outOptimized = *pSrc;
        return pSrc != nullptr;
    }

    if (!pSrc) return false;

    m_stat_total.fetch_add(1, std::memory_order_relaxed);

    bool strip_noops = m_strip_noops.load(std::memory_order_relaxed);
    bool narrow_stages = m_narrow_stages.load(std::memory_order_relaxed);
    bool dedup = m_dedup_consecutive.load(std::memory_order_relaxed);

    storage.memoryBarriers.clear();
    storage.bufferBarriers.clear();
    storage.imageBarriers.clear();

    uint64_t barrier_hash = 14695981039346656037ULL; // FNV offset basis
    hash_combine_64(barrier_hash, (uint64_t)pSrc->dependencyFlags);

    bool has_layout_transition = false;
    bool has_present_barrier = false;

    // 1. Process and narrow Image Memory Barriers
    if (pSrc->imageMemoryBarrierCount > 0 && pSrc->pImageMemoryBarriers != nullptr) {
        storage.imageBarriers.reserve(pSrc->imageMemoryBarrierCount);
        for (uint32_t i = 0; i < pSrc->imageMemoryBarrierCount; ++i) {
            VkImageMemoryBarrier2 b = pSrc->pImageMemoryBarriers[i];

            if (b.oldLayout != b.newLayout) {
                has_layout_transition = true;
            }
            if (b.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                b.oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                b.newLayout == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR ||
                b.oldLayout == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR) {
                has_present_barrier = true;
            }

            // Check for No-Op Image Barrier:
            // Same layout, same queue family, identical stages, and read-only access on both sides.
            if (strip_noops) {
                if (b.oldLayout == b.newLayout &&
                    b.srcQueueFamilyIndex == b.dstQueueFamilyIndex) {
                    if (b.srcAccessMask != 0 && b.dstAccessMask != 0 &&
                        is_read_only_access2(b.srcAccessMask) && is_read_only_access2(b.dstAccessMask)) {
                        m_stat_stripped_img.fetch_add(1, std::memory_order_relaxed);
                        continue; // Strip strictly redundant read-only image barrier!
                    }
                }
            }

            // TBDR Stage Narrowing:
            // Replace over-broad ALL_COMMANDS_BIT with safe GPU pipeline stages (graphics, transfer, compute).
            // NEVER narrow presentation barriers to preserve SurfaceFlinger display sync!
            if (narrow_stages && !has_present_barrier) {
                if (narrow_stages2(b.srcStageMask, b.srcAccessMask)) {
                    m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                }
                if (narrow_stages2(b.dstStageMask, b.dstAccessMask)) {
                    m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // Accumulate robust hash for deduplication (including all subresource and queue details)
            hash_combine_64(barrier_hash, (uint64_t)(uintptr_t)b.image);
            hash_combine_64(barrier_hash, (uint64_t)b.oldLayout);
            hash_combine_64(barrier_hash, (uint64_t)b.newLayout);
            hash_combine_64(barrier_hash, (uint64_t)b.srcStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.aspectMask);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.baseMipLevel);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.levelCount);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.baseArrayLayer);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.layerCount);
            hash_combine_64(barrier_hash, (uint64_t)b.srcQueueFamilyIndex);
            hash_combine_64(barrier_hash, (uint64_t)b.dstQueueFamilyIndex);

            storage.imageBarriers.push_back(b);
        }
    }

    // 2. Process and narrow Buffer Memory Barriers
    if (pSrc->bufferMemoryBarrierCount > 0 && pSrc->pBufferMemoryBarriers != nullptr) {
        storage.bufferBarriers.reserve(pSrc->bufferMemoryBarrierCount);
        for (uint32_t i = 0; i < pSrc->bufferMemoryBarrierCount; ++i) {
            VkBufferMemoryBarrier2 b = pSrc->pBufferMemoryBarriers[i];

            if (strip_noops) {
                if (b.srcQueueFamilyIndex == b.dstQueueFamilyIndex) {
                    if (b.srcAccessMask != 0 && b.dstAccessMask != 0 &&
                        is_read_only_access2(b.srcAccessMask) && is_read_only_access2(b.dstAccessMask)) {
                        m_stat_stripped_buf.fetch_add(1, std::memory_order_relaxed);
                        continue; // Strip redundant buffer barrier!
                    }
                }
            }

            if (narrow_stages) {
                if (narrow_stages2(b.srcStageMask, b.srcAccessMask)) {
                    m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                }
                if (narrow_stages2(b.dstStageMask, b.dstAccessMask)) {
                    m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                }
            }

            hash_combine_64(barrier_hash, (uint64_t)(uintptr_t)b.buffer);
            hash_combine_64(barrier_hash, (uint64_t)b.offset);
            hash_combine_64(barrier_hash, (uint64_t)b.size);
            hash_combine_64(barrier_hash, (uint64_t)b.srcStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.srcQueueFamilyIndex);
            hash_combine_64(barrier_hash, (uint64_t)b.dstQueueFamilyIndex);

            storage.bufferBarriers.push_back(b);
        }
    }

    // 3. Process Memory Barriers
    if (pSrc->memoryBarrierCount > 0 && pSrc->pMemoryBarriers != nullptr) {
        storage.memoryBarriers.reserve(pSrc->memoryBarrierCount);
        for (uint32_t i = 0; i < pSrc->memoryBarrierCount; ++i) {
            VkMemoryBarrier2 b = pSrc->pMemoryBarriers[i];
            if (strip_noops) {
                // Only strip if stages are also identical! If stages differ, it's a critical execution barrier.
                if (b.srcAccessMask == 0 && b.dstAccessMask == 0 && b.srcStageMask == b.dstStageMask) {
                    continue;
                }
            }

            if (narrow_stages) {
                if (narrow_stages2(b.srcStageMask, b.srcAccessMask)) {
                    m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                }
                if (narrow_stages2(b.dstStageMask, b.dstAccessMask)) {
                    m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                }
            }

            hash_combine_64(barrier_hash, (uint64_t)b.srcStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);

            storage.memoryBarriers.push_back(b);
        }
    }

    // 4. Consecutive Duplicate Barrier Check
    if (dedup) {
        std::lock_guard<std::mutex> lock(m_tracker_mutex);
        CmdTracker& tracker = get_or_create_tracker(cmd);

        // SAFETY RULE: NEVER drop or deduplicate barriers containing layout transitions or presentation!
        // Dropping layout transitions desynchronizes tile compression (AFBC/UBWC) and causes screen flickering.
        if (!has_layout_transition && !has_present_barrier) {
            if (tracker.hasLastBarrier && !tracker.hadAction.load(std::memory_order_relaxed)) {
                if (tracker.lastBarrier.hash == barrier_hash &&
                    tracker.lastBarrier.imgCount == (uint32_t)storage.imageBarriers.size() &&
                    tracker.lastBarrier.bufCount == (uint32_t)storage.bufferBarriers.size() &&
                    tracker.lastBarrier.memCount == (uint32_t)storage.memoryBarriers.size()) {
                    m_stat_eliminated.fetch_add(1, std::memory_order_relaxed);
                    LOG_OPT_DEBUG("TBDRBarrierOptimizer: eliminated consecutive duplicate barrier for cmd %p (hash %llx)",
                                  cmd, (unsigned long long)barrier_hash);
                    return false; // Safely dropped only when NO layout transitions or present operations exist!
                }
            }
        }

        // Record last barrier fingerprint
        tracker.lastBarrier.hash = barrier_hash;
        tracker.lastBarrier.dependencyFlags = pSrc->dependencyFlags;
        tracker.lastBarrier.imgCount = (uint32_t)storage.imageBarriers.size();
        tracker.lastBarrier.bufCount = (uint32_t)storage.bufferBarriers.size();
        tracker.lastBarrier.memCount = (uint32_t)storage.memoryBarriers.size();
        tracker.hasLastBarrier = true;
        tracker.hadAction.store(false, std::memory_order_relaxed);
    }

    // 5. Build optimized DependencyInfo
    outOptimized = *pSrc;
    outOptimized.memoryBarrierCount = (uint32_t)storage.memoryBarriers.size();
    outOptimized.pMemoryBarriers = storage.memoryBarriers.empty() ? nullptr : storage.memoryBarriers.data();
    outOptimized.bufferMemoryBarrierCount = (uint32_t)storage.bufferBarriers.size();
    outOptimized.pBufferMemoryBarriers = storage.bufferBarriers.empty() ? nullptr : storage.bufferBarriers.data();
    outOptimized.imageMemoryBarrierCount = (uint32_t)storage.imageBarriers.size();
    outOptimized.pImageMemoryBarriers = storage.imageBarriers.empty() ? nullptr : storage.imageBarriers.data();

    return true;
}

bool TBDRBarrierOptimizer::optimize_pipeline_barrier1(
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
) {
    if (!m_enabled.load(std::memory_order_relaxed)) return true;

    m_stat_total.fetch_add(1, std::memory_order_relaxed);

    bool strip_noops = m_strip_noops.load(std::memory_order_relaxed);
    bool narrow_stages = m_narrow_stages.load(std::memory_order_relaxed);
    bool dedup = m_dedup_consecutive.load(std::memory_order_relaxed);

    storage.memoryBarriers.clear();
    storage.bufferBarriers.clear();
    storage.imageBarriers.clear();

    uint64_t barrier_hash = 14695981039346656037ULL;
    hash_combine_64(barrier_hash, (uint64_t)inOutSrcStageMask);
    hash_combine_64(barrier_hash, (uint64_t)inOutDstStageMask);
    hash_combine_64(barrier_hash, (uint64_t)inOutDependencyFlags);

    bool has_layout_transition = false;
    bool has_present_barrier = false;

    // 1. Process Image Barriers
    if (inOutImgCount > 0 && pInOutImgBarriers != nullptr) {
        storage.imageBarriers.reserve(inOutImgCount);
        for (uint32_t i = 0; i < inOutImgCount; ++i) {
            const auto& b = pInOutImgBarriers[i];

            if (b.oldLayout != b.newLayout) {
                has_layout_transition = true;
            }
            if (b.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                b.oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                b.newLayout == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR ||
                b.oldLayout == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR) {
                has_present_barrier = true;
            }

            if (strip_noops) {
                if (b.oldLayout == b.newLayout &&
                    b.srcQueueFamilyIndex == b.dstQueueFamilyIndex) {
                    if (b.srcAccessMask != 0 && b.dstAccessMask != 0 &&
                        is_read_only_access1(b.srcAccessMask) && is_read_only_access1(b.dstAccessMask)) {
                        m_stat_stripped_img.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                }
            }

            // Accumulate robust hash for deduplication
            hash_combine_64(barrier_hash, (uint64_t)(uintptr_t)b.image);
            hash_combine_64(barrier_hash, (uint64_t)b.oldLayout);
            hash_combine_64(barrier_hash, (uint64_t)b.newLayout);
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.aspectMask);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.baseMipLevel);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.levelCount);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.baseArrayLayer);
            hash_combine_64(barrier_hash, (uint64_t)b.subresourceRange.layerCount);
            hash_combine_64(barrier_hash, (uint64_t)b.srcQueueFamilyIndex);
            hash_combine_64(barrier_hash, (uint64_t)b.dstQueueFamilyIndex);

            storage.imageBarriers.push_back(b);
        }
    }

    // 2. Process Buffer Barriers
    if (inOutBufCount > 0 && pInOutBufBarriers != nullptr) {
        storage.bufferBarriers.reserve(inOutBufCount);
        for (uint32_t i = 0; i < inOutBufCount; ++i) {
            const auto& b = pInOutBufBarriers[i];
            if (strip_noops) {
                if (b.srcQueueFamilyIndex == b.dstQueueFamilyIndex) {
                    if (b.srcAccessMask != 0 && b.dstAccessMask != 0 &&
                        is_read_only_access1(b.srcAccessMask) && is_read_only_access1(b.dstAccessMask)) {
                        m_stat_stripped_buf.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                }
            }

            hash_combine_64(barrier_hash, (uint64_t)(uintptr_t)b.buffer);
            hash_combine_64(barrier_hash, (uint64_t)b.offset);
            hash_combine_64(barrier_hash, (uint64_t)b.size);
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.srcQueueFamilyIndex);
            hash_combine_64(barrier_hash, (uint64_t)b.dstQueueFamilyIndex);

            storage.bufferBarriers.push_back(b);
        }
    }

    // 3. Process Memory Barriers
    if (inOutMemCount > 0 && pInOutMemBarriers != nullptr) {
        storage.memoryBarriers.reserve(inOutMemCount);
        for (uint32_t i = 0; i < inOutMemCount; ++i) {
            const auto& b = pInOutMemBarriers[i];
            if (strip_noops) {
                // Only strip if stages are also identical!
                if (b.srcAccessMask == 0 && b.dstAccessMask == 0 && inOutSrcStageMask == inOutDstStageMask) {
                    continue;
                }
            }
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);
            storage.memoryBarriers.push_back(b);
        }
    }

    // 4. Deduplication check
    if (dedup) {
        std::lock_guard<std::mutex> lock(m_tracker_mutex);
        CmdTracker& tracker = get_or_create_tracker(cmd);

        // SAFETY RULE: NEVER drop or deduplicate barriers containing layout transitions or swapchain presentation!
        if (!has_layout_transition && !has_present_barrier) {
            if (tracker.hasLastBarrier && !tracker.hadAction.load(std::memory_order_relaxed)) {
                if (tracker.lastBarrier.hash == barrier_hash &&
                    tracker.lastBarrier.imgCount == (uint32_t)storage.imageBarriers.size() &&
                    tracker.lastBarrier.bufCount == (uint32_t)storage.bufferBarriers.size() &&
                    tracker.lastBarrier.memCount == (uint32_t)storage.memoryBarriers.size()) {
                    m_stat_eliminated.fetch_add(1, std::memory_order_relaxed);
                    return false; // Safely dropped only when NO layout transitions or present operations exist!
                }
            }
        }

        tracker.lastBarrier.hash = barrier_hash;
        tracker.lastBarrier.dependencyFlags = inOutDependencyFlags;
        tracker.lastBarrier.imgCount = (uint32_t)storage.imageBarriers.size();
        tracker.lastBarrier.bufCount = (uint32_t)storage.bufferBarriers.size();
        tracker.lastBarrier.memCount = (uint32_t)storage.memoryBarriers.size();
        tracker.hasLastBarrier = true;
        tracker.hadAction.store(false, std::memory_order_relaxed);
    }

    // 5. TBDR Stage Narrowing for Vulkan 1.0 barriers
    // Safely narrow ALL_COMMANDS_BIT to ALL_GRAPHICS_BIT + TRANSFER_BIT + COMPUTE_SHADER_BIT, avoiding host/CPU bubbles.
    // NEVER narrow presentation barriers to preserve display sync!
    if (narrow_stages && !has_present_barrier) {
        VkAccessFlags srcAccess = 0;
        VkAccessFlags dstAccess = 0;
        for (const auto& b : storage.imageBarriers) {
            srcAccess |= b.srcAccessMask;
            dstAccess |= b.dstAccessMask;
        }
        for (const auto& b : storage.bufferBarriers) {
            srcAccess |= b.srcAccessMask;
            dstAccess |= b.dstAccessMask;
        }
        for (const auto& b : storage.memoryBarriers) {
            srcAccess |= b.srcAccessMask;
            dstAccess |= b.dstAccessMask;
        }
        if (narrow_stages1(inOutSrcStageMask, srcAccess)) {
            m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
        }
        if (narrow_stages1(inOutDstStageMask, dstAccess)) {
            m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    inOutMemCount = (uint32_t)storage.memoryBarriers.size();
    pInOutMemBarriers = storage.memoryBarriers.empty() ? nullptr : storage.memoryBarriers.data();
    inOutBufCount = (uint32_t)storage.bufferBarriers.size();
    pInOutBufBarriers = storage.bufferBarriers.empty() ? nullptr : storage.bufferBarriers.data();
    inOutImgCount = (uint32_t)storage.imageBarriers.size();
    pInOutImgBarriers = storage.imageBarriers.empty() ? nullptr : storage.imageBarriers.data();

    return true;
}

TBDRBarrierOptimizer::Stats TBDRBarrierOptimizer::get_stats() const {
    Stats s;
    s.total_barriers = m_stat_total.load(std::memory_order_relaxed);
    s.eliminated_barriers = m_stat_eliminated.load(std::memory_order_relaxed);
    s.narrowed_stages = m_stat_narrowed.load(std::memory_order_relaxed);
    s.stripped_image_barriers = m_stat_stripped_img.load(std::memory_order_relaxed);
    s.stripped_buffer_barriers = m_stat_stripped_buf.load(std::memory_order_relaxed);
    return s;
}







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

TBDRBarrierOptimizer& TBDRBarrierOptimizer::get() {
    static TBDRBarrierOptimizer s_instance;
    return s_instance;
}

TBDRBarrierOptimizer::TBDRBarrierOptimizer() {
    const char* env_opt = getenv("VULKAN_FIX_OPTIMIZE_BARRIER");
    if (env_opt && (strcmp(env_opt, "1") == 0 || strcasecmp(env_opt, "true") == 0)) {
        m_enabled.store(true, std::memory_order_relaxed);
        LOGI("TBDRBarrierOptimizer: ENABLED via environment variable (experimental barrier optimization)");
    } else {
        m_enabled.store(false, std::memory_order_relaxed);
        LOGI("TBDRBarrierOptimizer: disabled by default (pass-through for 100%% exact native driver barrier stability)");
    }

    const char* env_narrow = getenv("VULKAN_FIX_BARRIER_NARROW");
    if (env_narrow && (strcmp(env_narrow, "1") == 0 || strcasecmp(env_narrow, "true") == 0)) {
        m_narrow_stages.store(true, std::memory_order_relaxed);
    } else {
        m_narrow_stages.store(false, std::memory_order_relaxed);
    }

    const char* env_dedup = getenv("VULKAN_FIX_BARRIER_DEDUP");
    if (env_dedup && (strcmp(env_dedup, "1") == 0 || strcasecmp(env_dedup, "true") == 0)) {
        m_dedup_consecutive.store(true, std::memory_order_relaxed);
    } else {
        m_dedup_consecutive.store(false, std::memory_order_relaxed);
    }

    const char* env_strip = getenv("VULKAN_FIX_BARRIER_STRIP");
    if (env_strip && (strcmp(env_strip, "1") == 0 || strcasecmp(env_strip, "true") == 0)) {
        m_strip_noops.store(true, std::memory_order_relaxed);
    } else {
        m_strip_noops.store(false, std::memory_order_relaxed);
    }
}

TBDRBarrierOptimizer::CmdTracker& TBDRBarrierOptimizer::get_or_create_tracker(VkCommandBuffer cmd) {
    auto it = m_trackers.find((uint64_t)(uintptr_t)cmd);
    if (it != m_trackers.end()) {
        return *it->second;
    }
    auto tracker = std::make_unique<CmdTracker>();
    CmdTracker& ref = *tracker;
    m_trackers[(uint64_t)(uintptr_t)cmd] = std::move(tracker);
    return ref;
}

void TBDRBarrierOptimizer::notify_action_slow(VkCommandBuffer cmd, VkCommandBuffer& out_cached_cmd, CmdTracker*& out_cached_tracker) {
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    CmdTracker& tracker = get_or_create_tracker(cmd);
    tracker.hadAction.store(true, std::memory_order_relaxed);
    out_cached_cmd = cmd;
    out_cached_tracker = &tracker;
}

void TBDRBarrierOptimizer::on_cmd_begin(VkCommandBuffer cmd) {
    if (!m_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    auto it = m_trackers.find((uint64_t)(uintptr_t)cmd);
    if (it != m_trackers.end()) {
        it->second->hasLastBarrier = false;
        it->second->hadAction.store(false, std::memory_order_relaxed);
    }
}

void TBDRBarrierOptimizer::on_cmd_reset(VkCommandBuffer cmd) {
    on_cmd_begin(cmd);
}

void TBDRBarrierOptimizer::on_cmd_free(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_tracker_mutex);
    m_trackers.erase((uint64_t)(uintptr_t)cmd);
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

    // 1. Process and narrow Image Memory Barriers
    if (pSrc->imageMemoryBarrierCount > 0 && pSrc->pImageMemoryBarriers != nullptr) {
        storage.imageBarriers.reserve(pSrc->imageMemoryBarrierCount);
        for (uint32_t i = 0; i < pSrc->imageMemoryBarrierCount; ++i) {
            VkImageMemoryBarrier2 b = pSrc->pImageMemoryBarriers[i];

            // Check for No-Op Image Barrier:
            // Same layout, same queue family, identical stages, and read-only access on both sides.
            if (strip_noops) {
                if (b.oldLayout == b.newLayout &&
                    b.srcQueueFamilyIndex == b.dstQueueFamilyIndex &&
                    b.srcStageMask == b.dstStageMask) {
                    if (b.srcAccessMask != 0 && b.dstAccessMask != 0 &&
                        is_read_only_access2(b.srcAccessMask) && is_read_only_access2(b.dstAccessMask)) {
                        m_stat_stripped_img.fetch_add(1, std::memory_order_relaxed);
                        continue; // Strip strictly redundant read-only image barrier!
                    }
                }
            }

            // TBDR Stage Narrowing:
            // Replace over-broad ALL_COMMANDS_BIT / ALL_GRAPHICS_BIT with fine-grained stages.
            if (narrow_stages) {
                if (b.srcStageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
                    if (b.srcAccessMask & (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT)) {
                        b.srcStageMask = (b.srcStageMask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                        m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                    } else if (b.srcAccessMask & (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT)) {
                        b.srcStageMask = (b.srcStageMask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                        m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                    } else if (b.srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT) {
                        b.srcStageMask = (b.srcStageMask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                        m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                if (b.dstStageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
                    if (b.dstAccessMask & (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) {
                        b.dstStageMask = (b.dstStageMask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) |
                                         (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT);
                        m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                    } else if (b.dstAccessMask & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) {
                        b.dstStageMask = (b.dstStageMask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                        m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                    } else if (b.dstAccessMask & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) {
                        b.dstStageMask = (b.dstStageMask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
                        m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            // Accumulate hash for deduplication
            hash_combine_64(barrier_hash, (uint64_t)(uintptr_t)b.image);
            hash_combine_64(barrier_hash, (uint64_t)b.oldLayout);
            hash_combine_64(barrier_hash, (uint64_t)b.newLayout);
            hash_combine_64(barrier_hash, (uint64_t)b.srcStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);

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
                if (b.dstStageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
                    if (b.dstAccessMask & (VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) {
                        b.dstStageMask = (b.dstStageMask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) |
                                         (VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
                        m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            hash_combine_64(barrier_hash, (uint64_t)(uintptr_t)b.buffer);
            hash_combine_64(barrier_hash, (uint64_t)b.offset);
            hash_combine_64(barrier_hash, (uint64_t)b.size);
            hash_combine_64(barrier_hash, (uint64_t)b.srcStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstStageMask);
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);

            storage.bufferBarriers.push_back(b);
        }
    }

    // 3. Process Memory Barriers
    if (pSrc->memoryBarrierCount > 0 && pSrc->pMemoryBarriers != nullptr) {
        storage.memoryBarriers.reserve(pSrc->memoryBarrierCount);
        for (uint32_t i = 0; i < pSrc->memoryBarrierCount; ++i) {
            VkMemoryBarrier2 b = pSrc->pMemoryBarriers[i];
            if (strip_noops) {
                if (b.srcAccessMask == 0 && b.dstAccessMask == 0) {
                    continue;
                }
            }

            if (narrow_stages) {
                if (b.srcStageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
                    b.srcStageMask = (b.srcStageMask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
                }
                if (b.dstStageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
                    b.dstStageMask = (b.dstStageMask & ~VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
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

        if (tracker.hasLastBarrier && !tracker.hadAction.load(std::memory_order_relaxed)) {
            if (tracker.lastBarrier.hash == barrier_hash &&
                tracker.lastBarrier.imgCount == (uint32_t)storage.imageBarriers.size() &&
                tracker.lastBarrier.bufCount == (uint32_t)storage.bufferBarriers.size() &&
                tracker.lastBarrier.memCount == (uint32_t)storage.memoryBarriers.size()) {
                m_stat_eliminated.fetch_add(1, std::memory_order_relaxed);
                LOG_OPT_DEBUG("TBDRBarrierOptimizer: eliminated consecutive duplicate barrier for cmd %p (hash %llx)",
                              cmd, (unsigned long long)barrier_hash);
                return false; // COMPLETELY DROPPED!
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

    // 1. Process Image Barriers
    if (inOutImgCount > 0 && pInOutImgBarriers != nullptr) {
        storage.imageBarriers.reserve(inOutImgCount);
        for (uint32_t i = 0; i < inOutImgCount; ++i) {
            const auto& b = pInOutImgBarriers[i];
            if (strip_noops) {
                if (b.oldLayout == b.newLayout &&
                    b.srcQueueFamilyIndex == b.dstQueueFamilyIndex &&
                    inOutSrcStageMask == inOutDstStageMask) {
                    if (b.srcAccessMask != 0 && b.dstAccessMask != 0 &&
                        is_read_only_access1(b.srcAccessMask) && is_read_only_access1(b.dstAccessMask)) {
                        m_stat_stripped_img.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                }
            }

            hash_combine_64(barrier_hash, (uint64_t)(uintptr_t)b.image);
            hash_combine_64(barrier_hash, (uint64_t)b.oldLayout);
            hash_combine_64(barrier_hash, (uint64_t)b.newLayout);
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);
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
            storage.bufferBarriers.push_back(b);
        }
    }

    // 3. Process Memory Barriers
    if (inOutMemCount > 0 && pInOutMemBarriers != nullptr) {
        storage.memoryBarriers.reserve(inOutMemCount);
        for (uint32_t i = 0; i < inOutMemCount; ++i) {
            const auto& b = pInOutMemBarriers[i];
            if (strip_noops && b.srcAccessMask == 0 && b.dstAccessMask == 0) {
                continue;
            }
            hash_combine_64(barrier_hash, (uint64_t)b.srcAccessMask);
            hash_combine_64(barrier_hash, (uint64_t)b.dstAccessMask);
            storage.memoryBarriers.push_back(b);
        }
    }

    // Deduplication check
    if (dedup) {
        std::lock_guard<std::mutex> lock(m_tracker_mutex);
        CmdTracker& tracker = get_or_create_tracker(cmd);

        if (tracker.hasLastBarrier && !tracker.hadAction.load(std::memory_order_relaxed)) {
            if (tracker.lastBarrier.hash == barrier_hash &&
                tracker.lastBarrier.imgCount == (uint32_t)storage.imageBarriers.size() &&
                tracker.lastBarrier.bufCount == (uint32_t)storage.bufferBarriers.size() &&
                tracker.lastBarrier.memCount == (uint32_t)storage.memoryBarriers.size()) {
                m_stat_eliminated.fetch_add(1, std::memory_order_relaxed);
                return false; // DROPPED!
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

    // TBDR Stage Narrowing for Vulkan 1.0 barriers
    if (narrow_stages) {
        if (inOutSrcStageMask & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) {
            bool only_color = !storage.imageBarriers.empty();
            for (const auto& ib : storage.imageBarriers) {
                if (ib.srcAccessMask & (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)) {
                    continue;
                }
                only_color = false;
                break;
            }
            if (only_color) {
                inOutSrcStageMask = (inOutSrcStageMask & ~VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (inOutDstStageMask & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) {
            bool only_fragment_sampling = !storage.imageBarriers.empty();
            for (const auto& ib : storage.imageBarriers) {
                if (ib.dstAccessMask & VK_ACCESS_SHADER_READ_BIT) {
                    continue;
                }
                only_fragment_sampling = false;
                break;
            }
            if (only_fragment_sampling) {
                inOutDstStageMask = (inOutDstStageMask & ~VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                m_stat_narrowed.fetch_add(1, std::memory_order_relaxed);
            }
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

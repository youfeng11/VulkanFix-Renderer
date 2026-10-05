#ifndef PIPELINE_CACHE_MANAGER_H
#define PIPELINE_CACHE_MANAGER_H

#include "vk_common.h"
#include <string>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <unordered_map>

struct DevicePipelineCacheState {
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineCache disk_cache = VK_NULL_HANDLE;
    std::string cache_file_path;
    bool is_valid = false;
    std::atomic<uint32_t> pending_pipelines_count{0};
    std::atomic<bool> is_dirty{false};
    uint8_t driver_uuid[VK_UUID_SIZE]{};
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    mutable std::mutex cache_api_mutex;
};

/**
 * PipelineCacheManager
 * 
 * Provides global, disk-backed persistent pipeline caching (Disk-backed Pipeline Cache).
 * Eliminates shader compilation stutter (1% low FPS dips) by:
 * 1. Preloading compiled binary shader microcode from persistent storage at device creation.
 * 2. Automatically substituting null pipeline caches in vkCreateGraphicsPipelines / vkCreateComputePipelines.
 * 3. Bidirectionally merging with app-provided pipeline caches.
 * 4. Asynchronously and safely flushing newly compiled pipelines to storage in the background.
 */
class PipelineCacheManager {
public:
    static PipelineCacheManager& get();

    // Initialize disk cache for a newly created logical device
    void init_for_device(VkPhysicalDevice physicalDevice, VkDevice device);

    // Synchronize and destroy cache when logical device is being destroyed
    void on_destroy_device(VkDevice device);

    // Retrieve disk-backed cache for device
    VkPipelineCache get_disk_cache(VkDevice device);

    // Prepare effective pipeline cache before pipeline creation
    VkPipelineCache prepare_pipeline_cache(VkDevice device, VkPipelineCache appCache);

    // Called after graphics/compute pipelines are successfully created
    void on_pipelines_created(VkDevice device, VkPipelineCache usedCache, uint32_t count);

    // Explicitly synchronize cache to disk
    bool sync_device_cache_to_disk(VkDevice device, bool force_sync = false);

    // Hook for when application creates its own VkPipelineCache
    void on_post_create_app_pipeline_cache(VkDevice device, VkPipelineCache appCache);

    // Hook for when application destroys its own VkPipelineCache
    void on_pre_destroy_app_pipeline_cache(VkDevice device, VkPipelineCache appCache);

    bool is_enabled() const { return m_enabled.load(std::memory_order_relaxed); }

private:
    PipelineCacheManager();
    ~PipelineCacheManager();
    PipelineCacheManager(const PipelineCacheManager&) = delete;
    PipelineCacheManager& operator=(const PipelineCacheManager&) = delete;

    std::string resolve_cache_path(VkPhysicalDevice physDev, uint32_t vendorId, uint32_t deviceId, const uint8_t* uuid);
    bool load_initial_cache_data(const std::string& path, uint32_t vendorId, uint32_t deviceId, const uint8_t* uuid, std::vector<uint8_t>& outData);
    bool write_cache_data_to_file(const std::string& path, const void* data, size_t size);

    void start_flusher_thread();
    void stop_flusher_thread();
    void flusher_thread_loop();

    std::atomic<bool> m_enabled{true};

    // Fast-path primary device cache
    std::atomic<VkDevice> m_primary_device{VK_NULL_HANDLE};
    std::atomic<VkPipelineCache> m_primary_cache{VK_NULL_HANDLE};

    mutable std::shared_mutex m_mutex;
    std::unordered_map<uint64_t, std::unique_ptr<DevicePipelineCacheState>> m_devices;

    // Asynchronous background flusher
    std::thread m_flusher_thread;
    std::mutex m_flusher_mutex;
    std::condition_variable m_flusher_cv;
    std::atomic<bool> m_flusher_running{false};
    std::atomic<bool> m_has_pending_flush{false};
    std::atomic<int64_t> m_last_pipeline_created_time_ms{0};
};

#endif // PIPELINE_CACHE_MANAGER_H

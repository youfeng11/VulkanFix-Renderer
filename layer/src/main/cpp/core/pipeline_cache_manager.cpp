#include "pipeline_cache_manager.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <cstdlib>

static bool ensure_directory_exists(const std::string& dir_path) {
    if (dir_path.empty()) return false;
    struct stat st;
    if (stat(dir_path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    size_t pos = dir_path.find_last_of("/\\");
    if (pos != std::string::npos && pos > 0) {
        ensure_directory_exists(dir_path.substr(0, pos));
    }
    return (mkdir(dir_path.c_str(), 0777) == 0);
}

static bool test_file_writable(const std::string& filepath) {
    size_t pos = filepath.find_last_of("/\\");
    if (pos != std::string::npos && pos > 0) {
        ensure_directory_exists(filepath.substr(0, pos));
    }
    FILE* f = fopen(filepath.c_str(), "ab+");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

PipelineCacheManager& PipelineCacheManager::get() {
    static PipelineCacheManager s_instance;
    return s_instance;
}

PipelineCacheManager::PipelineCacheManager() {
    const char* env_dis = getenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE");
    if (env_dis && (strcmp(env_dis, "1") == 0 || strcmp(env_dis, "true") == 0)) {
        m_enabled.store(false, std::memory_order_relaxed);
        LOGI("PipelineCacheManager: Disabled by environment variable");
        return;
    }

    start_flusher_thread();
    LOGI("PipelineCacheManager: Initialized with disk-backed persistent caching enabled");
}

PipelineCacheManager::~PipelineCacheManager() {
    stop_flusher_thread();
}

void PipelineCacheManager::start_flusher_thread() {
    if (m_flusher_running.load()) return;
    m_flusher_running.store(true);
    m_flusher_thread = std::thread(&PipelineCacheManager::flusher_thread_loop, this);
}

void PipelineCacheManager::stop_flusher_thread() {
    if (!m_flusher_running.load()) return;
    {
        std::lock_guard<std::mutex> lock(m_flusher_mutex);
        m_flusher_running.store(false);
        m_flusher_cv.notify_all();
    }
    if (m_flusher_thread.joinable()) {
        m_flusher_thread.join();
    }
}

void PipelineCacheManager::flusher_thread_loop() {
    while (m_flusher_running.load()) {
        std::unique_lock<std::mutex> lock(m_flusher_mutex);
        m_flusher_cv.wait(lock, [this]() {
            return !m_flusher_running.load() || m_has_pending_flush.load();
        });

        if (!m_flusher_running.load()) break;

        if (m_has_pending_flush.load()) {
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t last_time = m_last_pipeline_created_time_ms.load(std::memory_order_relaxed);

            // The pending flag remains true during debounce, so it must not be
            // the predicate for this timed wait: that would busy-spin.
            const int64_t remaining_ms = 2500 - (now_ms - last_time);
            if (remaining_ms > 0) {
                m_flusher_cv.wait_for(lock, std::chrono::milliseconds(remaining_ms), [this, last_time]() {
                    return !m_flusher_running.load() ||
                        m_last_pipeline_created_time_ms.load() != last_time;
                });
                continue;
            }
            {
                m_has_pending_flush.store(false, std::memory_order_release);
                lock.unlock();

                std::vector<VkDevice> devices_to_sync;
                {
                    std::shared_lock<std::shared_mutex> dev_lock(m_mutex);
                    for (const auto& kv : m_devices) {
                        if (kv.second && kv.second->is_dirty.load(std::memory_order_acquire)) {
                            devices_to_sync.push_back((VkDevice)(uintptr_t)kv.first);
                        }
                    }
                }

                for (VkDevice dev : devices_to_sync) {
                    sync_device_cache_to_disk(dev, false);
                }
            }
        }
    }

    // Flush any remaining dirty caches on shutdown
    std::vector<VkDevice> devices_to_sync;
    {
        std::shared_lock<std::shared_mutex> dev_lock(m_mutex);
        for (const auto& kv : m_devices) {
            if (kv.second && kv.second->is_dirty.load(std::memory_order_acquire)) {
                devices_to_sync.push_back((VkDevice)(uintptr_t)kv.first);
            }
        }
    }
    for (VkDevice dev : devices_to_sync) {
        sync_device_cache_to_disk(dev, true);
    }
}

std::string PipelineCacheManager::resolve_cache_path(
    VkPhysicalDevice physDev,
    uint32_t vendorId,
    uint32_t deviceId,
    const uint8_t* uuid
) {
    const char* custom_path = getenv("VULKAN_FIX_PIPELINE_CACHE_PATH");
    if (custom_path && custom_path[0] != '\0') {
        if (test_file_writable(custom_path)) {
            LOGI("PipelineCacheManager: Using custom cache path: %s", custom_path);
            return custom_path;
        }
    }

    const char* home = getenv("HOME");
    if (home && home[0] != '\0') {
        std::string mc_dir = std::string(home) + "/.minecraft";
        ensure_directory_exists(mc_dir);
        std::string mc_cache = mc_dir + "/vulkan_pipeline_cache.bin";
        if (test_file_writable(mc_cache)) {
            LOGI("PipelineCacheManager: Using Minecraft home cache path: %s", mc_cache.c_str());
            return mc_cache;
        }

        std::string home_cache = std::string(home) + "/vulkan_pipeline_cache.bin";
        if (test_file_writable(home_cache)) {
            LOGI("PipelineCacheManager: Using HOME cache path: %s", home_cache.c_str());
            return home_cache;
        }
    }

    // Try current working directory
    std::string cwd_cache = "./vulkan_pipeline_cache.bin";
    if (test_file_writable(cwd_cache)) {
        LOGI("PipelineCacheManager: Using current directory cache path: %s", cwd_cache.c_str());
        return cwd_cache;
    }

    // Try common PojavLauncher / Android paths
    const char* ext_storage = getenv("EXTERNAL_STORAGE");
    if (ext_storage && ext_storage[0] != '\0') {
        std::string pojav_cache1 = std::string(ext_storage) + "/Android/data/net.kdt.pojavlaunch/files/vulkan_pipeline_cache.bin";
        if (test_file_writable(pojav_cache1)) {
            LOGI("PipelineCacheManager: Using PojavLauncher external cache path: %s", pojav_cache1.c_str());
            return pojav_cache1;
        }
        std::string pojav_cache2 = std::string(ext_storage) + "/Android/data/org.pojavlauncher.mobile/files/vulkan_pipeline_cache.bin";
        if (test_file_writable(pojav_cache2)) {
            LOGI("PipelineCacheManager: Using PojavLauncher mobile cache path: %s", pojav_cache2.c_str());
            return pojav_cache2;
        }
    }

    // Fallback to local tmp
    std::string tmp_cache = "/data/local/tmp/vulkan_pipeline_cache.bin";
    if (test_file_writable(tmp_cache)) {
        LOGI("PipelineCacheManager: Using fallback tmp cache path: %s", tmp_cache.c_str());
        return tmp_cache;
    }

    return "vulkan_pipeline_cache.bin";
}

bool PipelineCacheManager::load_initial_cache_data(
    const std::string& path,
    uint32_t vendorId,
    uint32_t deviceId,
    const uint8_t* uuid,
    std::vector<uint8_t>& outData
) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < (long)sizeof(VkPipelineCacheHeaderVersionOne)) {
        fclose(f);
        return false;
    }

    VkPipelineCacheHeaderVersionOne header{};
    if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return false;
    }

    // Validate header compatibility according to Vulkan Specification
    if (header.headerSize < sizeof(VkPipelineCacheHeaderVersionOne) ||
        header.headerSize > static_cast<unsigned long>(sz) ||
        header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        header.vendorID != vendorId ||
        header.deviceID != deviceId ||
        memcmp(header.pipelineCacheUUID, uuid, VK_UUID_SIZE) != 0) {
        LOGW("PipelineCacheManager: Cache file '%s' exists but header mismatch (vendor=0x%x/0x%x, dev=0x%x/0x%x). Discarding stale cache.",
             path.c_str(), header.vendorID, vendorId, header.deviceID, deviceId);
        fclose(f);
        return false;
    }

    outData.resize(sz);
    memcpy(outData.data(), &header, sizeof(header));
    size_t remaining = sz - sizeof(header);
    if (remaining > 0) {
        if (fread(outData.data() + sizeof(header), 1, remaining, f) != remaining) {
            fclose(f);
            outData.clear();
            return false;
        }
    }

    fclose(f);
    LOGI("PipelineCacheManager: Successfully read %zu bytes valid cache data from '%s'", outData.size(), path.c_str());
    return true;
}

bool PipelineCacheManager::write_cache_data_to_file(const std::string& path, const void* data, size_t size) {
    if (path.empty() || !data || size == 0) return false;

    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos && pos > 0) {
        ensure_directory_exists(path.substr(0, pos));
    }

    std::string tmp_path = path + ".tmp.XXXXXX";
    const int fd = mkostemp(tmp_path.data(), O_CLOEXEC);
    if (fd < 0) {
        LOGE("PipelineCacheManager: Failed to create cache temp file for '%s'", path.c_str());
        return false;
    }
    FILE* f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmp_path.c_str());
        LOGE("PipelineCacheManager: Failed to open temp file '%s' for writing", tmp_path.c_str());
        return false;
    }

    size_t written = fwrite(data, 1, size, f);
    const int flushed = fflush(f);
    const int closed = fclose(f);

    if (written != size || flushed != 0 || closed != 0) {
        LOGE("PipelineCacheManager: Failed cache write/flush to '%s' (%zu / %zu bytes)", tmp_path.c_str(), written, size);
        unlink(tmp_path.c_str());
        return false;
    }

    if (rename(tmp_path.c_str(), path.c_str()) != 0) {
        LOGE("PipelineCacheManager: Atomic rename from '%s' to '%s' failed", tmp_path.c_str(), path.c_str());
        unlink(tmp_path.c_str());
        return false;
    }

    LOGI("PipelineCacheManager: Successfully persisted %zu bytes pipeline cache to '%s'", size, path.c_str());
    return true;
}

void PipelineCacheManager::init_for_device(VkPhysicalDevice physicalDevice, VkDevice device) {
    if (!m_enabled.load(std::memory_order_relaxed) || device == VK_NULL_HANDLE) return;

    VkInstance inst = get_last_instance();
    PFN_vkGetPhysicalDeviceProperties real_props =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(inst, VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (!real_props) return;

    VkPhysicalDeviceProperties props{};
    real_props(physicalDevice, &props);

    std::string path = resolve_cache_path(physicalDevice, props.vendorID, props.deviceID, props.pipelineCacheUUID);

    std::vector<uint8_t> initialData;
    bool has_initial_data = load_initial_cache_data(path, props.vendorID, props.deviceID, props.pipelineCacheUUID, initialData);

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkCreatePipelineCache real_create_cache = dt.CreatePipelineCache ? dt.CreatePipelineCache :
        (PFN_vkCreatePipelineCache) get_real_proc(inst, device, "vkCreatePipelineCache");
    if (!real_create_cache) {
        LOGE("PipelineCacheManager: vkCreatePipelineCache function pointer not found!");
        return;
    }

    VkPipelineCacheCreateInfo createInfo{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    if (has_initial_data && !initialData.empty()) {
        createInfo.initialDataSize = initialData.size();
        createInfo.pInitialData = initialData.data();
    }

    VkPipelineCache diskCache = VK_NULL_HANDLE;
    VkResult res = real_create_cache(device, &createInfo, nullptr, &diskCache);

    // If creation with existing data failed, fallback to creating an empty cache
    if (res != VK_SUCCESS && has_initial_data) {
        LOGW("PipelineCacheManager: vkCreatePipelineCache failed with code %d using file data. Falling back to clean cache.", res);
        createInfo.initialDataSize = 0;
        createInfo.pInitialData = nullptr;
        res = real_create_cache(device, &createInfo, nullptr, &diskCache);
    }

    if (res == VK_SUCCESS && diskCache != VK_NULL_HANDLE) {
        auto state = std::make_shared<DevicePipelineCacheState>();
        state->device = device;
        state->disk_cache = diskCache;
        state->cache_file_path = path;
        state->is_valid = true;
        state->vendor_id = props.vendorID;
        state->device_id = props.deviceID;
        memcpy(state->driver_uuid, props.pipelineCacheUUID, VK_UUID_SIZE);

        {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_devices[(uint64_t)(uintptr_t)device] = std::move(state);
        }

        LOGI("PipelineCacheManager: Successfully initialized persistent pipeline cache (handle=%p) for device %p at '%s'",
             (void*)diskCache, (void*)device, path.c_str());
    } else {
        LOGE("PipelineCacheManager: Failed to create VkPipelineCache for device %p (res=%d)", (void*)device, res);
    }
}

void PipelineCacheManager::on_destroy_device(VkDevice device) {
    if (device == VK_NULL_HANDLE) return;

    std::shared_ptr<DevicePipelineCacheState> state = get_state(device);
    if (!state) return;

    // Final flush while the device and its dispatch table are still valid.
    sync_device_cache_to_disk(device, true);

    // Remove it from the lookup table first: no new pipeline creation may
    // resolve to this cache from now on.
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_devices.find((uint64_t)(uintptr_t)device);
        if (it != m_devices.end() && it->second == state) {
            m_devices.erase(it);
        }
    }

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkDestroyPipelineCache real_destroy_cache = dt.DestroyPipelineCache ? dt.DestroyPipelineCache :
        (PFN_vkDestroyPipelineCache) get_real_proc(get_last_instance(), device, "vkDestroyPipelineCache");

    // Wait for in-flight pipeline creation / flush, then retire the handle. The
    // shared_ptr keeps `state` alive for any flusher that already grabbed it.
    {
        std::unique_lock<std::shared_mutex> useLock(state->cache_use_mutex);
        state->alive.store(false, std::memory_order_release);
        if (real_destroy_cache && state->disk_cache != VK_NULL_HANDLE) {
            real_destroy_cache(device, state->disk_cache, nullptr);
            LOGI("PipelineCacheManager: Destroyed persistent pipeline cache %p for device %p",
                 (void*)state->disk_cache, (void*)device);
            state->disk_cache = VK_NULL_HANDLE;
        }
    }
}

VkPipelineCache PipelineCacheManager::get_disk_cache(VkDevice device) {
    if (!m_enabled.load(std::memory_order_relaxed) || device == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    // Read device ownership and its handle under the same lock. Separate
    // primary-device/cache atomics could publish another device's old cache.
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_devices.find((uint64_t)(uintptr_t)device);
    if (it != m_devices.end() && it->second) {
        return it->second->disk_cache;
    }
    return VK_NULL_HANDLE;
}

std::shared_ptr<DevicePipelineCacheState> PipelineCacheManager::get_state(VkDevice device) {
    if (device == VK_NULL_HANDLE) return nullptr;
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_devices.find((uint64_t)(uintptr_t)device);
    if (it == m_devices.end()) return nullptr;
    return it->second;
}

PipelineCacheUseGuard PipelineCacheManager::lock_cache_for_creation(VkDevice device, VkPipelineCache cache) {
    PipelineCacheUseGuard guard;
    if (!m_enabled.load(std::memory_order_relaxed) || device == VK_NULL_HANDLE || cache == VK_NULL_HANDLE) {
        return guard;
    }

    auto state = get_state(device);
    if (!state) return guard;

    // Take the lock before comparing the handle: on_destroy_device clears
    // disk_cache while holding it exclusively.
    guard.state = std::move(state);
    guard.lock = std::shared_lock<std::shared_mutex>(guard.state->cache_use_mutex);
    if (!guard.state->alive.load(std::memory_order_acquire) || guard.state->disk_cache != cache) {
        // Device is being torn down, or this is an application-provided cache
        // (the application owns its synchronization).
        guard.lock.unlock();
        guard.state.reset();
    }
    return guard;
}

VkPipelineCache PipelineCacheManager::prepare_pipeline_cache(VkDevice device, VkPipelineCache appCache) {
    if (!m_enabled.load(std::memory_order_relaxed)) return appCache;

    VkPipelineCache diskCache = get_disk_cache(device);
    if (diskCache == VK_NULL_HANDLE) return appCache;

    // If application did not specify a pipeline cache, substitute with our persistent disk-backed cache!
    if (appCache == VK_NULL_HANDLE) {
        return diskCache;
    }

    // App provided its own cache: it was pre-warmed upon creation via on_post_create_app_pipeline_cache.
    // Return appCache directly without repeating expensive vkMergePipelineCaches calls on every pipeline creation!
    return appCache;
}

void PipelineCacheManager::on_pipelines_created(VkDevice device, VkPipelineCache usedCache, uint32_t count) {
    if (!m_enabled.load(std::memory_order_relaxed) || count == 0 || device == VK_NULL_HANDLE) return;

    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_devices.find((uint64_t)(uintptr_t)device);
    if (it == m_devices.end() || !it->second) return;

    DevicePipelineCacheState* state = it->second.get();
    VkPipelineCache diskCache = state->disk_cache;
    if (diskCache == VK_NULL_HANDLE || diskCache != usedCache) return;

    mark_dirty(it->second, count);
}

void PipelineCacheManager::mark_dirty(const std::shared_ptr<DevicePipelineCacheState>& state, uint32_t pipelines) {
    // Serialize revision publication with the completion of an older snapshot.
    // Never hold this mutex while accessing a native cache or doing file I/O.
    std::lock_guard<std::mutex> lock(m_flusher_mutex);
    if (!state->alive.load(std::memory_order_acquire)) return;
    state->pending_pipelines_count.fetch_add(pipelines, std::memory_order_relaxed);
    state->revision.fetch_add(1, std::memory_order_release);
    state->is_dirty.store(true, std::memory_order_release);
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_last_pipeline_created_time_ms.store(now_ms, std::memory_order_release);
    m_has_pending_flush.store(true, std::memory_order_release);
    m_flusher_cv.notify_one();
}

bool PipelineCacheManager::sync_device_cache_to_disk(VkDevice device, bool force_sync) {
    if (!m_enabled.load(std::memory_order_relaxed) || device == VK_NULL_HANDLE) return false;

    auto state = get_state(device);
    if (!state) return false;

    std::lock_guard<std::mutex> persist_lock(state->persistence_mutex);
    std::vector<uint8_t> buffer;
    std::string cache_path;
    uint64_t revision = 0;
    {
        // Exclusive: no pipeline creation and no concurrent flush on this cache.
        std::unique_lock<std::shared_mutex> useLock(state->cache_use_mutex);
        if (!state->alive.load(std::memory_order_acquire)) return false;
        if (!force_sync && !state->is_dirty.load(std::memory_order_acquire)) return true;
        revision = state->revision.load(std::memory_order_acquire);

        VkPipelineCache disk_cache = state->disk_cache;
        if (disk_cache == VK_NULL_HANDLE) return false;

        const auto& dt = LayerManager::get().get_dispatch_table(device);
        PFN_vkGetPipelineCacheData real_get_data = dt.GetPipelineCacheData ? dt.GetPipelineCacheData :
            (PFN_vkGetPipelineCacheData) get_real_proc(get_last_instance(), device, "vkGetPipelineCacheData");
        if (!real_get_data) return false;

        size_t dataSize = 0;
        VkResult res = real_get_data(device, disk_cache, &dataSize, nullptr);
        if (res != VK_SUCCESS || dataSize <= sizeof(VkPipelineCacheHeaderVersionOne)) {
            return false;
        }

        buffer.resize(dataSize);
        res = real_get_data(device, disk_cache, &dataSize, buffer.data());
        if (res != VK_SUCCESS || dataSize > buffer.size() ||
            dataSize < sizeof(VkPipelineCacheHeaderVersionOne)) {
            LOGE("PipelineCacheManager: vkGetPipelineCacheData failed with code %d", res);
            return false;
        }
        buffer.resize(dataSize);
        cache_path = state->cache_file_path;
    }

    // File I/O happens without holding the cache lock: pipeline creation must
    // not stall behind a disk write.
    bool success = m_cache_writer(cache_path, buffer.data(), buffer.size());
    if (success) {
        std::lock_guard<std::mutex> lock(m_flusher_mutex);
        if (state->revision.load(std::memory_order_acquire) == revision) {
            state->is_dirty.store(false, std::memory_order_release);
            state->pending_pipelines_count.store(0, std::memory_order_relaxed);
        }
    }

    return success;
}

void PipelineCacheManager::on_post_create_app_pipeline_cache(VkDevice device, VkPipelineCache appCache) {
    if (!m_enabled.load(std::memory_order_relaxed) || appCache == VK_NULL_HANDLE) return;

    auto state = get_state(device);
    if (!state) return;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkMergePipelineCaches real_merge = dt.MergePipelineCaches ? dt.MergePipelineCaches :
        (PFN_vkMergePipelineCaches) get_real_proc(get_last_instance(), device, "vkMergePipelineCaches");

    std::unique_lock<std::shared_mutex> useLock(state->cache_use_mutex);
    if (!state->alive.load(std::memory_order_acquire)) return;

    VkPipelineCache diskCache = state->disk_cache;
    if (diskCache == VK_NULL_HANDLE || diskCache == appCache) return;

    if (real_merge) {
        real_merge(device, appCache, 1, &diskCache);
        LOGI("PipelineCacheManager: Safely merged persistent disk cache into newly created app pipeline cache %p", (void*)appCache);
    }
}

void PipelineCacheManager::on_pre_destroy_app_pipeline_cache(VkDevice device, VkPipelineCache appCache) {
    if (!m_enabled.load(std::memory_order_relaxed) || appCache == VK_NULL_HANDLE) return;

    auto state = get_state(device);
    if (!state) return;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkMergePipelineCaches real_merge = dt.MergePipelineCaches ? dt.MergePipelineCaches :
        (PFN_vkMergePipelineCaches) get_real_proc(get_last_instance(), device, "vkMergePipelineCaches");

    bool merged = false;
    {
        std::unique_lock<std::shared_mutex> useLock(state->cache_use_mutex);
        if (!state->alive.load(std::memory_order_acquire)) return;

        VkPipelineCache diskCache = state->disk_cache;
        if (diskCache == VK_NULL_HANDLE || diskCache == appCache) return;

        if (real_merge) {
            real_merge(device, diskCache, 1, &appCache);
            merged = true;
        }
    }

    if (merged) {
        mark_dirty(state);
    }
}

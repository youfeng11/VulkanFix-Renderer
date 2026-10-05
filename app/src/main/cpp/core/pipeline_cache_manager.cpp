#include "pipeline_cache_manager.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <memory>

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
        m_flusher_cv.wait_for(lock, std::chrono::seconds(4), [this]() {
            return !m_flusher_running.load() || m_has_pending_flush.load();
        });

        if (!m_flusher_running.load()) break;

        if (m_has_pending_flush.load()) {
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t last_time = m_last_pipeline_created_time_ms.load(std::memory_order_relaxed);

            // Debounce: wait until at least 2.5 seconds have elapsed since the last pipeline creation burst
            if (now_ms - last_time >= 2500) {
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

    std::string tmp_path = path + ".tmp";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    if (!f) {
        LOGE("PipelineCacheManager: Failed to open temp file '%s' for writing", tmp_path.c_str());
        return false;
    }

    size_t written = fwrite(data, 1, size, f);
    fflush(f);
    fclose(f);

    if (written != size) {
        LOGE("PipelineCacheManager: Incomplete write to '%s' (%zu / %zu bytes)", tmp_path.c_str(), written, size);
        unlink(tmp_path.c_str());
        return false;
    }

    if (rename(tmp_path.c_str(), path.c_str()) != 0) {
        LOGE("PipelineCacheManager: Atomic rename from '%s' to '%s' failed", tmp_path.c_str(), path.c_str());
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
        auto state = std::make_unique<DevicePipelineCacheState>();
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
            if (m_primary_device.load(std::memory_order_relaxed) == VK_NULL_HANDLE ||
                m_primary_device.load(std::memory_order_relaxed) == device) {
                m_primary_device.store(device, std::memory_order_release);
                m_primary_cache.store(diskCache, std::memory_order_release);
            }
        }

        LOGI("PipelineCacheManager: Successfully initialized persistent pipeline cache (handle=%p) for device %p at '%s'",
             (void*)diskCache, (void*)device, path.c_str());
    } else {
        LOGE("PipelineCacheManager: Failed to create VkPipelineCache for device %p (res=%d)", (void*)device, res);
    }
}

void PipelineCacheManager::on_destroy_device(VkDevice device) {
    if (device == VK_NULL_HANDLE) return;

    VkPipelineCache disk_cache_to_destroy = VK_NULL_HANDLE;

    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_devices.find((uint64_t)(uintptr_t)device);
        if (it != m_devices.end() && it->second) {
            disk_cache_to_destroy = it->second->disk_cache;
        }
    }

    if (disk_cache_to_destroy != VK_NULL_HANDLE) {
        // Sync final cache to storage synchronously before destroying the device
        sync_device_cache_to_disk(device, true);

        const auto& dt = LayerManager::get().get_dispatch_table(device);
        PFN_vkDestroyPipelineCache real_destroy_cache = dt.DestroyPipelineCache ? dt.DestroyPipelineCache :
            (PFN_vkDestroyPipelineCache) get_real_proc(get_last_instance(), device, "vkDestroyPipelineCache");
        if (real_destroy_cache) {
            real_destroy_cache(device, disk_cache_to_destroy, nullptr);
            LOGI("PipelineCacheManager: Destroyed persistent pipeline cache %p for device %p",
                 (void*)disk_cache_to_destroy, (void*)device);
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_devices.erase((uint64_t)(uintptr_t)device);
        if (m_primary_device.load(std::memory_order_relaxed) == device) {
            if (!m_devices.empty()) {
                auto first = m_devices.begin();
                m_primary_device.store((VkDevice)(uintptr_t)first->first, std::memory_order_release);
                m_primary_cache.store(first->second->disk_cache, std::memory_order_release);
            } else {
                m_primary_device.store(VK_NULL_HANDLE, std::memory_order_release);
                m_primary_cache.store(VK_NULL_HANDLE, std::memory_order_release);
            }
        }
    }
}

VkPipelineCache PipelineCacheManager::get_disk_cache(VkDevice device) {
    if (!m_enabled.load(std::memory_order_relaxed) || device == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    if (m_primary_device.load(std::memory_order_relaxed) == device) {
        return m_primary_cache.load(std::memory_order_relaxed);
    }

    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_devices.find((uint64_t)(uintptr_t)device);
    if (it != m_devices.end() && it->second) {
        return it->second->disk_cache;
    }
    return VK_NULL_HANDLE;
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
    if (diskCache == VK_NULL_HANDLE) return;

    // If pipelines were created using an app cache, merge the newly created pipelines back into diskCache safely under lock
    if (usedCache != VK_NULL_HANDLE && usedCache != diskCache) {
        const auto& dt = LayerManager::get().get_dispatch_table(device);
        PFN_vkMergePipelineCaches real_merge = dt.MergePipelineCaches ? dt.MergePipelineCaches :
            (PFN_vkMergePipelineCaches) get_real_proc(get_last_instance(), device, "vkMergePipelineCaches");
        if (real_merge) {
            std::lock_guard<std::mutex> api_lock(state->cache_api_mutex);
            real_merge(device, diskCache, 1, &usedCache);
        }
    }

    state->pending_pipelines_count.fetch_add(count, std::memory_order_relaxed);
    state->is_dirty.store(true, std::memory_order_release);

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_last_pipeline_created_time_ms.store(now_ms, std::memory_order_release);
    m_has_pending_flush.store(true, std::memory_order_release);
    m_flusher_cv.notify_one();
}

bool PipelineCacheManager::sync_device_cache_to_disk(VkDevice device, bool force_sync) {
    if (!m_enabled.load(std::memory_order_relaxed) || device == VK_NULL_HANDLE) return false;

    DevicePipelineCacheState* state_ptr = nullptr;
    std::string cache_path;
    bool is_dirty = false;

    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_devices.find((uint64_t)(uintptr_t)device);
        if (it == m_devices.end() || !it->second) return false;
        state_ptr = it->second.get();
        cache_path = state_ptr->cache_file_path;
        is_dirty = state_ptr->is_dirty.load(std::memory_order_acquire);
    }

    if (!force_sync && !is_dirty) {
        return true;
    }

    VkPipelineCache disk_cache = state_ptr->disk_cache;
    if (disk_cache == VK_NULL_HANDLE) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkGetPipelineCacheData real_get_data = dt.GetPipelineCacheData ? dt.GetPipelineCacheData :
        (PFN_vkGetPipelineCacheData) get_real_proc(get_last_instance(), device, "vkGetPipelineCacheData");
    if (!real_get_data) return false;

    std::vector<uint8_t> buffer;
    size_t dataSize = 0;
    {
        // Host Synchronization requirement: protect vkGetPipelineCacheData against concurrent vkMergePipelineCaches
        std::lock_guard<std::mutex> api_lock(state_ptr->cache_api_mutex);
        VkResult res = real_get_data(device, disk_cache, &dataSize, nullptr);
        if (res != VK_SUCCESS || dataSize <= sizeof(VkPipelineCacheHeaderVersionOne)) {
            return false;
        }

        buffer.resize(dataSize);
        res = real_get_data(device, disk_cache, &dataSize, buffer.data());
        if (res != VK_SUCCESS) {
            LOGE("PipelineCacheManager: vkGetPipelineCacheData failed with code %d", res);
            return false;
        }
    }

    bool success = write_cache_data_to_file(cache_path, buffer.data(), dataSize);
    if (success) {
        state_ptr->is_dirty.store(false, std::memory_order_release);
        state_ptr->pending_pipelines_count.store(0, std::memory_order_relaxed);
    }

    return success;
}

void PipelineCacheManager::on_post_create_app_pipeline_cache(VkDevice device, VkPipelineCache appCache) {
    if (!m_enabled.load(std::memory_order_relaxed) || appCache == VK_NULL_HANDLE) return;

    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_devices.find((uint64_t)(uintptr_t)device);
    if (it == m_devices.end() || !it->second) return;

    DevicePipelineCacheState* state = it->second.get();
    VkPipelineCache diskCache = state->disk_cache;
    if (diskCache == VK_NULL_HANDLE || diskCache == appCache) return;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkMergePipelineCaches real_merge = dt.MergePipelineCaches ? dt.MergePipelineCaches :
        (PFN_vkMergePipelineCaches) get_real_proc(get_last_instance(), device, "vkMergePipelineCaches");
    if (real_merge) {
        std::lock_guard<std::mutex> api_lock(state->cache_api_mutex);
        real_merge(device, appCache, 1, &diskCache);
        LOGI("PipelineCacheManager: Safely merged persistent disk cache into newly created app pipeline cache %p", (void*)appCache);
    }
}

void PipelineCacheManager::on_pre_destroy_app_pipeline_cache(VkDevice device, VkPipelineCache appCache) {
    if (!m_enabled.load(std::memory_order_relaxed) || appCache == VK_NULL_HANDLE) return;

    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_devices.find((uint64_t)(uintptr_t)device);
    if (it == m_devices.end() || !it->second) return;

    DevicePipelineCacheState* state = it->second.get();
    VkPipelineCache diskCache = state->disk_cache;
    if (diskCache == VK_NULL_HANDLE || diskCache == appCache) return;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkMergePipelineCaches real_merge = dt.MergePipelineCaches ? dt.MergePipelineCaches :
        (PFN_vkMergePipelineCaches) get_real_proc(get_last_instance(), device, "vkMergePipelineCaches");
    if (real_merge) {
        {
            std::lock_guard<std::mutex> api_lock(state->cache_api_mutex);
            real_merge(device, diskCache, 1, &appCache);
        }
        state->is_dirty.store(true, std::memory_order_release);
        m_has_pending_flush.store(true, std::memory_order_release);
        m_flusher_cv.notify_one();
    }
}

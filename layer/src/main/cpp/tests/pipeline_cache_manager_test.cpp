#include "core/pipeline_cache_manager.h"
#include "core/layer_manager.h"
#include "core/driver_loader.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <dirent.h>
#include <sys/resource.h>
#include <new>

namespace {
unsigned failures;
std::atomic<unsigned> reads{0};
std::atomic<bool> shrink_data{false};
std::atomic<unsigned> destroyed{0};
std::atomic<unsigned> created{0};
const auto device = reinterpret_cast<VkDevice>(uintptr_t(0xcace));
constexpr size_t cache_bytes = sizeof(VkPipelineCacheHeaderVersionOne) + 64;
void check(bool ok, const char* name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
VKAPI_ATTR VkResult VKAPI_CALL get_data(VkDevice, VkPipelineCache, size_t* size, void* out) {
    reads.fetch_add(1);
    if (!out) { *size = cache_bytes; return VK_SUCCESS; }
    const size_t actual = shrink_data ? cache_bytes - 16 : cache_bytes;
    if (*size < actual) return VK_INCOMPLETE;
    std::memset(out, 0, actual);
    VkPipelineCacheHeaderVersionOne header{};
    header.headerSize = sizeof(header);
    header.headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
    std::memcpy(out, &header, sizeof(header));
    *size = actual;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_cache(VkDevice, VkPipelineCache, const VkAllocationCallbacks*) {
    destroyed.fetch_add(1);
}
VKAPI_ATTR VkResult VKAPI_CALL create_cache(VkDevice d, const VkPipelineCacheCreateInfo*,
                                           const VkAllocationCallbacks*, VkPipelineCache* out) {
    *out = (VkPipelineCache)((uint64_t(uintptr_t(d)) << 16) | (created.fetch_add(1) + 1));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    *out = {};
    out->apiVersion = VK_API_VERSION_1_0;
}
struct WriteGate {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned calls = 0;
    bool released = false;
    bool write() {
        std::unique_lock<std::mutex> lock(mutex);
        if (++calls == 1) {
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
        }
        return true;
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        check(cv.wait_for(lock, std::chrono::seconds(2), [&] { return calls != 0; }),
              "snapshot reaches blocked disk writer");
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};
void cleanup(const std::string& path) {
    unlink(path.c_str());
    unlink((path + ".tmp").c_str());
}
} // namespace

// Keep the real cache manager with a mock driver. Avoid filling the connected
// device's logcat ring with the churn test's thousands of lifecycle messages.
extern "C" int __android_log_vprint(int, const char*, const char*, va_list) { return 0; }
LayerManager& LayerManager::get() { static LayerManager manager; return manager; }
std::shared_ptr<const DeviceDispatchTable> LayerManager::get_dispatch_table_slow(VkDevice) {
    static auto table = [] {
        auto value = std::make_shared<DeviceDispatchTable>();
        value->GetPipelineCacheData = get_data;
        value->DestroyPipelineCache = destroy_cache;
        value->CreatePipelineCache = create_cache;
        return value;
    }();
    return table;
}
extern "C" VkInstance get_last_instance() { return VK_NULL_HANDLE; }
extern "C" void* get_real_proc(VkInstance, VkDevice, const char* name) {
    return strcmp(name, "vkGetPhysicalDeviceProperties") == 0 ? (void*)properties : nullptr;
}

class PipelineCacheManagerTest {
    static std::shared_ptr<DevicePipelineCacheState> prepare(PipelineCacheManager& manager,
                                                            const std::string& path) {
        manager.m_enabled.store(true);
        auto state = std::make_shared<DevicePipelineCacheState>();
        state->device = device;
        state->disk_cache = VkPipelineCache(uintptr_t(0xbeef));
        state->cache_file_path = path;
        manager.m_devices.emplace(uint64_t(uintptr_t(device)), state);
        reads.store(0); shrink_data.store(false);
        manager.on_pipelines_created(device, state->disk_cache, 1);
        return state;
    }
public:
    static void run(const std::string& root) {
        {
            PipelineCacheManager manager;
            const auto path = root + "/debounce";
            prepare(manager, path);
            manager.start_flusher_thread();
            clockid_t cpu_clock{};
            const bool have_clock = pthread_getcpuclockid(manager.m_flusher_thread.native_handle(), &cpu_clock) == 0;
            check(have_clock, "read cache worker CPU clock");
            timespec before{}, after{};
            clock_gettime(cpu_clock, &before);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            clock_gettime(cpu_clock, &after);
            const auto cpu_ns = (after.tv_sec - before.tv_sec) * 1000000000LL + after.tv_nsec - before.tv_nsec;
            printf("Debounce worker CPU over 300 ms: %.3f ms\n", cpu_ns / 1000000.0);
            check(have_clock && cpu_ns < 50000000, "pending debounce sleeps instead of busy-spinning");
            check(reads.load() == 0, "debounce avoids premature native readback");
            const auto start = std::chrono::steady_clock::now();
            manager.stop_flusher_thread();
            check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "shutdown interrupts debounce promptly");
            check(reads.load() == 2, "shutdown flushes pending cache exactly once");
            cleanup(path);
        }
        {
            PipelineCacheManager manager;
            const auto path = root + "/app-owned";
            auto state = prepare(manager, path);
            state->is_dirty.store(false);
            manager.m_has_pending_flush.store(false);
            manager.on_pipelines_created(device, VkPipelineCache(uintptr_t(0x1234)), 1);
            check(!state->is_dirty.load() && !manager.m_has_pending_flush.load(),
                  "app-owned cache compilation does not schedule unchanged disk-cache snapshots");
        }
        {
            PipelineCacheManager manager;
            const auto path = root + "/quiet-period";
            auto state = prepare(manager, path);
            manager.start_flusher_thread();
            for (unsigned i = 0; i < 3; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                manager.on_pipelines_created(device, state->disk_cache, 1);
            }
            check(reads.load() == 0, "pipeline burst postpones native readback");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (state->is_dirty.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            check(!state->is_dirty.load() && reads.load() == 2, "quiet period coalesces the burst into one snapshot");
            manager.stop_flusher_thread();
            cleanup(path);
        }
        {
            WriteGate gate;
            PipelineCacheManager manager;
            const auto path = root + "/generation";
            auto state = prepare(manager, path);
            manager.m_cache_writer = [&](const std::string&, const void*, size_t) { return gate.write(); };
            bool saved = false;
            std::thread writer([&] { saved = manager.sync_device_cache_to_disk(device); });
            gate.wait();
            const bool unlocked = state->cache_use_mutex.try_lock();
            check(unlocked, "compilation cache lock is released during file I/O");
            if (unlocked) state->cache_use_mutex.unlock();
            manager.on_pipelines_created(device, state->disk_cache, 1);
            gate.release();
            writer.join();
            check(saved && state->is_dirty.load(), "pipeline created during disk I/O remains dirty");
            check(manager.sync_device_cache_to_disk(device) && !state->is_dirty.load(), "next snapshot persists the new generation");
            cleanup(path);
        }
        {
            WriteGate gate;
            PipelineCacheManager manager;
            const auto path = root + "/serialization";
            auto state = prepare(manager, path);
            manager.m_cache_writer = [&](const std::string&, const void*, size_t) { return gate.write(); };
            bool first = false, second = false;
            std::thread a([&] { first = manager.sync_device_cache_to_disk(device, true); });
            gate.wait();
            std::thread b([&] { second = manager.sync_device_cache_to_disk(device, true); });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(reads.load() == 2, "same-device flushes serialize snapshot and temporary-file ownership");
            gate.release();
            a.join(); b.join();
            check(first && second, "both serialized cache saves complete");
            cleanup(path);
        }
        {
            PipelineCacheManager manager;
            const auto path = root + "/length";
            prepare(manager, path);
            shrink_data.store(true);
            check(manager.sync_device_cache_to_disk(device), "save cache whose native size shrinks");
            struct stat st{};
            check(stat(path.c_str(), &st) == 0 && size_t(st.st_size) == cache_bytes - 16,
                  "cache file contains only bytes returned by the driver");
            cleanup(path);
        }
        {
            PipelineCacheManager manager;
            const auto path = root + "/disk-error";
            auto state = prepare(manager, path);
            rlimit previous{};
            const bool have_limit = getrlimit(RLIMIT_FSIZE, &previous) == 0;
            auto limited = previous; limited.rlim_cur = 1;
            auto handler = std::signal(SIGXFSZ, SIG_IGN);
            const bool limited_write = have_limit && setrlimit(RLIMIT_FSIZE, &limited) == 0;
            check(limited_write, "prepare buffered disk-write failure");
            check(!manager.sync_device_cache_to_disk(device) && state->is_dirty.load(),
                  "flush errors preserve pending cache data");
            if (have_limit) setrlimit(RLIMIT_FSIZE, &previous);
            if (handler != SIG_ERR) std::signal(SIGXFSZ, handler);
            struct stat st{};
            check(lstat(path.c_str(), &st) != 0, "failed flush never replaces the cache file");
            cleanup(path);
        }
        {
            PipelineCacheManager manager;
            const auto path = root + "/atomic";
            std::vector<uint8_t> a(65536, 0xaa), b(65536, 0xbb);
            std::atomic<bool> ok{true};
            auto save = [&](const std::vector<uint8_t>& bytes) {
                for (unsigned i = 0; i < 32; ++i)
                    if (!manager.write_cache_data_to_file(path, bytes.data(), bytes.size())) ok.store(false);
            };
            std::thread first([&] { save(a); }), second([&] { save(b); });
            first.join(); second.join();
            std::ifstream file(path, std::ios::binary);
            std::vector<uint8_t> actual((std::istreambuf_iterator<char>(file)), {});
            check(ok.load() && (actual == a || actual == b), "64 concurrent same-path writes remain complete and atomic");
            cleanup(path);
            const auto directory = root + "/rename-target";
            check(mkdir(directory.c_str(), 0700) == 0, "prepare failed atomic rename");
            check(!manager.write_cache_data_to_file(directory, a.data(), a.size()), "failed rename is reported");
            rmdir(directory.c_str());
            unsigned temporaries = 0;
            DIR* dir = opendir(root.c_str());
            const bool opened = dir != nullptr;
            if (dir) {
                while (auto* entry = readdir(dir))
                    if (strstr(entry->d_name, ".tmp.")) ++temporaries;
                closedir(dir);
            }
            check(opened && temporaries == 0, "successful and failed writes retire every temporary file");
        }
        {
            const auto path = root + "/lifetime";
            destroyed.store(0);
            bool released = true;
            for (unsigned i = 0; i < 256; ++i) {
                std::weak_ptr<DevicePipelineCacheState> retired;
                {
                    PipelineCacheManager manager;
                    auto state = prepare(manager, path);
                    retired = state;
                    manager.on_destroy_device(device);
                    released &= !state->alive.load() && !manager.get_state(device);
                }
                released &= retired.expired();
            }
            check(released, "256 device retirements release every cache-state owner");
            check(destroyed.load() == 256, "each retired native cache is destroyed exactly once");
            cleanup(path);
        }
        {
            PipelineCacheManager manager;
            const auto path = root + "/header";
            VkPipelineCacheHeaderVersionOne header{};
            header.headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
            header.headerSize = sizeof(header) + 1024;
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(&header), sizeof(header)); file.close();
            std::vector<uint8_t> data;
            uint8_t uuid[VK_UUID_SIZE]{};
            check(!manager.load_initial_cache_data(path, 0, 0, uuid, data), "reject cache header extending beyond the file");
            cleanup(path);
        }
        {
            // Exercise every naturally aligned placement within a cache line,
            // including publications whose fields fall on different lines.
            alignas(64) unsigned char storage[sizeof(PipelineCacheManager) + 64];
            for (size_t alignment = 0; alignment < 64; alignment += alignof(PipelineCacheManager)) {
                auto& manager = *new (storage + alignment) PipelineCacheManager;
                manager.m_enabled.store(true);
                manager.m_cache_writer = [](const std::string&, const void*, size_t) { return true; };
                const auto path = root + "/devices";
                setenv("VULKAN_FIX_PIPELINE_CACHE_PATH", path.c_str(), 1);
                const auto physical = reinterpret_cast<VkPhysicalDevice>(uintptr_t(0x123));
                const VkDevice devices[]{device, reinterpret_cast<VkDevice>(uintptr_t(0xfacade))};
                const auto creates_before = created.load(), destroys_before = destroyed.load();
                for (auto d : devices) manager.init_for_device(physical, d);
                check(manager.get_disk_cache(devices[0]) != VK_NULL_HANDLE &&
                          manager.get_disk_cache(devices[1]) != VK_NULL_HANDLE,
                      "initialize two independent persistent native caches");
                constexpr unsigned rounds = 2048;
                std::mutex mutex;
                std::condition_variable cv;
                unsigned phase = 0;
                bool ready = false, finished = false;
                VkDevice target = VK_NULL_HANDLE;
                VkPipelineCache expected = VK_NULL_HANDLE;
                unsigned errors = 0;
                std::thread reader([&] {
                    for (unsigned i = 0; i < rounds; ++i) {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [&] { return phase == i + 1; });
                        const auto d = target;
                        const auto cache = expected;
                        ready = true; cv.notify_all();
                        lock.unlock();
                        for (unsigned query = 0; query < 2048; ++query)
                            errors += manager.get_disk_cache(d) != cache;
                        lock.lock(); finished = true; cv.notify_all();
                    }
                });
                for (unsigned i = 0; i < rounds; ++i) {
                    const auto retiring = devices[i % 2], surviving = devices[(i + 1) % 2];
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        target = surviving; expected = manager.get_disk_cache(surviving);
                        ready = finished = false; phase = i + 1; cv.notify_all();
                        cv.wait(lock, [&] { return ready; });
                    }
                    manager.on_destroy_device(retiring);
                    manager.init_for_device(physical, retiring);
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [&] { return finished; });
                    }
                }
                reader.join();
                printf("Concurrent pipeline cache misroutes: %u\n", errors);
                check(errors == 0, "2048 other-device retirements never expose the retiring device's cache");
                for (auto d : devices) manager.on_destroy_device(d);
                check(created.load() - creates_before == rounds + 2 &&
                          destroyed.load() - destroys_before == rounds + 2,
                      "multi-device churn destroys every native cache exactly once");
                unsetenv("VULKAN_FIX_PIPELINE_CACHE_PATH");
                cleanup(path);
                manager.~PipelineCacheManager();
            }
        }
    }
};

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    setenv("VULKAN_FIX_LOG_PATH", "/dev/null", 1);
    char directory[] = "/data/local/tmp/vulkanfix-cache-test-XXXXXX";
    if (!mkdtemp(directory)) return 2;
    PipelineCacheManagerTest::run(directory);
    rmdir(directory);
    printf("Pipeline cache regressions: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}

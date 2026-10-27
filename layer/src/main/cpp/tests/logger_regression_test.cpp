#include "core/vk_common.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <dirent.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

FILE* logger_file_from_peer();
void logger_write_from_peer(unsigned writer, unsigned line);

namespace {
std::string log_path;
std::mutex open_mutex;
std::condition_variable open_cv;
bool opening = false, release_open = false;
unsigned opens = 0, failures = 0;
void check(bool success, const char* description) {
    printf("%s: %s\n", success ? "PASS" : "FAIL", description);
    failures += !success;
}
unsigned log_descriptor_count() {
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return ~0u;
    unsigned count = 0;
    while (auto* entry = readdir(directory)) {
        const std::string path = std::string("/proc/self/fd/") + entry->d_name;
        char target[2048];
        const auto size = readlink(path.c_str(), target, sizeof(target));
        if (size >= 0 && std::string(target, size_t(size)) == log_path) ++count;
    }
    closedir(directory);
    return count;
}
}

// Pause the actual first open, so readers overlap initialization deterministically.
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    if (log_path == path) {
        std::unique_lock<std::mutex> lock(open_mutex);
        ++opens;
        opening = true;
        open_cv.notify_all();
        open_cv.wait(lock, [] { return release_open; });
    }
    return __real_fopen(path, mode);
}
// The test verifies the file sink without filling the device's logcat ring.
extern "C" int __android_log_vprint(int, const char*, const char*, va_list) { return 0; }

int main() {
    char path[] = "/data/local/tmp/vulkanfix-logger-XXXXXX";
    const int temporary = mkstemp(path);
    if (temporary < 0) return 2;
    close(temporary);
    unlink(path);
    log_path = path;
    setenv("VULKAN_FIX_LOG_PATH", path, 1);
    setenv("VULKAN_FIX_DEBUG", "true", 1);

    FILE* first = nullptr;
    std::thread initializer([&] { first = get_writable_log_file(); });
    {
        std::unique_lock<std::mutex> lock(open_mutex);
        if (!open_cv.wait_for(lock, std::chrono::seconds(5), [] { return opening; })) {
            // A broken setup must terminate rather than leave a joinable thread.
            fprintf(stderr, "FAIL: first log open did not reach the test gate\n");
            std::abort();
        }
    }
    constexpr unsigned readers = 16;
    std::atomic<unsigned> started{0}, finished{0};
    FILE* files[readers]{};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < readers; ++i) {
        threads.emplace_back([&, i] {
            started.fetch_add(1);
            files[i] = i % 2 ? logger_file_from_peer() : get_writable_log_file();
            finished.fetch_add(1);
        });
    }
    while (started.load() != readers) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(finished.load() == 0, "concurrent readers wait for the initial file open");
    {
        std::lock_guard<std::mutex> lock(open_mutex);
        release_open = true;
    }
    open_cv.notify_all();
    initializer.join();
    for (auto& thread : threads) thread.join();
    bool shared = first != nullptr;
    for (auto* file : files) shared &= file == first;
    check(shared, "all threads and translation units receive the same non-null file");
    check(opens == 1 && log_descriptor_count() == 1,
          "logger opens exactly one process-lifetime file descriptor");

    constexpr unsigned writers = 8, lines = 64;
    std::atomic<unsigned> debug_enabled{0};
    threads.clear();
    for (unsigned i = 0; i < writers; ++i) {
        threads.emplace_back([&, i] {
            debug_enabled.fetch_add(is_debug_logging());
            for (unsigned line = 0; line < lines; ++line) {
                if (i % 2) logger_write_from_peer(i, line);
                else LOGI("logger line %u:%u", i, line);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    check(debug_enabled.load() == writers, "concurrent debug option queries preserve the setting");
    FILE* readback = __real_fopen(path, "r");
    unsigned seen[writers][lines]{};
    unsigned count = 0;
    bool valid = readback != nullptr;
    char buffer[256];
    while (readback && fgets(buffer, sizeof(buffer), readback)) {
        unsigned writer, line;
        if (sscanf(buffer, "[INFO] logger line %u:%u", &writer, &line) != 2 ||
            writer >= writers || line >= lines) valid = false;
        else ++seen[writer][line];
        ++count;
    }
    if (readback) fclose(readback);
    for (const auto& writer : seen) for (auto line : writer) valid &= line == 1;
    check(valid && count == writers * lines,
          "512 concurrent messages survive without truncation or interleaving");
    check(logger_file_from_peer() == first && opens == 1 && log_descriptor_count() == 1,
          "repeated logging retains one shared file without opening extra descriptors");
    unlink(path);
    printf("Logger regression: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}

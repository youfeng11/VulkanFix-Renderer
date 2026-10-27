#include "core/extension_module_base.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <memory>
#include <new>

namespace {
class Probe : public ExtensionModuleBase {
public:
    Probe() : ExtensionModuleBase("test") {}
    bool is_phys_device_native(VkPhysicalDevice p) override { return uintptr_t(p) == 1; }
};
unsigned failures;
void check(bool ok, const char* message) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
}
extern "C" VkInstance get_last_instance() { return VK_NULL_HANDLE; }
extern "C" void* get_real_proc(VkInstance, VkDevice, const char*) { return nullptr; }

int main() {
    Probe probe;
    const auto a = reinterpret_cast<VkDevice>(uintptr_t(0x123));
    const auto b = reinterpret_cast<VkDevice>(uintptr_t(0x456));
    probe.on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t(1)), a, VK_SUCCESS, nullptr);
    probe.on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t(2)), b, VK_SUCCESS, nullptr);
    std::atomic<bool> start{false};
    std::atomic<unsigned> errors{0};
    std::array<std::thread, 8> threads;
    for (unsigned i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            unsigned local = 0;
            for (unsigned n = 0; n < 200000; ++n) {
                const bool native = ((n + i) & 1) == 0;
                local += probe.is_device_native(native ? a : b) != native;
            }
            errors.fetch_add(local);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    printf("Concurrent capability misroutes: %u\n", errors.load());
    check(errors.load() == 0, "concurrent device queries never mix another device's native flag");
    {
        const auto other = reinterpret_cast<VkDevice>(uintptr_t(0x999));
        std::atomic<bool> done{false};
        errors.store(0);
        std::thread reader([&] {
            while (!done.load(std::memory_order_acquire)) {
                if (!probe.is_device_native(a)) errors.fetch_add(1);
                if (probe.is_device_native(b)) errors.fetch_add(1);
            }
        });
        for (unsigned n = 0; n < 20000; ++n) {
            probe.on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t((n & 1) + 1)),
                                          other, VK_SUCCESS, nullptr);
            probe.on_destroy_device(other);
        }
        done.store(true, std::memory_order_release);
        reader.join();
        check(errors.load() == 0, "concurrent third-device publication never tears the cached pair");
    }
    probe.on_destroy_device(a);
    check(!probe.is_device_native(a) && !probe.is_device_native(b), "device retirement invalidates cached capability");
    probe.on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t(2)), a, VK_SUCCESS, nullptr);
    check(!probe.is_device_native(a), "reused device handle cannot retrieve its old native flag");
    probe.on_destroy_device(a);
    probe.on_destroy_device(b);
    {
        std::atomic<unsigned> phase{0};
        bool before = false, after = true;
        probe.on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t(1)), a, VK_SUCCESS, nullptr);
        std::thread reader([&] {
            before = probe.is_device_native(a);
            phase.store(1, std::memory_order_release);
            while (phase.load(std::memory_order_acquire) != 2) std::this_thread::yield();
            after = probe.is_device_native(a);
        });
        while (phase.load(std::memory_order_acquire) != 1) std::this_thread::yield();
        probe.on_destroy_device(a);
        probe.on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t(2)), a, VK_SUCCESS, nullptr);
        phase.store(2, std::memory_order_release);
        reader.join();
        check(before && !after, "device reincarnation invalidates another thread's warm entry");
        probe.on_destroy_device(a);
    }
    {
        alignas(Probe) unsigned char storage[sizeof(Probe)];
        auto* old = new (storage) Probe;
        old->on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t(1)), a, VK_SUCCESS, nullptr);
        bool before = old->is_device_native(a);
        old->~Probe();
        auto* next = new (storage) Probe;
        next->on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t(2)), a, VK_SUCCESS, nullptr);
        check(before && !next->is_device_native(a), "reused module address cannot retrieve a previous instance's entry");
        next->~Probe();
    }
    {
        std::array<std::unique_ptr<Probe>, 256> probes;
        for (unsigned i = 0; i < probes.size(); ++i) {
            probes[i] = std::make_unique<Probe>();
            probes[i]->on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t((i & 1) + 1)),
                                              a, VK_SUCCESS, nullptr);
        }
        bool correct = true;
        for (unsigned n = 0; n < 3; ++n)
            for (unsigned i = 0; i < probes.size(); ++i)
                correct &= probes[i]->is_device_native(a) == ((i & 1) == 0);
        check(correct, "independent module caches preserve each device map's capability");
    }
    timespec begin{}, end{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &begin);
    unsigned native = 0;
    probe.on_post_create_device(reinterpret_cast<VkPhysicalDevice>(uintptr_t(1)), a, VK_SUCCESS, nullptr);
    for (unsigned n = 0; n < 1000000; ++n) native += probe.is_device_native(a);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);
    const auto elapsed = (end.tv_sec - begin.tv_sec) * 1000000LL + (end.tv_nsec - begin.tv_nsec) / 1000;
    printf("Single-device cached query CPU: %lld us / 1000000\n", elapsed);
    check(native == 1000000, "hot capability lookup preserves the native result");
    printf("Device capability regressions: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}

#include "driver_loader.h"
#include <dlfcn.h>
#include <mutex>
#include <atomic>

static std::atomic<void*> g_real_vulkan_handle{nullptr};
static std::atomic<PFN_vkGetInstanceProcAddr> g_real_vkGetInstanceProcAddr{nullptr};
static std::atomic<PFN_vkGetDeviceProcAddr> g_real_vkGetDeviceProcAddr{nullptr};
static void* g_self_handle = NULL;
static std::atomic<VkInstance> g_last_instance{VK_NULL_HANDLE};
static std::mutex g_driver_mutex;

void init_real_vulkan() {
    if (g_real_vulkan_handle.load(std::memory_order_acquire) != nullptr) return;

    std::lock_guard<std::mutex> lock(g_driver_mutex);
    if (g_real_vulkan_handle.load(std::memory_order_relaxed) != nullptr) return;

    void* handle = nullptr;

    // 1. Check custom path from environment
    const char* custom_path = getenv("POJAV_REAL_VULKAN_PATH");
    if (!custom_path) custom_path = getenv("DRIVER_PATH");
    if (custom_path && custom_path[0] != '\0') {
        handle = dlopen(custom_path, RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            LOGI("Loaded real Vulkan from custom path: %s", custom_path);
        }
    }

    // 2. Try standard libvulkan.so
    if (!handle) {
        handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            LOGI("Loaded real Vulkan from libvulkan.so");
        }
    }

    // 3. Fallback to system / vendor paths
    if (!handle) {
#if defined(__aarch64__) || defined(__x86_64__)
        const char* paths[] = {
            "/system/lib64/libvulkan.so",
            "/vendor/lib64/hw/vulkan.mali.so",
            "/vendor/lib64/hw/vulkan.adreno.so",
            "/vendor/lib64/hw/vulkan.powerVR.so",
            "/vendor/lib64/libvulkan.so",
            "/system_ext/lib64/libvulkan.so"
        };
#else
        const char* paths[] = {
            "/system/lib/libvulkan.so",
            "/vendor/lib/hw/vulkan.mali.so",
            "/vendor/lib/hw/vulkan.adreno.so",
            "/vendor/lib/hw/vulkan.powerVR.so",
            "/vendor/lib/libvulkan.so",
            "/system_ext/lib/libvulkan.so"
        };
#endif
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            handle = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
            if (handle) {
                LOGI("Loaded real Vulkan from system path: %s", paths[i]);
                break;
            }
        }
    }

    if (!handle) {
        LOGE("CRITICAL: Failed to load underlying real Vulkan driver!");
        return;
    }

    PFN_vkGetInstanceProcAddr inst_proc = (PFN_vkGetInstanceProcAddr) dlsym(handle, "vkGetInstanceProcAddr");
    if (!inst_proc) {
        LOGE("CRITICAL: vkGetInstanceProcAddr not found in real Vulkan driver!");
    } else {
        LOGI("Successfully loaded real vkGetInstanceProcAddr");
    }

    PFN_vkGetDeviceProcAddr dev_proc = (PFN_vkGetDeviceProcAddr) dlsym(handle, "vkGetDeviceProcAddr");
    if (!dev_proc && inst_proc) {
        dev_proc = (PFN_vkGetDeviceProcAddr) inst_proc(VK_NULL_HANDLE, "vkGetDeviceProcAddr");
    }
    if (dev_proc) {
        LOGI("Successfully loaded real vkGetDeviceProcAddr");
    }

    g_real_vkGetInstanceProcAddr.store(inst_proc, std::memory_order_release);
    g_real_vkGetDeviceProcAddr.store(dev_proc, std::memory_order_release);
    g_real_vulkan_handle.store(handle, std::memory_order_release);
}

void register_vulkan_ptr() {
    Dl_info info;
    if (dladdr((void*)&register_vulkan_ptr, &info) && info.dli_fname) {
        g_self_handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_self_handle) {
        g_self_handle = dlopen("libvulkan_fix.so", RTLD_NOW | RTLD_GLOBAL);
    }

    if (g_self_handle) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%" PRIxPTR, (uintptr_t)g_self_handle);
        setenv("VULKAN_PTR", buf, 1);
        setenv("POJAV_VULKAN_PTR", buf, 1);
        LOGI("Registered VULKAN_PTR = %s (self handle = %p)", buf, g_self_handle);
    } else {
        LOGE("Failed to obtain self handle for VULKAN_PTR registration!");
    }
}

void* get_real_proc(VkInstance instance, VkDevice device, const char* name) {
    init_real_vulkan();
    void* ptr = NULL;
    VkInstance last_inst = g_last_instance.load(std::memory_order_relaxed);
    PFN_vkGetDeviceProcAddr dev_proc = g_real_vkGetDeviceProcAddr.load(std::memory_order_relaxed);
    PFN_vkGetInstanceProcAddr inst_proc = g_real_vkGetInstanceProcAddr.load(std::memory_order_relaxed);
    void* handle = g_real_vulkan_handle.load(std::memory_order_relaxed);

    // 1. If device is valid, try real vkGetDeviceProcAddr
    if (device != VK_NULL_HANDLE) {
        if (!dev_proc && inst_proc) {
            dev_proc = (PFN_vkGetDeviceProcAddr) inst_proc(instance ? instance : last_inst, "vkGetDeviceProcAddr");
            if (dev_proc) {
                g_real_vkGetDeviceProcAddr.store(dev_proc, std::memory_order_relaxed);
            }
        }
        if (dev_proc) {
            ptr = (void*) dev_proc(device, name);
            if (ptr) return ptr;
        }
        // Special case: standard Vulkan 1.0 queue functions might be queried from instance on some Vulkan 1.0 loaders
        if (name && (strcmp(name, "vkQueueSubmit") == 0 ||
                     strcmp(name, "vkQueueWaitIdle") == 0 ||
                     strcmp(name, "vkQueueBindSparse") == 0)) {
            VkInstance inst = (instance != VK_NULL_HANDLE) ? instance : last_inst;
            if (inst != VK_NULL_HANDLE && inst_proc) {
                ptr = (void*) inst_proc(inst, name);
                if (ptr) return ptr;
            }
        }
        // Device-level function not supported by real driver! DO NOT fall through to dlsym,
        // as Android libvulkan loader exports generic trampolines that branch to NULL and crash!
        return NULL;
    }

    // 2. If instance is valid (or g_last_instance is available), try vkGetInstanceProcAddr
    VkInstance inst = (instance != VK_NULL_HANDLE) ? instance : last_inst;
    if (inst != VK_NULL_HANDLE && inst_proc) {
        ptr = (void*) inst_proc(inst, name);
        if (ptr) return ptr;
    }

    // 3. Try dlsym directly on real Vulkan library handle (only for global functions like vkCreateInstance)
    if (handle) {
        ptr = dlsym(handle, name);
        if (ptr) return ptr;
    }

    // 4. Try vkGetInstanceProcAddr with NULL (for global functions)
    if (inst_proc) {
        ptr = (void*) inst_proc(VK_NULL_HANDLE, name);
        if (ptr) return ptr;
    }

    return NULL;
}

PFN_vkGetInstanceProcAddr get_real_instance_proc_addr() {
    init_real_vulkan();
    return g_real_vkGetInstanceProcAddr.load(std::memory_order_relaxed);
}

PFN_vkGetDeviceProcAddr get_real_device_proc_addr() {
    init_real_vulkan();
    return g_real_vkGetDeviceProcAddr.load(std::memory_order_relaxed);
}

void set_last_instance(VkInstance instance) {
    g_last_instance.store(instance, std::memory_order_release);
}

VkInstance get_last_instance() {
    return g_last_instance.load(std::memory_order_acquire);
}

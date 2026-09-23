#include "driver_loader.h"
#include <dlfcn.h>
#include <mutex>

static void* g_real_vulkan_handle = NULL;
static PFN_vkGetInstanceProcAddr g_real_vkGetInstanceProcAddr = NULL;
static PFN_vkGetDeviceProcAddr g_real_vkGetDeviceProcAddr = NULL;
static void* g_self_handle = NULL;
static VkInstance g_last_instance = VK_NULL_HANDLE;
static std::mutex g_driver_mutex;

void init_real_vulkan() {
    if (g_real_vulkan_handle != NULL) return;

    std::lock_guard<std::mutex> lock(g_driver_mutex);
    if (g_real_vulkan_handle != NULL) return;

    // 1. Check custom path from environment
    const char* custom_path = getenv("POJAV_REAL_VULKAN_PATH");
    if (!custom_path) custom_path = getenv("DRIVER_PATH");
    if (custom_path && custom_path[0] != '\0') {
        g_real_vulkan_handle = dlopen(custom_path, RTLD_NOW | RTLD_LOCAL);
        if (g_real_vulkan_handle) {
            LOGI("Loaded real Vulkan from custom path: %s", custom_path);
        }
    }

    // 2. Try standard libvulkan.so
    if (!g_real_vulkan_handle) {
        g_real_vulkan_handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (g_real_vulkan_handle) {
            LOGI("Loaded real Vulkan from libvulkan.so");
        }
    }

    // 3. Fallback to system / vendor paths
    if (!g_real_vulkan_handle) {
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
            g_real_vulkan_handle = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
            if (g_real_vulkan_handle) {
                LOGI("Loaded real Vulkan from system path: %s", paths[i]);
                break;
            }
        }
    }

    if (!g_real_vulkan_handle) {
        LOGE("CRITICAL: Failed to load underlying real Vulkan driver!");
        return;
    }

    g_real_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) dlsym(g_real_vulkan_handle, "vkGetInstanceProcAddr");
    if (!g_real_vkGetInstanceProcAddr) {
        LOGE("CRITICAL: vkGetInstanceProcAddr not found in real Vulkan driver!");
    } else {
        LOGI("Successfully loaded real vkGetInstanceProcAddr");
    }

    g_real_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) dlsym(g_real_vulkan_handle, "vkGetDeviceProcAddr");
    if (!g_real_vkGetDeviceProcAddr && g_real_vkGetInstanceProcAddr) {
        g_real_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) g_real_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetDeviceProcAddr");
    }
    if (g_real_vkGetDeviceProcAddr) {
        LOGI("Successfully loaded real vkGetDeviceProcAddr");
    }
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

    // 1. If device is valid, try real vkGetDeviceProcAddr
    if (device != VK_NULL_HANDLE) {
        if (!g_real_vkGetDeviceProcAddr && g_real_vkGetInstanceProcAddr) {
            g_real_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) g_real_vkGetInstanceProcAddr(instance ? instance : g_last_instance, "vkGetDeviceProcAddr");
        }
        if (g_real_vkGetDeviceProcAddr) {
            ptr = (void*) g_real_vkGetDeviceProcAddr(device, name);
            if (ptr) return ptr;
        }
        // Special case: standard Vulkan 1.0 queue functions might be queried from instance on some Vulkan 1.0 loaders
        if (name && (strcmp(name, "vkQueueSubmit") == 0 ||
                     strcmp(name, "vkQueueWaitIdle") == 0 ||
                     strcmp(name, "vkQueueBindSparse") == 0)) {
            VkInstance inst = (instance != VK_NULL_HANDLE) ? instance : g_last_instance;
            if (inst != VK_NULL_HANDLE && g_real_vkGetInstanceProcAddr) {
                ptr = (void*) g_real_vkGetInstanceProcAddr(inst, name);
                if (ptr) return ptr;
            }
        }
        // Device-level function not supported by real driver! DO NOT fall through to dlsym,
        // as Android libvulkan loader exports generic trampolines that branch to NULL and crash!
        return NULL;
    }

    // 2. If instance is valid (or g_last_instance is available), try vkGetInstanceProcAddr
    VkInstance inst = (instance != VK_NULL_HANDLE) ? instance : g_last_instance;
    if (inst != VK_NULL_HANDLE && g_real_vkGetInstanceProcAddr) {
        ptr = (void*) g_real_vkGetInstanceProcAddr(inst, name);
        if (ptr) return ptr;
    }

    // 3. Try dlsym directly on real Vulkan library handle (only for global functions like vkCreateInstance)
    if (g_real_vulkan_handle) {
        ptr = dlsym(g_real_vulkan_handle, name);
        if (ptr) return ptr;
    }

    // 4. Try vkGetInstanceProcAddr with NULL (for global functions)
    if (g_real_vkGetInstanceProcAddr) {
        ptr = (void*) g_real_vkGetInstanceProcAddr(VK_NULL_HANDLE, name);
        if (ptr) return ptr;
    }

    return NULL;
}

PFN_vkGetInstanceProcAddr get_real_instance_proc_addr() {
    init_real_vulkan();
    return g_real_vkGetInstanceProcAddr;
}

PFN_vkGetDeviceProcAddr get_real_device_proc_addr() {
    init_real_vulkan();
    return g_real_vkGetDeviceProcAddr;
}

void set_last_instance(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_driver_mutex);
    g_last_instance = instance;
}

VkInstance get_last_instance() {
    std::lock_guard<std::mutex> lock(g_driver_mutex);
    return g_last_instance;
}

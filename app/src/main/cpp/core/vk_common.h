#ifndef VK_COMMON_H
#define VK_COMMON_H

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/log.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "VulkanLayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

static inline bool is_debug_logging() {
    static int s_debug = -1;
    if (__builtin_expect(s_debug == -1, 0)) {
        const char* env1 = getenv("VK_LAYER_DEBUG");
        const char* env2 = getenv("VK_DEBUG");
        const char* env3 = getenv("VK_FIX_DEBUG");
        bool enabled = (env1 && (strcmp(env1, "1") == 0 || strcmp(env1, "true") == 0)) ||
                       (env2 && (strcmp(env2, "1") == 0 || strcmp(env2, "true") == 0)) ||
                       (env3 && (strcmp(env3, "1") == 0 || strcmp(env3, "true") == 0));
        s_debug = enabled ? 1 : 0;
    }
    return s_debug == 1;
}

#define LOG_OPT_DEBUG(...) do { if (__builtin_expect(is_debug_logging(), 0)) LOGI(__VA_ARGS__); } while(0)

#define VK_EXPORT __attribute__((visibility("default")))
#define VK_LAYER_EXPORT __attribute__((visibility("default")))

// Helper to safely format 32-bit (uint64_t) and 64-bit non-dispatchable Vulkan handles with %p
#define VK_HANDLE(h) ((void*)(uintptr_t)(h))

#ifndef PROJECT_VERSION_NAME
#define PROJECT_VERSION_NAME "1.0"
#endif

#ifndef PROJECT_VERSION_CODE
#define PROJECT_VERSION_CODE "1"
#endif

#endif // VK_COMMON_H

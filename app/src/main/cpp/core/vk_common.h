#ifndef VK_COMMON_H
#define VK_COMMON_H

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/log.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>
#include <stdarg.h>

static inline FILE* get_writable_log_file() {
    static FILE* s_fp = nullptr;
    static bool s_initialized = false;
    if (!s_initialized) {
        s_initialized = true;

        // 1. Custom environment variable
        const char* custom_log = getenv("VULKAN_FIX_LOG_PATH");
        if (custom_log && custom_log[0] != '\0') {
            s_fp = fopen(custom_log, "w");
        }

        // 2. App internal sandboxed directory (HOME env, 100% guaranteed writable by process)
        if (!s_fp) {
            const char* home = getenv("HOME");
            if (home && home[0] != '\0') {
                char path[1024];
                snprintf(path, sizeof(path), "%s/vulkanfix.log", home);
                s_fp = fopen(path, "w");
                if (!s_fp) {
                    snprintf(path, sizeof(path), "%s/.minecraft/vulkanfix.log", home);
                    s_fp = fopen(path, "w");
                }
            }
        }

        // 3. Current working directory
        if (!s_fp) {
            s_fp = fopen("./vulkanfix.log", "w");
        }

        // 4. External storage app-private directories
        if (!s_fp) {
            const char* ext = getenv("EXTERNAL_STORAGE");
            if (ext && ext[0] != '\0') {
                const char* subpaths[] = {
                    "/Android/data/net.kdt.pojavlaunch/files/vulkanfix.log",
                    "/Android/data/org.pojavlauncher.mobile/files/vulkanfix.log",
                    "/vulkanfix.log"
                };
                for (const char* sub : subpaths) {
                    char path[1024];
                    snprintf(path, sizeof(path), "%s%s", ext, sub);
                    s_fp = fopen(path, "w");
                    if (s_fp) break;
                }
            }
        }

        // 5. Fallback paths
        if (!s_fp) {
            s_fp = fopen("/data/local/tmp/vulkanfix.log", "w");
        }
        if (!s_fp) {
            s_fp = fopen("/sdcard/vulkanfix.log", "w");
        }
    }
    return s_fp;
}

static inline void file_log(const char* level, const char* fmt, ...) {
    FILE* fp = get_writable_log_file();
    if (fp) {
        va_list args;
        va_start(args, fmt);
        fprintf(fp, "[%s] ", level);
        vfprintf(fp, fmt, args);
        fprintf(fp, "\n");
        fflush(fp);
        va_end(args);
    }
}

#define LOGI(...) do { file_log("INFO", __VA_ARGS__); } while(0)
#define LOGW(...) do { file_log("WARN", __VA_ARGS__); } while(0)
#define LOGE(...) do { file_log("ERROR", __VA_ARGS__); } while(0)
#define LOGD(...) do { file_log("DEBUG", __VA_ARGS__); } while(0)

static inline bool is_debug_logging() {
    static int s_debug = -1;
    if (__builtin_expect(s_debug == -1, 0)) {
        const char* env = getenv("VULKAN_FIX_DEBUG");
        s_debug = (env && (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0)) ? 1 : 0;
    }
    return s_debug == 1;
}

#define LOG_OPT_DEBUG(...) do { if (__builtin_expect(is_debug_logging(), 0)) LOGI(__VA_ARGS__); } while(0)

enum class EmulationMode {
    Auto = 0,        // 自动：根据驱动原生扩展支持自动选择 Native 或 Emulation
    ForceEmulate,    // 强制模拟：忽略原生扩展，强行使用模块软件/层级模拟
    Skip             // 跳过/直通：强行禁用模拟，直通原生驱动（Bypass）
};

static inline EmulationMode parse_emulation_mode(const char* env1, const char* env2 = nullptr) {
    const char* val = getenv(env1);
    if ((!val || val[0] == '\0') && env2) {
        val = getenv(env2);
    }
    if (!val || val[0] == '\0') return EmulationMode::Auto;

    if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 ||
        strcasecmp(val, "force") == 0 || strcasecmp(val, "emulate") == 0 || strcasecmp(val, "on") == 0) {
        return EmulationMode::ForceEmulate;
    }

    if (strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0 ||
        strcasecmp(val, "skip") == 0 || strcasecmp(val, "bypass") == 0 ||
        strcasecmp(val, "off") == 0 || strcasecmp(val, "native") == 0) {
        return EmulationMode::Skip;
    }

    return EmulationMode::Auto;
}

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

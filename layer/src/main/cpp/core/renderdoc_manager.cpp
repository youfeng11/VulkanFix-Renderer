#include "renderdoc_manager.h"
#include <dlfcn.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

RenderDocManager& RenderDocManager::get() {
    static RenderDocManager s_instance;
    return s_instance;
}

void RenderDocManager::init() {
    if (m_initialized.exchange(true)) return;

    pRENDERDOC_GetAPI get_api = nullptr;

    // 1. Check if RenderDoc is already resident in process memory (e.g. injected by RenderDoc server/layer)
#if defined(__linux__) || defined(__ANDROID__)
    void* self_mod = dlopen(NULL, RTLD_NOW);
    if (self_mod) {
        get_api = (pRENDERDOC_GetAPI)dlsym(self_mod, "RENDERDOC_GetAPI");
        dlclose(self_mod);
    }

    // 2. If not found in global namespace, check configured or default library locations
    if (!get_api) {
        const char* custom_lib = getenv("VULKAN_FIX_RENDERDOC_LIB");
        const char* search_libs[] = {
            custom_lib,
            "libVkLayer_GLES_RenderDoc.so",
            "libVkLayer_renderdoc_capture.so",
            "librenderdoc.so",
            "/data/local/tmp/libVkLayer_GLES_RenderDoc.so",
            "/data/local/tmp/libVkLayer_renderdoc_capture.so",
            "/data/local/tmp/librenderdoc.so"
        };
        for (const char* lib_name : search_libs) {
            if (!lib_name || lib_name[0] == '\0') continue;
            void* mod = dlopen(lib_name, RTLD_NOW | RTLD_LOCAL);
            if (mod) {
                get_api = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
                if (get_api) {
                    m_rdoc_lib = mod;
                    LOGI("RenderDocManager: Loaded RenderDoc from %s", lib_name);
                    break;
                }
                dlclose(mod);
            }
        }
    }
#endif

    if (!get_api) {
        LOG_OPT_DEBUG("RenderDocManager: RenderDoc library not detected (running normally)");
        return;
    }

    // Request RenderDoc API (supporting 1.6.0 down to 1.1.2)
    int ret = get_api(eRENDERDOC_API_Version_1_6_0, (void**)&m_rdoc_api);
    if (ret != 1 || !m_rdoc_api) {
        ret = get_api(eRENDERDOC_API_Version_1_4_0, (void**)&m_rdoc_api);
    }
    if (ret != 1 || !m_rdoc_api) {
        ret = get_api(eRENDERDOC_API_Version_1_1_2, (void**)&m_rdoc_api);
    }

    if (ret == 1 && m_rdoc_api) {
        int major = 0, minor = 0, patch = 0;
        m_rdoc_api->GetAPIVersion(&major, &minor, &patch);
        LOGI("RenderDocManager: Successfully connected to RenderDoc In-App API v%d.%d.%d", major, minor, patch);

        // Optimal capture options for Vulkan graphics debugging
        m_rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureAllCmdLists, 1);
        m_rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_SaveAllInitials, 1);
        m_rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 1);
        m_rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 1);

        // Configure capture directory
        const char* custom_dir = getenv("VULKAN_FIX_CAPTURE_DIR");
        if (custom_dir && custom_dir[0] != '\0') {
            m_capture_dir = custom_dir;
        } else {
            const char* home = getenv("HOME");
            if (home && home[0] != '\0' && strcmp(home, "/") != 0) {
                m_capture_dir = std::string(home) + "/vulkanfix_capture";
            } else if (access("/sdcard/Download", W_OK) == 0) {
                m_capture_dir = "/sdcard/Download/vulkanfix_capture";
            } else {
                m_capture_dir = "/data/local/tmp/vulkanfix_capture";
            }
        }
        m_rdoc_api->SetCaptureFilePathTemplate(m_capture_dir.c_str());
        LOGI("RenderDocManager: Capture path template set to '%s'", m_capture_dir.c_str());

        // Configure trigger file
        const char* custom_trigger = getenv("VULKAN_FIX_CAPTURE_TRIGGER");
        if (custom_trigger && custom_trigger[0] != '\0') {
            m_trigger_file = custom_trigger;
        } else {
            const char* home = getenv("HOME");
            if (home && home[0] != '\0' && strcmp(home, "/") != 0) {
                m_trigger_file = std::string(home) + "/vulkanfix_capture.trigger";
            } else if (access("/sdcard", W_OK) == 0) {
                m_trigger_file = "/sdcard/vulkanfix_capture.trigger";
            } else {
                m_trigger_file = "/data/local/tmp/vulkanfix_capture.trigger";
            }
        }

        // Configure target frame if set
        const char* target_str = getenv("VULKAN_FIX_CAPTURE_FRAME");
        if (target_str && target_str[0] != '\0') {
            uint64_t target = (uint64_t)strtoull(target_str, nullptr, 10);
            if (target > 0) {
                m_target_frame.store(target);
                LOGI("RenderDocManager: Scheduled capture for frame %" PRIu64, target);
            }
        }

        const char* count_str = getenv("VULKAN_FIX_CAPTURE_COUNT");
        if (count_str && count_str[0] != '\0') {
            uint32_t count = (uint32_t)strtoul(count_str, nullptr, 10);
            if (count > 0) {
                m_remaining_captures.store(count);
            }
        }
    }
}

void RenderDocManager::trigger_capture(uint32_t num_frames) {
    if (!m_rdoc_api) return;
    uint32_t n = (num_frames > 0) ? num_frames : 1;
    m_remaining_captures.store(n, std::memory_order_release);
    LOGI("RenderDocManager: Capture manually triggered for %u frame(s)", n);
}

void RenderDocManager::start_capture(VkDevice device) {
    if (!m_rdoc_api) return;
    VkDevice dev = device;
    if (dev == VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(m_mutex);
        dev = m_active_device;
    }
    m_rdoc_api->StartFrameCapture(dev, NULL);
    m_capturing.store(true, std::memory_order_release);
    LOGI("RenderDocManager: StartFrameCapture initiated (device: %p)", dev);
}

void RenderDocManager::end_capture(VkDevice device) {
    if (!m_rdoc_api) return;
    VkDevice dev = device;
    if (dev == VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(m_mutex);
        dev = m_active_device;
    }
    m_rdoc_api->EndFrameCapture(dev, NULL);
    m_capturing.store(false, std::memory_order_release);
    LOGI("RenderDocManager: EndFrameCapture completed (device: %p)", dev);

    uint32_t numCaptures = m_rdoc_api->GetNumCaptures();
    if (numCaptures > 0) {
        char path[1024] = {0};
        uint32_t pathLen = sizeof(path);
        uint64_t timestamp = 0;
        if (m_rdoc_api->GetCapture(numCaptures - 1, path, &pathLen, &timestamp)) {
            LOGI("RenderDocManager: Capture file successfully written to: %s", path);
        }
    }
}

void RenderDocManager::on_device_created(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_active_device = device;
}

void RenderDocManager::on_device_destroyed(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_active_device == device) {
        m_active_device = VK_NULL_HANDLE;
    }
}

void RenderDocManager::check_trigger_file() {
    if (m_trigger_file.empty()) return;
    if (access(m_trigger_file.c_str(), F_OK) == 0) {
        LOGI("RenderDocManager: Detected trigger file '%s', starting capture!", m_trigger_file.c_str());
        unlink(m_trigger_file.c_str());
        trigger_capture(1);
    }
}

void RenderDocManager::on_pre_present(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    (void)queue;
    (void)pPresentInfo;

    if (!m_rdoc_api) return;

    if (m_capturing.load(std::memory_order_relaxed)) {
        VkDevice dev = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            dev = m_active_device;
        }

        m_rdoc_api->EndFrameCapture(dev, NULL);
        m_capturing.store(false, std::memory_order_release);

        uint32_t rem = m_remaining_captures.load(std::memory_order_relaxed);
        if (rem > 0) {
            m_remaining_captures.store(rem - 1, std::memory_order_relaxed);
        }

        uint64_t current_frame = m_frame_counter.load(std::memory_order_relaxed);
        LOGI("RenderDocManager: Captured frame %" PRIu64 " successfully!", current_frame);

        uint32_t numCaptures = m_rdoc_api->GetNumCaptures();
        if (numCaptures > 0) {
            char path[1024] = {0};
            uint32_t pathLen = sizeof(path);
            uint64_t timestamp = 0;
            if (m_rdoc_api->GetCapture(numCaptures - 1, path, &pathLen, &timestamp)) {
                LOGI("RenderDocManager: RDC file written to: %s", path);
            }
        }
    }
}

void RenderDocManager::on_post_present(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    (void)queue;
    (void)pPresentInfo;

    if (!m_rdoc_api) return;

    uint64_t current_frame = ++m_frame_counter;

    // Check trigger file periodically (every 15 frames to minimize syscall overhead)
    if ((current_frame % 15) == 0) {
        check_trigger_file();
    }

    if (!m_capturing.load(std::memory_order_relaxed)) {
        bool should_start = false;

        uint32_t rem = m_remaining_captures.load(std::memory_order_acquire);
        if (rem > 0) {
            should_start = true;
        } else {
            uint64_t target = m_target_frame.load(std::memory_order_acquire);
            if (target > 0 && (current_frame + 1) == target) {
                should_start = true;
            }
        }

        if (should_start) {
            VkDevice dev = VK_NULL_HANDLE;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                dev = m_active_device;
            }
            m_rdoc_api->StartFrameCapture(dev, NULL);
            m_capturing.store(true, std::memory_order_release);
            LOGI("RenderDocManager: Started frame capture for frame %" PRIu64, current_frame + 1);
        }
    }
}

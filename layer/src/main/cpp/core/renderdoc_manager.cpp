#include "renderdoc_manager.h"
#include "driver_loader.h"
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
std::string setting(const char *name, const std::string &fallback) {
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}
std::string json(const std::string &value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32) {
            char escaped[7];
            snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        } else
            out += char(c);
    }
    return out + '"';
}
} // namespace
RenderDocManager &RenderDocManager::get() {
    static RenderDocManager manager;
    return manager;
}
void RenderDocManager::init() {
    if (is_available())
        return;
    std::lock_guard<std::mutex> initialization(m_init_mutex);
    if (is_available())
        return;
    auto get_api =
        (pRENDERDOC_GetAPI)get_real_proc(VK_NULL_HANDLE, VK_NULL_HANDLE, "RENDERDOC_GetAPI");
    if (!get_api)
        get_api = (pRENDERDOC_GetAPI)dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI");
    if (!get_api) {
        const char *paths[] = {getenv("VULKAN_FIX_RENDERDOC_LIB"),
                               "libVkLayer_GLES_RenderDoc.so",
                               "libVkLayer_renderdoc_capture.so",
                               "librenderdoc.so",
                               "/data/local/tmp/libVkLayer_GLES_RenderDoc.so",
                               "/data/local/tmp/librenderdoc.so"};
        for (auto path : paths) {
            if (!path || !*path)
                continue;
            // Discover an injected layer without loading capture tooling into
            // ordinary runs merely because a library exists in /data/local/tmp.
            const auto custom = getenv("VULKAN_FIX_RENDERDOC_LIB");
            const bool explicit_path = custom && *custom && !strcmp(path, custom);
            auto library = dlopen(path, RTLD_NOW | RTLD_LOCAL | (explicit_path ? 0 : RTLD_NOLOAD));
            if (!library)
                continue;
            get_api = (pRENDERDOC_GetAPI)dlsym(library, "RENDERDOC_GetAPI");
            if (get_api) {
                m_library = library;
                break;
            }
            dlclose(library);
        }
    }
    if (!get_api)
        return;
    RENDERDOC_API_1_6_0 *api = nullptr;
    bool obtained = false;
    for (auto version : {eRENDERDOC_API_Version_1_6_0, eRENDERDOC_API_Version_1_4_0,
                         eRENDERDOC_API_Version_1_1_2}) {
        api = nullptr;
        if (get_api(version, (void **)&api) == 1 && api) {
            m_can_discard = version >= eRENDERDOC_API_Version_1_4_0;
            obtained = true;
            break;
        }
    }
    if (!obtained || !api || !api->SetCaptureOptionU32 || !api->SetCaptureFilePathTemplate ||
        !api->IsFrameCapturing || !api->StartFrameCapture || !api->EndFrameCapture ||
        !api->GetCapture || !api->GetNumCaptures)
        return;
    api->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 0);
    auto home = setting("HOME", "/data/local/tmp");
    if (home == "/")
        home = access("/sdcard/Download", W_OK) == 0 ? "/sdcard/Download" : "/data/local/tmp";
    std::lock_guard<std::mutex> lock(m_mutex);
    m_template = setting("VULKAN_FIX_CAPTURE_DIR", home + "/vulkanfix_capture");
    m_trigger = setting("VULKAN_FIX_CAPTURE_TRIGGER", home + "/vulkanfix_capture.trigger");
    m_status = setting("VULKAN_FIX_CAPTURE_STATUS", home + "/vulkanfix-renderdoc.json");
    m_target_frame = strtoull(setting("VULKAN_FIX_CAPTURE_FRAME", "0").c_str(), nullptr, 10);
    m_pending = uint32_t(strtoul(setting("VULKAN_FIX_CAPTURE_COUNT", "0").c_str(), nullptr, 10));
    api->SetCaptureFilePathTemplate(m_template.c_str());
    m_api.store(api, std::memory_order_release);
    status_locked();
    LOGI("RenderDoc: control API available; Vulkan capture is unverified until a new RDC is "
         "written. status=%s trigger=%s",
         m_status.c_str(), m_trigger.c_str());
}
void RenderDocManager::status_locked() {
    if (!is_available() || m_status.empty())
        return;
    const auto temporary = m_status + ".tmp";
    auto file = fopen(temporary.c_str(), "w");
    if (!file) {
        LOGW("RenderDoc: cannot write status %s", temporary.c_str());
        return;
    }
    fprintf(file,
            "{\"pid\":%d,\"api_available\":true,\"capturing\":%s,\"completed\":%u,\"pending\":%u,"
            "\"devices\":%zu,\"trigger\":%s,\"capture_template\":%s,\"last_capture\":%s,\"error\":%"
            "s}\n",
            getpid(), is_capturing() ? "true" : "false", m_completed, m_pending, m_devices.size(),
            json(m_trigger).c_str(), json(m_template).c_str(), json(m_last_capture).c_str(),
            json(m_error).c_str());
    const bool written = !ferror(file);
    const int closed = fclose(file);
    if (!written || closed || rename(temporary.c_str(), m_status.c_str()))
        LOGW("RenderDoc: status update failed for %s", m_status.c_str());
}
void RenderDocManager::fail_locked(const char *error) {
    m_error = error;
    LOGW("RenderDoc: %s", error);
    status_locked();
}
void RenderDocManager::trigger_capture(uint32_t count) {
    init();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!is_available()) {
        LOGW("RenderDoc: capture requested without a control API");
        return;
    }
    m_pending = count ? count : 1;
    m_error.clear();
    status_locked();
}
bool RenderDocManager::start_locked(VkDevice device, bool automatic) {
    auto api = m_api.load(std::memory_order_acquire);
    if (!api)
        return false;
    if (is_capturing() || api->IsFrameCapturing()) {
        fail_locked("another capture is already active");
        return false;
    }
    if (!device && m_devices.size() == 1)
        device = m_devices.begin()->first;
    auto context = m_devices.find(device);
    if (context == m_devices.end() || !context->second.instance) {
        fail_locked("capture requires a known device and its owning VkInstance");
        return false;
    }
    auto pointer = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(context->second.instance);
    m_before_count = api->GetNumCaptures();
    api->StartFrameCapture(pointer, nullptr);
    if (!api->IsFrameCapturing()) {
        fail_locked("StartFrameCapture did not start; check the Vulkan capture layer chain");
        return false;
    }
    m_capture_pointer = pointer;
    m_capture_device = device;
    m_automatic = automatic;
    m_capturing.store(true, std::memory_order_release);
    m_error.clear();
    status_locked();
    LOGI("RenderDoc: capture started for device %p", device);
    return true;
}
bool RenderDocManager::start_capture(VkDevice device) {
    init();
    std::lock_guard<std::mutex> lock(m_mutex);
    return start_locked(device, false);
}
bool RenderDocManager::end_locked() {
    if (!is_capturing())
        return false;
    auto api = m_api.load(std::memory_order_acquire);
    const bool ended = api->EndFrameCapture(m_capture_pointer, nullptr) == 1;
    if (!ended && api->IsFrameCapturing()) {
        fail_locked("EndFrameCapture failed; capture remains active");
        return false;
    }
    m_capturing.store(false, std::memory_order_release);
    m_capture_device = VK_NULL_HANDLE;
    m_capture_pointer = nullptr;
    m_automatic = false;
    if (!ended) {
        fail_locked("EndFrameCapture failed; no successful capture recorded");
        return false;
    }
    const auto count = api->GetNumCaptures();
    if (count <= m_before_count) {
        fail_locked("capture ended without a new RDC file");
        return false;
    }
    uint32_t length = 0;
    uint64_t timestamp = 0;
    if (!api->GetCapture(count - 1, nullptr, &length, &timestamp) || !length ||
        length > 1024 * 1024) {
        fail_locked("cannot query the new RDC path");
        return false;
    }
    std::vector<char> path(size_t(length) + 1, 0);
    if (!api->GetCapture(count - 1, path.data(), &length, &timestamp)) {
        fail_locked("cannot read the new RDC path");
        return false;
    }
    struct stat info {};
    if (stat(path.data(), &info) || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        fail_locked("new RDC file is missing or empty");
        return false;
    }
    m_last_capture = path.data();
    ++m_completed;
    m_error.clear();
    status_locked();
    LOGI("RenderDoc: RDC written: %s (%llu bytes)", m_last_capture.c_str(),
         (unsigned long long)info.st_size);
    return true;
}
bool RenderDocManager::end_capture(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (device && device != m_capture_device) {
        fail_locked("capture belongs to another device");
        return false;
    }
    return end_locked();
}
void RenderDocManager::abort_locked() {
    auto api = m_api.load(std::memory_order_acquire);
    if (m_can_discard && api && api->DiscardFrameCapture)
        api->DiscardFrameCapture(m_capture_pointer, nullptr);
    m_capturing.store(false, std::memory_order_release);
    m_capture_device = VK_NULL_HANDLE;
    m_capture_pointer = nullptr;
    m_automatic = false;
    fail_locked("capture aborted because its target was destroyed");
}
void RenderDocManager::on_device_created(VkDevice device, VkInstance instance) {
    init();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_devices[device] = {instance, 0};
    status_locked();
}
void RenderDocManager::on_device_destroyed(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (is_capturing() && device == m_capture_device && !end_locked() && is_capturing())
        abort_locked();
    m_devices.erase(device);
    status_locked();
}
void RenderDocManager::on_instance_destroyed(VkInstance instance) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_devices.begin(); it != m_devices.end();) {
        if (it->second.instance != instance) {
            ++it;
            continue;
        }
        if (is_capturing() && it->first == m_capture_device && !end_locked() && is_capturing())
            abort_locked();
        it = m_devices.erase(it);
    }
    status_locked();
}
void RenderDocManager::on_post_present(VkDevice device, VkResult result) {
    if (!is_available())
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto context = m_devices.find(device);
    if (context == m_devices.end())
        return;
    if (is_capturing() && m_automatic && device == m_capture_device)
        end_locked();
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return;
    const auto frame = ++context->second.frames;
    if (frame % 15 == 0 && !m_trigger.empty() && access(m_trigger.c_str(), F_OK) == 0) {
        if (unlink(m_trigger.c_str()) == 0) {
            m_pending = 1;
            LOGI("RenderDoc: consumed trigger %s", m_trigger.c_str());
        }
    }
    if (is_capturing())
        return;
    const bool scheduled = m_target_frame && frame + 1 >= m_target_frame;
    if (!m_pending && !scheduled)
        return;
    if (m_pending)
        --m_pending;
    if (scheduled)
        m_target_frame = 0;
    start_locked(device, true);
}
std::string RenderDocManager::last_capture_path() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_capture;
}

#pragma once
#include "renderdoc_app.h"
#include "vk_common.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>

class RenderDocManager {
  public:
    static RenderDocManager &get();
    void init();
    bool is_available() const { return m_api.load(std::memory_order_acquire) != nullptr; }
    bool is_capturing() const { return m_capturing.load(std::memory_order_acquire); }
    void trigger_capture(uint32_t count = 1);
    bool start_capture(VkDevice device = VK_NULL_HANDLE);
    bool end_capture(VkDevice device = VK_NULL_HANDLE);
    void on_device_created(VkDevice device, VkInstance instance);
    void on_device_destroyed(VkDevice device);
    void on_instance_destroyed(VkInstance instance);
    void on_post_present(VkDevice device, VkResult result);
    std::string last_capture_path();

  private:
    struct Context {
        VkInstance instance{};
        uint64_t frames = 0;
    };
    RenderDocManager() = default;
    bool start_locked(VkDevice device, bool automatic);
    bool end_locked();
    void abort_locked();
    void status_locked();
    void fail_locked(const char *error);
    std::atomic<RENDERDOC_API_1_6_0 *> m_api{nullptr};
    std::atomic<bool> m_capturing{false};
    std::mutex m_init_mutex, m_mutex;
    std::map<VkDevice, Context> m_devices;
    void *m_library = nullptr;
    std::string m_trigger, m_status, m_template, m_last_capture, m_error;
    RENDERDOC_DevicePointer m_capture_pointer = nullptr;
    VkDevice m_capture_device = VK_NULL_HANDLE;
    uint32_t m_before_count = 0, m_pending = 0, m_completed = 0;
    uint64_t m_target_frame = 0;
    bool m_automatic = false;
    bool m_can_discard = false;
};

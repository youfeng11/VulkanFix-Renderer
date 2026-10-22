#ifndef RENDERDOC_MANAGER_H
#define RENDERDOC_MANAGER_H

#include "vk_common.h"
#include "renderdoc_app.h"
#include <atomic>
#include <string>
#include <mutex>

class RenderDocManager {
public:
    static RenderDocManager& get();

    void init();
    bool is_available() const { return m_rdoc_api != nullptr; }
    bool is_capturing() const { return m_capturing.load(std::memory_order_relaxed); }

    void trigger_capture(uint32_t num_frames = 1);
    void start_capture(VkDevice device = VK_NULL_HANDLE);
    void end_capture(VkDevice device = VK_NULL_HANDLE);

    void on_device_created(VkDevice device);
    void on_device_destroyed(VkDevice device);

    void on_pre_present(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
    void on_post_present(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);

private:
    RenderDocManager() = default;
    ~RenderDocManager() = default;

    void check_trigger_file();

    RENDERDOC_API_1_6_0* m_rdoc_api = nullptr;
    void* m_rdoc_lib = nullptr;

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_capturing{false};
    std::atomic<uint64_t> m_frame_counter{0};
    std::atomic<uint64_t> m_target_frame{0};
    std::atomic<uint32_t> m_remaining_captures{0};

    std::string m_trigger_file;
    std::string m_capture_dir;
    VkDevice m_active_device{VK_NULL_HANDLE};
    std::mutex m_mutex;
};

#endif // RENDERDOC_MANAGER_H

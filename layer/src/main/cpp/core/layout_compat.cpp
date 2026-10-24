#include "layout_compat.h"

LayoutCompat& LayoutCompat::get() {
    static LayoutCompat s_instance;
    return s_instance;
}

void LayoutCompat::set_device_emulated(VkDevice device, bool emulated) {
    if (device == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t key = reinterpret_cast<uint64_t>(device);
    if (emulated) {
        if (m_devices.insert(key).second) {
            m_count.fetch_add(1, std::memory_order_release);
        }
    } else {
        if (m_devices.erase(key) != 0) {
            m_count.fetch_sub(1, std::memory_order_release);
        }
    }
}

bool LayoutCompat::is_emulated(VkDevice device) const {
    if (device == VK_NULL_HANDLE) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_devices.count(reinterpret_cast<uint64_t>(device)) != 0;
}

void LayoutCompat::remove_device(VkDevice device) {
    if (device == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_devices.erase(reinterpret_cast<uint64_t>(device)) != 0) {
        m_count.fetch_sub(1, std::memory_order_release);
    }
}

VkImageLayout LayoutCompat::sanitize(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
            // The combined attachment layout is valid for any single-aspect
            // attachment; allowing the other aspect to be written as well is a
            // legal over-synchronisation.
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        default:
            return layout;
    }
}

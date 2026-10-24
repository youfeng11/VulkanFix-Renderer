#ifndef LAYOUT_COMPAT_H
#define LAYOUT_COMPAT_H

#include "vk_common.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

/**
 * VK_KHR_separate_depth_stencil_layouts / Vulkan 1.2 core emulation.
 *
 * Vulkan 1.0 only knows the combined DEPTH_STENCIL_* layouts. When an
 * application uses the separate depth/stencil layouts (advertised by the layer)
 * every layout handed to the driver is rewritten to the closest combined
 * layout. Over-synchronising with the combined layout is always valid; it only
 * loses a small amount of pipelining on hardware that could split the aspects.
 */
class LayoutCompat {
public:
    static LayoutCompat& get();

    void set_device_emulated(VkDevice device, bool emulated);
    bool is_emulated(VkDevice device) const;
    void remove_device(VkDevice device);

    // Fast check for hot command-recording paths.
    bool any_emulated() const { return m_count.load(std::memory_order_relaxed) > 0; }

    static VkImageLayout sanitize(VkImageLayout layout);

private:
    LayoutCompat() = default;
    ~LayoutCompat() = default;
    LayoutCompat(const LayoutCompat&) = delete;
    LayoutCompat& operator=(const LayoutCompat&) = delete;

    mutable std::mutex m_mutex;
    std::unordered_set<uint64_t> m_devices;
    std::atomic<uint32_t> m_count{0};
};

#endif  // LAYOUT_COMPAT_H

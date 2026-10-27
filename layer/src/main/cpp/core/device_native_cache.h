#ifndef VKFIX_DEVICE_NATIVE_CACHE_H
#define VKFIX_DEVICE_NATIVE_CACHE_H

#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
#include <mutex>

// A stable primary-device entry avoids cache-line bouncing between query
// threads. Other devices use the owning module's synchronized device map.
class DeviceNativeCache {
public:
    bool lookup(VkDevice device, bool& native) const {
        // All fields participate in the same sequentially consistent order.
        // Matching even revisions prove no publication crossed the pair read.
        const auto revision = m_revision.load();
        if ((revision & 1) || m_device.load() != device) return false;
        const bool value = m_native.load();
        if (m_revision.load() != revision) return false;
        native = value;
        return true;
    }

    // The caller holds its device-map lock through remember()/invalidate().
    void remember(VkDevice device, bool native) {
        if (m_device.load() != VK_NULL_HANDLE) return;
        std::lock_guard<std::mutex> lock(m_writer);
        if (m_device.load() != VK_NULL_HANDLE) return;
        m_revision.fetch_add(1);
        m_native.store(native);
        m_device.store(device);
        m_revision.fetch_add(1);
    }

    void invalidate() {
        std::lock_guard<std::mutex> lock(m_writer);
        m_revision.fetch_add(1);
        m_device.store(VK_NULL_HANDLE);
        m_native.store(false);
        m_revision.fetch_add(1);
    }

private:
    std::mutex m_writer;
    std::atomic<uint64_t> m_revision{0};
    std::atomic<VkDevice> m_device{VK_NULL_HANDLE};
    std::atomic<bool> m_native{false};
};

#endif

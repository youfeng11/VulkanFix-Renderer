#include "imageless_framebuffer_manager.h"

#include "driver_loader.h"
#include "vk_pnext.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <type_traits>

namespace {

inline void hash_combine(size_t& seed, size_t value) {
    seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
}

// VkFramebuffer / VkRenderPass / VkImageView are pointers on 64-bit and 64-bit
// integers on 32-bit ABIs; normalise both representations.
template <typename T>
inline uint64_t handle_to_u64(T handle) {
    if constexpr (std::is_pointer<T>::value) {
        return reinterpret_cast<uint64_t>(handle);
    } else {
        return static_cast<uint64_t>(handle);
    }
}

template <typename T>
inline T u64_to_handle(uint64_t value) {
    if constexpr (std::is_pointer<T>::value) {
        return reinterpret_cast<T>(static_cast<uintptr_t>(value));
    } else {
        return static_cast<T>(value);
    }
}
PFN_vkCreateFramebuffer real_create_framebuffer(VkDevice device) {
    return (PFN_vkCreateFramebuffer) get_real_proc(get_last_instance(), device, "vkCreateFramebuffer");
}

PFN_vkDestroyFramebuffer real_destroy_framebuffer(VkDevice device) {
    return (PFN_vkDestroyFramebuffer) get_real_proc(get_last_instance(), device, "vkDestroyFramebuffer");
}

}  // namespace

ImagelessFramebufferManager& ImagelessFramebufferManager::get() {
    static ImagelessFramebufferManager s_instance;
    return s_instance;
}

size_t ImagelessFramebufferManager::KeyHash::operator()(const Key& key) const {
    size_t seed = 0;
    hash_combine(seed, static_cast<size_t>(handle_to_u64(key.renderPass)));
    hash_combine(seed, key.width);
    hash_combine(seed, key.height);
    hash_combine(seed, key.layers);
    for (VkImageView view : key.views) {
        hash_combine(seed, static_cast<size_t>(handle_to_u64(view)));
    }
    return seed;
}

void ImagelessFramebufferManager::set_device_emulated(VkDevice device, bool emulated) {
    if (device == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t key = handle_to_u64(device);
    if (emulated) {
        m_emulated_devices.insert(key);
    } else {
        m_emulated_devices.erase(key);
    }
}

bool ImagelessFramebufferManager::is_emulated(VkDevice device) const {
    if (device == VK_NULL_HANDLE) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_emulated_devices.count(handle_to_u64(device)) != 0;
}

bool ImagelessFramebufferManager::is_imageless(VkFramebuffer framebuffer) const {
    if (framebuffer == VK_NULL_HANDLE) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_records.count(handle_to_u64(framebuffer)) != 0;
}

VkResult ImagelessFramebufferManager::create(
    VkDevice device,
    const VkFramebufferCreateInfo* pCreateInfo,
    VkFramebuffer* pFramebuffer
) {
    if (device == VK_NULL_HANDLE || pCreateInfo == nullptr || pFramebuffer == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    Record record;
    record.device = device;
    record.renderPass = pCreateInfo->renderPass;
    record.width = pCreateInfo->width;
    record.height = pCreateInfo->height;
    record.layers = pCreateInfo->layers;

    const auto* attachments = vku::find_pnext<VkFramebufferAttachmentsCreateInfo>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO);
    if (attachments != nullptr) {
        record.attachmentCount = attachments->attachmentImageInfoCount;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t handle = m_next_handle.fetch_add(1, std::memory_order_relaxed);
    m_records[handle] = record;
    m_record_count.fetch_add(1, std::memory_order_relaxed);
    *pFramebuffer = u64_to_handle<VkFramebuffer>(handle);

    LOG_OPT_DEBUG("ImagelessFramebuffer: synthetic handle %p (%ux%u, %u layers, %u attachments)",
                  VK_HANDLE(*pFramebuffer), record.width, record.height, record.layers, record.attachmentCount);
    return VK_SUCCESS;
}

bool ImagelessFramebufferManager::destroy(VkDevice device, VkFramebuffer framebuffer) {
    if (framebuffer == VK_NULL_HANDLE) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t handle = handle_to_u64(framebuffer);
    auto it = m_records.find(handle);
    if (it == m_records.end()) return false;

    destroy_for_owner_locked(handle);
    m_records.erase(it);
    m_record_count.fetch_sub(1, std::memory_order_relaxed);
    LOG_OPT_DEBUG("ImagelessFramebuffer: destroyed synthetic handle %p", VK_HANDLE(framebuffer));
    return true;
}

ImagelessFramebufferManager::ResolveResult ImagelessFramebufferManager::resolve(
    const VkRenderPassBeginInfo* pBegin,
    VkRenderPassBeginInfo& out
) {
    if (pBegin == nullptr || pBegin->framebuffer == VK_NULL_HANDLE) return ResolveResult::NotImageless;
    if (m_record_count.load(std::memory_order_relaxed) == 0) return ResolveResult::NotImageless;

    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t handle = handle_to_u64(pBegin->framebuffer);
    auto record_it = m_records.find(handle);
    if (record_it == m_records.end()) return ResolveResult::NotImageless;

    const Record& record = record_it->second;
    const auto* attachments = vku::find_pnext<VkRenderPassAttachmentBeginInfo>(
        pBegin->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO);
    if (attachments == nullptr || attachments->attachmentCount == 0 || attachments->pAttachments == nullptr) {
        LOGE("ImagelessFramebuffer: begin without VkRenderPassAttachmentBeginInfo (framebuffer %p)",
             VK_HANDLE(pBegin->framebuffer));
        return ResolveResult::Failed;
    }

    Key key;
    key.renderPass = record.renderPass;
    key.width = record.width;
    key.height = record.height;
    key.layers = record.layers;
    key.views.assign(attachments->pAttachments, attachments->pAttachments + attachments->attachmentCount);

    auto real_it = m_real.find(key);
    if (real_it == m_real.end()) {
        PFN_vkCreateFramebuffer create_fn = real_create_framebuffer(record.device);
        if (create_fn == nullptr) {
            LOGE("ImagelessFramebuffer: vkCreateFramebuffer not available on device %p", (void*)record.device);
            return ResolveResult::Failed;
        }

        VkFramebufferCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        create_info.renderPass = key.renderPass;
        create_info.attachmentCount = static_cast<uint32_t>(key.views.size());
        create_info.pAttachments = key.views.data();
        create_info.width = key.width;
        create_info.height = key.height;
        create_info.layers = key.layers;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        const VkResult result = create_fn(record.device, &create_info, nullptr, &framebuffer);
        if (result != VK_SUCCESS) {
            LOGE("ImagelessFramebuffer: real framebuffer creation failed (result %d, %zu views)", result, key.views.size());
            return ResolveResult::Failed;
        }

        RealFramebuffer entry;
        entry.framebuffer = framebuffer;
        entry.device = record.device;
        entry.owner = handle;
        entry.views = key.views;
        real_it = m_real.emplace(key, std::move(entry)).first;
        m_real_count.fetch_add(1, std::memory_order_relaxed);
        LOG_OPT_DEBUG("ImagelessFramebuffer: created real framebuffer %p for %zu views",
                      (void*)framebuffer, key.views.size());
    }

    out = *pBegin;
    out.framebuffer = real_it->second.framebuffer;
    return ResolveResult::Resolved;
}

void ImagelessFramebufferManager::on_destroy_image_view(VkDevice device, VkImageView image_view) {
    if (image_view == VK_NULL_HANDLE) return;
    if (m_real_count.load(std::memory_order_relaxed) == 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_real.begin(); it != m_real.end();) {
        const bool matches_device = (it->second.device == device);
        const bool references_view = std::find(it->second.views.begin(), it->second.views.end(), image_view) != it->second.views.end();
        if (matches_device && references_view) {
            PFN_vkDestroyFramebuffer destroy_fn = real_destroy_framebuffer(it->second.device);
            if (destroy_fn != nullptr && it->second.framebuffer != VK_NULL_HANDLE) {
                destroy_fn(it->second.device, it->second.framebuffer, nullptr);
            }
            it = m_real.erase(it);
            m_real_count.fetch_sub(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }
}

void ImagelessFramebufferManager::remove_device(VkDevice device) {
    if (device == VK_NULL_HANDLE) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_emulated_devices.erase(handle_to_u64(device));

    for (auto it = m_real.begin(); it != m_real.end();) {
        if (it->second.device == device) {
            PFN_vkDestroyFramebuffer destroy_fn = real_destroy_framebuffer(device);
            if (destroy_fn != nullptr && it->second.framebuffer != VK_NULL_HANDLE) {
                destroy_fn(device, it->second.framebuffer, nullptr);
            }
            it = m_real.erase(it);
            m_real_count.fetch_sub(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }

    for (auto it = m_records.begin(); it != m_records.end();) {
        if (it->second.device == device) {
            it = m_records.erase(it);
            m_record_count.fetch_sub(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }
}

void ImagelessFramebufferManager::destroy_for_owner_locked(uint64_t owner) {
    for (auto it = m_real.begin(); it != m_real.end();) {
        if (it->second.owner == owner) {
            PFN_vkDestroyFramebuffer destroy_fn = real_destroy_framebuffer(it->second.device);
            if (destroy_fn != nullptr && it->second.framebuffer != VK_NULL_HANDLE) {
                destroy_fn(it->second.device, it->second.framebuffer, nullptr);
            }
            it = m_real.erase(it);
            m_real_count.fetch_sub(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }
}




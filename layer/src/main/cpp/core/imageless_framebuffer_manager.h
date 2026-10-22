#ifndef IMAGELESS_FRAMEBUFFER_MANAGER_H
#define IMAGELESS_FRAMEBUFFER_MANAGER_H

#include "vk_common.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * VK_KHR_imageless_framebuffer / Vulkan 1.2 core emulation.
 *
 * Applications create a framebuffer with VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT and
 * only supply the real attachment image views later, at vkCmdBeginRenderPass via
 * VkRenderPassAttachmentBeginInfo. Drivers without the feature reject that.
 *
 * This manager returns a synthetic VkFramebuffer handle at creation time and,
 * when the render pass begins, builds (and caches) a real framebuffer from the
 * attachment views supplied in VkRenderPassAttachmentBeginInfo. The driver sees
 * a plain, non-imageless framebuffer; the extra pNext structure is left in the
 * chain because Vulkan implementations must skip unknown pNext structures.
 */
class ImagelessFramebufferManager {
public:
    static ImagelessFramebufferManager& get();

    // Per-device switch, set from the Vulkan 1.2 core module.
    void set_device_emulated(VkDevice device, bool emulated);
    bool is_emulated(VkDevice device) const;
    void remove_device(VkDevice device);

    bool is_imageless(VkFramebuffer framebuffer) const;

    // Creates a synthetic handle when pCreateInfo carries the imageless flag.
    VkResult create(VkDevice device, const VkFramebufferCreateInfo* pCreateInfo, VkFramebuffer* pFramebuffer);

    // Returns true when `framebuffer` was a synthetic handle and was consumed.
    bool destroy(VkDevice device, VkFramebuffer framebuffer);

    // Substitutes the real framebuffer in `out`.
    enum class ResolveResult {
        NotImageless,  // ordinary framebuffer: leave the call untouched
        Resolved,      // `out` holds the real framebuffer
        Failed,        // synthetic handle but no usable attachment views: do not call the driver
    };
    ResolveResult resolve(const VkRenderPassBeginInfo* pBegin, VkRenderPassBeginInfo& out);

    void on_destroy_image_view(VkDevice device, VkImageView image_view);

private:
    ImagelessFramebufferManager() = default;
    ~ImagelessFramebufferManager() = default;
    ImagelessFramebufferManager(const ImagelessFramebufferManager&) = delete;
    ImagelessFramebufferManager& operator=(const ImagelessFramebufferManager&) = delete;

    static constexpr size_t kMaxCachedFramebuffers = 1024;
    struct Record {
        VkDevice device = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 0;
        uint32_t attachmentCount = 0;
    };

    struct Key {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 0;
        std::vector<VkImageView> views;

        bool operator==(const Key& other) const {
            return renderPass == other.renderPass && width == other.width && height == other.height &&
                   layers == other.layers && views == other.views;
        }
    };

    struct KeyHash {
        size_t operator()(const Key& key) const;
    };

    struct RealFramebuffer {
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint64_t owner = 0;
        std::vector<VkImageView> views;
    };

    void destroy_for_owner_locked(uint64_t owner);

    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, Record> m_records;
    std::unordered_map<Key, RealFramebuffer, KeyHash> m_real;
    std::unordered_set<uint64_t> m_emulated_devices;
    std::atomic<uint64_t> m_next_handle{0x1A000000ull};
    // Fast paths: render pass begin and image view destruction must not take the
    // mutex when the feature is unused.
    std::atomic<uint32_t> m_record_count{0};
    std::atomic<uint32_t> m_real_count{0};
};

#endif  // IMAGELESS_FRAMEBUFFER_MANAGER_H

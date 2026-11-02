#pragma once
#include "vk_common.h"
#include "device_object_key.h"
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace vkfix {
// Immutable application render-pass state. No pointer in this snapshot refers
// to application memory; pipelines/command buffers may retain it independently
// of the application's VkRenderPass handle.
struct MultiviewPassPlan {
    struct Subpass {
        VkSubpassDescriptionFlags flags = 0;
        VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
        uint32_t view_mask = 0;
        std::vector<VkAttachmentReference> inputs, colors, resolves;
        std::optional<VkAttachmentReference> depth;
        std::vector<uint32_t> preserves;
        std::vector<VkImageAspectFlags> input_aspects;
    };
    struct Dependency {
        VkSubpassDependency info{};
        int32_t view_offset = 0;
    };
    struct Lifetime {
        std::array<uint32_t, 32> first, last;
        Lifetime() { first.fill(VK_SUBPASS_EXTERNAL); last.fill(VK_SUBPASS_EXTERNAL); }
    };
    // One native, non-multiview subpass. Original attachment indices map the
    // framebuffer views and clear values to this compact attachment list.
    struct Chunk {
        std::vector<VkAttachmentDescription> attachments;
        std::vector<uint32_t> original_attachments;
        std::vector<VkAttachmentReference> inputs, colors, resolves;
        std::optional<VkAttachmentReference> depth;
        std::vector<VkSubpassDependency> dependencies;
        std::vector<VkInputAttachmentAspectReference> input_aspects;
        VkSubpassDescription subpass{};
        VkRenderPassInputAttachmentAspectCreateInfo aspects{VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO};
        VkRenderPassCreateInfo info(bool native_input_aspects);
        std::vector<VkClearValue> clear_values(uint32_t count, const VkClearValue* original) const;
    };
    VkRenderPassCreateFlags flags = 0;
    uint32_t all_views = 0;
    bool core_only = true;
    std::vector<VkAttachmentDescription> attachments;
    std::vector<Subpass> subpasses;
    std::vector<Dependency> dependencies;
    std::vector<uint32_t> correlation_masks;
    std::vector<Lifetime> lifetimes;

    static std::shared_ptr<const MultiviewPassPlan> capture(const VkRenderPassCreateInfo* info);
    static std::shared_ptr<const MultiviewPassPlan> capture(const VkRenderPassCreateInfo2* info);
    uint32_t source_views(const Dependency& dependency, uint32_t destination_view) const;
    // first/last refer to chunks of one logical subpass, not the entire pass.
    // Only the last chunk resolves multisampled attachments. Non-final chunks
    // store their contents so barrier-driven splits do not discard them.
    Chunk chunk(uint32_t subpass, uint32_t view, bool first, bool last,
        const std::vector<uint32_t>& manual_layouts = {}) const;
    VkImageLayout attachment_layout(uint32_t attachment, uint32_t subpass) const;
private:
    void finish();
    VkImageLayout previous_layout(uint32_t attachment, uint32_t subpass, uint32_t view) const;
};

// Owns the internal native render passes independently of the public handle.
// Native resources are lazy: tracking native multiview alone creates none.
class MultiviewPassResources {
public:
    MultiviewPassResources(VkDevice device, std::shared_ptr<const MultiviewPassPlan> plan, bool emulated = false);
    ~MultiviewPassResources();
    std::shared_ptr<const MultiviewPassPlan> plan() const { return plan_; }
    bool emulated() const { return emulated_; }
    VkResult create_base(const VkAllocationCallbacks* allocator, bool native_input_aspects, VkRenderPass& result);
    VkResult native_pass(uint32_t subpass, uint32_t view, bool first, bool last,
                         bool native_input_aspects, VkRenderPass& result, const std::vector<uint32_t>& manual_layouts = {});
    void release();
private:
    std::mutex mutex_;
    VkDevice device_;
    std::shared_ptr<const MultiviewPassPlan> plan_;
    bool emulated_;
    PFN_vkCreateRenderPass create_ = nullptr;
    PFN_vkDestroyRenderPass destroy_ = nullptr;
    std::map<std::tuple<uint32_t, uint32_t, bool, bool, bool, std::vector<uint32_t>>, VkRenderPass> native_;
};

class MultiviewRenderPasses {
public:
    static MultiviewRenderPasses& get();
    void remember(VkDevice device, VkRenderPass pass, const VkRenderPassCreateInfo* info);
    void remember(VkDevice device, VkRenderPass pass, const VkRenderPassCreateInfo2* info);
    bool create_fallback(VkDevice device, const VkRenderPassCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkRenderPass* pass, bool native_input_aspects, VkResult& result);
    bool create_fallback(VkDevice device, const VkRenderPassCreateInfo2* info,
        const VkAllocationCallbacks* allocator, VkRenderPass* pass, bool native_input_aspects, VkResult& result);
    std::shared_ptr<MultiviewPassResources> acquire(VkDevice device, VkRenderPass pass);
    void forget(VkDevice device, VkRenderPass pass);
    // Also releases resources retained by a pipeline/command buffer whose
    // application render-pass handle was already destroyed.
    void forget_device(VkDevice device);
private:
    bool create_fallback(VkDevice device, std::shared_ptr<const MultiviewPassPlan> plan,
        const VkAllocationCallbacks* allocator, VkRenderPass* pass, bool native_input_aspects, VkResult& result);
    void remember_resources(VkDevice device, VkRenderPass pass, std::shared_ptr<MultiviewPassResources> resources);
    void remember(VkDevice device, VkRenderPass pass, std::shared_ptr<const MultiviewPassPlan> plan);
    std::mutex mutex_;
    std::unordered_map<DeviceObjectKey, std::shared_ptr<MultiviewPassResources>, DeviceObjectKeyHash> passes_;
    std::map<VkDevice, std::vector<std::weak_ptr<MultiviewPassResources>>> live_;
};
}

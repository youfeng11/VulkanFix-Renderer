#pragma once
#include "multiview_attachments.h"
#include <set>

namespace vkfix {
class MultiviewDescriptorArena;
struct MultiviewDescriptorLayout {
    struct Binding {
        VkDescriptorSetLayoutBinding info{};
        std::vector<VkSampler> immutable;
    };
    bool core_only = true, has_inputs = false, has_images = false;
    std::vector<Binding> bindings; // Sorted by binding, including reserved entries.
    explicit MultiviewDescriptorLayout(const VkDescriptorSetLayoutCreateInfo&);
};
struct MultiviewDescriptorSet {
    struct Input {
        std::shared_ptr<MultiviewViewResources> view;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    VkDevice device{};
    VkDescriptorSet handle{};
    VkDescriptorPool pool{};
    std::shared_ptr<const MultiviewDescriptorLayout> layout;
    std::map<uint32_t, std::vector<Input>> inputs;
    std::vector<VkDescriptorImageInfo> image_scratch;
    uint64_t revision = 0;
    bool alive = true, core_only = true, tracked = true;
};
using MultiviewInputSelection = std::vector<std::pair<uint32_t, uint32_t>>; // binding, array element

class MultiviewDescriptors {
public:
    static MultiviewDescriptors& get();
    void remember_layout(VkDevice, VkDescriptorSetLayout, const VkDescriptorSetLayoutCreateInfo&);
    void forget_layout(VkDevice, VkDescriptorSetLayout);
    void remember_pool(VkDevice, VkDescriptorPool, const VkDescriptorPoolCreateInfo&);
    void forget_pool(VkDevice, VkDescriptorPool, bool destroyed = false);
    void forget_sets(VkDevice, uint32_t, const VkDescriptorSet*);
    // All metadata nodes are allocated BEFORE calling the driver. Rolling back
    // successful allocation with vkFreeDescriptorSets is illegal for some pools.
    VkResult allocate(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*, PFN_vkAllocateDescriptorSets);
    void update(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*);
    // Scratch is reserved before the native set allocation. A void update
    // must not need a new allocation to translate virtual volume layouts.
    bool update_native_layouts(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t,
                               const VkCopyDescriptorSet*, PFN_vkUpdateDescriptorSets);
    void remember_template(VkDevice, VkDescriptorUpdateTemplate, const VkDescriptorUpdateTemplateCreateInfo&);
    void forget_template(VkDevice, VkDescriptorUpdateTemplate);
    void update_template(VkDevice, VkDescriptorSet, VkDescriptorUpdateTemplate, const void*);
    void forget_device(VkDevice);
private:
    friend class MultiviewDescriptorArena;
    struct Less {
        bool operator()(const DeviceObjectKey& a, const DeviceObjectKey& b) const {
            return a.device == b.device ? a.handle < b.handle : std::less<VkDevice>{}(a.device, b.device);
        }
    };
    struct Template { std::vector<VkDescriptorUpdateTemplateEntry> entries; };
    void write_locked(VkDevice, MultiviewDescriptorSet&, uint32_t binding, uint32_t element,
        uint32_t count, const VkDescriptorImageInfo*, size_t stride);
    static void revised(MultiviewDescriptorSet&);
    void retire_locked(const std::shared_ptr<MultiviewDescriptorSet>&);
    std::mutex mutex_;
    std::map<DeviceObjectKey, std::shared_ptr<const MultiviewDescriptorLayout>, Less> layouts_;
    std::map<DeviceObjectKey, VkDescriptorPoolCreateFlags, Less> pools_;
    std::map<DeviceObjectKey, std::shared_ptr<MultiviewDescriptorSet>, Less> sets_;
    std::map<DeviceObjectKey, Template, Less> templates_;
    std::atomic<uint32_t> set_count_{0};
    std::map<VkDevice, std::set<MultiviewDescriptorArena*>> arenas_;
};

// One arena belongs to one command-buffer recording. Core descriptor updates
// invalidate recordings using that set; immutable snapshots can therefore be
// reused within the recording. Reset/free the arena with the command buffer.
class MultiviewDescriptorArena {
public:
    ~MultiviewDescriptorArena();
    VkResult select(VkDevice, VkDescriptorSet source, uint32_t view,
        const MultiviewInputSelection& used_inputs, VkDescriptorSet& result);
    VkResult select_all(VkDevice, VkDescriptorSet source, uint32_t view, VkDescriptorSet& result, bool whole = false);
    void release();
private:
    friend class MultiviewDescriptors;
    void release_locked();
    void invalidate_locked(const std::shared_ptr<MultiviewDescriptorSet>&);
    VkResult select_locked(VkDevice, VkDescriptorSet, uint32_t, const MultiviewInputSelection&, VkDescriptorSet&, bool whole = false);
    struct Native {
        std::shared_ptr<MultiviewDescriptorSet> source;
        uint64_t revision;
        uint32_t view;
        MultiviewInputSelection inputs;
        VkDescriptorPool pool;
        VkDescriptorSet handle;
        bool whole;
        const AttachmentReplayScope::Bindings* attachments;
    };
    std::mutex mutex_;
    VkDevice device_{};
    PFN_vkDestroyDescriptorPool destroy_pool_{};
    std::vector<Native> native_;
    bool retired_ = false;
};
}

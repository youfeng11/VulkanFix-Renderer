#include "multiview_descriptors.h"
#include "driver_loader.h"
#include "layout_compat.h"
#include "volume_attachment.h"
#include <algorithm>
#include <limits>

namespace vkfix {
namespace {
template<class T> T proc(VkDevice device, const char* name) {
    return reinterpret_cast<T>(get_real_proc(get_last_instance(), device, name));
}
bool image_descriptor(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
        type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
// Advance in binding-number order, skipping reserved and absent bindings.
// Valid consecutive writes/copies already require matching binding properties.
template<class Map> struct Cursor {
    typename Map::iterator it, end;
    uint32_t element;
    Cursor(Map& map, uint32_t binding, uint32_t offset) : it(map.find(binding)), end(map.end()), element(offset) { skip(); }
    void skip() { while (it != end && element >= it->second.size()) { element -= uint32_t(it->second.size()); ++it; } }
    auto* value() { return it == end ? nullptr : &it->second[element]; }
    void next() { ++element; skip(); }
};
}
MultiviewDescriptorLayout::MultiviewDescriptorLayout(const VkDescriptorSetLayoutCreateInfo& info) {
    core_only = !info.flags;
    for (auto p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            const auto& flags = *reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(p);
            for (uint32_t i = 0; i < flags.bindingCount; ++i) if (flags.pBindingFlags[i]) core_only = false;
        } else core_only = false;
    }
    for (uint32_t i = 0; i < info.bindingCount; ++i) {
        Binding b; b.info = info.pBindings[i]; b.info.pImmutableSamplers = nullptr;
        has_inputs |= b.info.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT && b.info.descriptorCount;
        has_images |= image_descriptor(b.info.descriptorType) && b.info.descriptorCount;
        if (b.info.descriptorCount)
            core_only &= b.info.descriptorType >= VK_DESCRIPTOR_TYPE_SAMPLER && b.info.descriptorType <= VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        if (b.info.descriptorCount && info.pBindings[i].pImmutableSamplers &&
            (b.info.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || b.info.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
            b.immutable.assign(info.pBindings[i].pImmutableSamplers, info.pBindings[i].pImmutableSamplers + b.info.descriptorCount);
        bindings.push_back(std::move(b));
    }
    std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return a.info.binding < b.info.binding; });
}
MultiviewDescriptors& MultiviewDescriptors::get() { static MultiviewDescriptors registry; return registry; }
void MultiviewDescriptors::remember_layout(VkDevice device, VkDescriptorSetLayout handle, const VkDescriptorSetLayoutCreateInfo& info) {
    auto record = std::make_shared<MultiviewDescriptorLayout>(info);
    std::lock_guard<std::mutex> lock(mutex_);
    layouts_[DeviceObjectKey::make(device, handle)] = std::move(record);
}
void MultiviewDescriptors::forget_layout(VkDevice device, VkDescriptorSetLayout handle) {
    std::lock_guard<std::mutex> lock(mutex_); layouts_.erase(DeviceObjectKey::make(device, handle));
}
void MultiviewDescriptors::remember_pool(VkDevice device, VkDescriptorPool pool, const VkDescriptorPoolCreateInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_); pools_[DeviceObjectKey::make(device, pool)] = info.flags;
}
void MultiviewDescriptors::forget_pool(VkDevice device, VkDescriptorPool pool, bool destroyed) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Keep flags across reset; creation of a reused handle replaces them.
    for (auto it = sets_.begin(); it != sets_.end();) {
        if (it->first.device == device && it->second->pool == pool) { retire_locked(it->second); it = sets_.erase(it); }
        else ++it;
    }
    set_count_.store(uint32_t(sets_.size()), std::memory_order_release);
    if (destroyed) pools_.erase(DeviceObjectKey::make(device, pool));
}
void MultiviewDescriptors::forget_sets(VkDevice device, uint32_t count, const VkDescriptorSet* sets) {
    if (!set_count_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count; ++i) {
        const auto found = sets_.find(DeviceObjectKey::make(device, sets[i]));
        if (found != sets_.end()) { retire_locked(found->second); sets_.erase(found); }
    }
    set_count_.store(uint32_t(sets_.size()), std::memory_order_release);
}
VkResult MultiviewDescriptors::allocate(VkDevice device, const VkDescriptorSetAllocateInfo* info,
    VkDescriptorSet* sets, PFN_vkAllocateDescriptorSets allocate) {
    if (!allocate) return VK_ERROR_INITIALIZATION_FAILED;
    if (!info || !sets) return allocate(device, info, sets);
    try {
        decltype(sets_) prepared;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto pool = pools_.find(DeviceObjectKey::make(device, info->descriptorPool));
            const bool core_pool = pool == pools_.end() || !(pool->second & ~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
            for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
                const auto layout = layouts_.find(DeviceObjectKey::make(device, info->pSetLayouts[i]));
                if (layout == layouts_.end() || (!layout->second->has_inputs && !layout->second->has_images)) continue;
                auto record = std::make_shared<MultiviewDescriptorSet>();
                record->device = device; record->pool = info->descriptorPool; record->layout = layout->second;
                record->core_only = core_pool && !info->pNext && layout->second->core_only;
                for (const auto& b : layout->second->bindings)
                    if (b.info.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
                        record->inputs[b.info.binding].resize(b.info.descriptorCount);
                size_t images = 0;
                for (const auto& b : layout->second->bindings) if (image_descriptor(b.info.descriptorType)) images += b.info.descriptorCount;
                record->image_scratch.resize(images);
                prepared.emplace(DeviceObjectKey{device, i}, std::move(record));
            }
        }
        const auto status = allocate(device, info, sets);
        if (status != VK_SUCCESS) return status;
        std::lock_guard<std::mutex> lock(mutex_);
        while (!prepared.empty()) {
            auto node = prepared.extract(prepared.begin());
            const auto set = sets[node.key().handle];
            node.key() = DeviceObjectKey::make(device, set); node.mapped()->handle = set;
            auto inserted = sets_.insert(std::move(node)); // Transfer preallocated nodes, no host allocation.
            if (!inserted.inserted) { retire_locked(inserted.position->second); inserted.position->second = std::move(inserted.node.mapped()); }
        }
        set_count_.store(uint32_t(sets_.size()), std::memory_order_release);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        for (uint32_t i = 0; i < info->descriptorSetCount; ++i) sets[i] = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void MultiviewDescriptors::revised(MultiviewDescriptorSet& set) { if (!++set.revision) set.tracked = false; }
bool MultiviewDescriptors::update_native_layouts(VkDevice device, uint32_t wc, const VkWriteDescriptorSet* writes,
    uint32_t cc, const VkCopyDescriptorSet* copies, PFN_vkUpdateDescriptorSets update) {
    if (!update) return false;
    if (!wc || std::none_of(writes,writes + wc,[](const auto& write) { return image_descriptor(write.descriptorType) && write.descriptorCount; })) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // Validate ownership before issuing any part of the update batch.
    for (uint32_t w = 0; w < wc; ++w) if (image_descriptor(writes[w].descriptorType) && writes[w].descriptorCount) {
        const auto found = sets_.find(DeviceObjectKey::make(device,writes[w].dstSet));
        if (found == sets_.end() || !found->second->alive || found->second->image_scratch.size() < writes[w].descriptorCount) return false;
    }
    for (uint32_t w = 0; w < wc; ++w) {
        auto native = writes[w];
        if (image_descriptor(native.descriptorType) && native.descriptorCount) {
            auto& scratch = sets_.find(DeviceObjectKey::make(device,native.dstSet))->second->image_scratch;
            std::memcpy(scratch.data(),native.pImageInfo,sizeof(VkDescriptorImageInfo) * native.descriptorCount);
            for (uint32_t i = 0; i < native.descriptorCount; ++i)
                scratch[i].imageLayout = attachment_descriptor_layout(device,scratch[i].imageView,scratch[i].imageLayout);
            native.pImageInfo = scratch.data();
        }
        update(device,1,&native,0,nullptr);
    }
    if (cc) update(device,0,nullptr,cc,copies);
    return true;
}
void MultiviewDescriptors::retire_locked(const std::shared_ptr<MultiviewDescriptorSet>& set) {
    set->alive = false;
    const auto found = arenas_.find(set->device);
    if (found != arenas_.end()) for (auto* arena : found->second) {
        std::lock_guard<std::mutex> lock(arena->mutex_);
        arena->invalidate_locked(set);
    }
}
void MultiviewDescriptors::write_locked(VkDevice device, MultiviewDescriptorSet& set,
    uint32_t binding, uint32_t element, uint32_t count, const VkDescriptorImageInfo* images, size_t stride) {
    Cursor cursor(set.inputs, binding, element);
    for (uint32_t i = 0; i < count; ++i, cursor.next()) {
        auto* target = cursor.value();
        if (!target) { set.tracked = false; return; }
        VkDescriptorImageInfo image{};
        std::memcpy(&image, reinterpret_cast<const char*>(images) + size_t(i) * stride, sizeof(image));
        target->view = MultiviewAttachments::get().acquire_view(device, image.imageView);
        target->layout = image.imageLayout;
    }
}
void MultiviewDescriptors::update(VkDevice device, uint32_t wc, const VkWriteDescriptorSet* writes,
    uint32_t cc, const VkCopyDescriptorSet* copies) {
    if (!set_count_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < wc; ++i) {
        const auto& w = writes[i]; const auto found = sets_.find(DeviceObjectKey::make(device, w.dstSet));
        if (found == sets_.end()) continue;
        revised(*found->second);
        if (w.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
            write_locked(device, *found->second, w.dstBinding, w.dstArrayElement, w.descriptorCount, w.pImageInfo, sizeof(VkDescriptorImageInfo));
    }
    for (uint32_t i = 0; i < cc; ++i) {
        const auto& c = copies[i]; const auto target = sets_.find(DeviceObjectKey::make(device, c.dstSet));
        if (target == sets_.end()) continue;
        auto& dst = *target->second; revised(dst);
        if (!dst.inputs.count(c.dstBinding)) continue;
        const auto source = sets_.find(DeviceObjectKey::make(device, c.srcSet));
        if (source == sets_.end()) { dst.tracked = false; continue; }
        Cursor out(dst.inputs, c.dstBinding, c.dstArrayElement);
        Cursor in(source->second->inputs, c.srcBinding, c.srcArrayElement);
        for (uint32_t n = 0; n < c.descriptorCount; ++n, out.next(), in.next()) {
            if (!out.value() || !in.value()) { dst.tracked = false; break; }
            *out.value() = *in.value();
        }
    }
}
void MultiviewDescriptors::remember_template(VkDevice device, VkDescriptorUpdateTemplate handle,
    const VkDescriptorUpdateTemplateCreateInfo& info) {
    if (info.templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) return;
    Template record;
    if (info.descriptorUpdateEntryCount) record.entries.assign(info.pDescriptorUpdateEntries, info.pDescriptorUpdateEntries + info.descriptorUpdateEntryCount);
    std::lock_guard<std::mutex> lock(mutex_); templates_[DeviceObjectKey::make(device, handle)] = std::move(record);
}
void MultiviewDescriptors::forget_template(VkDevice device, VkDescriptorUpdateTemplate handle) {
    std::lock_guard<std::mutex> lock(mutex_); templates_.erase(DeviceObjectKey::make(device, handle));
}
void MultiviewDescriptors::update_template(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate handle, const void* data) {
    if (!set_count_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sets_.find(DeviceObjectKey::make(device, set));
    if (found == sets_.end()) return;
    auto& record = *found->second; revised(record);
    const auto tmpl = templates_.find(DeviceObjectKey::make(device, handle));
    if (tmpl == templates_.end()) { record.tracked = false; return; }
    for (const auto& e : tmpl->second.entries) if (e.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT && e.descriptorCount)
        write_locked(device, record, e.dstBinding, e.dstArrayElement, e.descriptorCount,
            reinterpret_cast<const VkDescriptorImageInfo*>(static_cast<const char*>(data) + e.offset), e.stride);
}
void MultiviewDescriptors::forget_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto arenas = arenas_.find(device);
    if (arenas != arenas_.end()) {
        for (auto* arena : arenas->second) {
            std::lock_guard<std::mutex> arena_lock(arena->mutex_);
            arena->release_locked();
        }
        arenas_.erase(arenas);
    }
    for (auto& set : sets_) if (set.first.device == device) set.second->alive = false;
    auto erase = [device](auto& map) {
        for (auto it = map.begin(); it != map.end();) if (it->first.device == device) it = map.erase(it); else ++it;
    };
    erase(sets_); erase(layouts_); erase(pools_); erase(templates_);
    set_count_.store(uint32_t(sets_.size()), std::memory_order_release);
}

MultiviewDescriptorArena::~MultiviewDescriptorArena() { release(); }
void MultiviewDescriptorArena::release() {
    auto& registry = MultiviewDescriptors::get();
    std::lock_guard<std::mutex> state_lock(registry.mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = registry.arenas_.find(device_);
    if (found != registry.arenas_.end()) {
        found->second.erase(this);
        if (found->second.empty()) registry.arenas_.erase(found);
    }
    release_locked();
}
void MultiviewDescriptorArena::release_locked() {
    if (device_ && destroy_pool_) for (const auto& record : native_) destroy_pool_(device_, record.pool, nullptr);
    native_.clear(); device_ = VK_NULL_HANDLE; retired_ = true;
}
void MultiviewDescriptorArena::invalidate_locked(const std::shared_ptr<MultiviewDescriptorSet>& source) {
    // Free/reset invalidates the recording that used the public set. Retire
    // private sets now, before the application can destroy immutable samplers
    // whose last public descriptor set/pool has just been freed.
    for (auto it = native_.begin(); it != native_.end();) {
        if (it->source == source) {
            if (device_ && destroy_pool_) destroy_pool_(device_, it->pool, nullptr);
            it = native_.erase(it);
        } else ++it;
    }
}
VkResult MultiviewDescriptorArena::select(VkDevice device, VkDescriptorSet source, uint32_t view,
    const MultiviewInputSelection& used, VkDescriptorSet& result) {
    result = VK_NULL_HANDLE;
    auto& registry = MultiviewDescriptors::get();
    std::lock_guard<std::mutex> state_lock(registry.mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    return select_locked(device, source, view, used, result);
}
VkResult MultiviewDescriptorArena::select_all(VkDevice device, VkDescriptorSet source, uint32_t view, VkDescriptorSet& result, bool whole) {
    auto& registry = MultiviewDescriptors::get();
    std::lock_guard<std::mutex> state_lock(registry.mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        MultiviewInputSelection selection;
        const auto found = registry.sets_.find(DeviceObjectKey::make(device, source));
        if (found != registry.sets_.end())
            for (const auto& binding : found->second->inputs)
                for (uint32_t i = 0; i < binding.second.size(); ++i) selection.emplace_back(binding.first, i);
        return select_locked(device, source, view, selection, result,whole);
    } catch (const std::bad_alloc&) { result = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult MultiviewDescriptorArena::select_locked(VkDevice device, VkDescriptorSet source, uint32_t view,
    const MultiviewInputSelection& used, VkDescriptorSet& result, bool whole) {
    result = VK_NULL_HANDLE;
    auto& registry = MultiviewDescriptors::get();
    if (retired_ || (device_ && device_ != device) || view >= 32) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (used.empty()) { result = source; return VK_SUCCESS; }
    // Registry lock protects revisions, all selected descriptor values and
    // source-set retirement during capture. Driver calls below do not reenter it.
    const auto found = registry.sets_.find(DeviceObjectKey::make(device, source));
    if (found == registry.sets_.end()) return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto& set = found->second;
    if (!set->alive || !set->tracked || !set->core_only) return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        auto selection = used; std::sort(selection.begin(), selection.end());
        selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
        for (const auto& cached : native_)
            if (cached.source == set && cached.revision == set->revision && cached.view == view && cached.inputs == selection && cached.whole == whole &&
                cached.attachments == AttachmentReplayScope::current()) {
                result = cached.handle; return VK_SUCCESS;
            }
        auto create_layout = proc<PFN_vkCreateDescriptorSetLayout>(device, "vkCreateDescriptorSetLayout");
        auto destroy_layout = proc<PFN_vkDestroyDescriptorSetLayout>(device, "vkDestroyDescriptorSetLayout");
        auto create_pool = proc<PFN_vkCreateDescriptorPool>(device, "vkCreateDescriptorPool");
        auto allocate = proc<PFN_vkAllocateDescriptorSets>(device, "vkAllocateDescriptorSets");
        auto update = proc<PFN_vkUpdateDescriptorSets>(device, "vkUpdateDescriptorSets");
        destroy_pool_ = proc<PFN_vkDestroyDescriptorPool>(device, "vkDestroyDescriptorPool");
        if (!create_layout || !destroy_layout || !create_pool || !destroy_pool_ || !allocate || !update) return VK_ERROR_INITIALIZATION_FAILED;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::map<VkDescriptorType, uint64_t> counts;
        std::vector<VkCopyDescriptorSet> copies;
        for (const auto& b : set->layout->bindings) {
            auto info = b.info; info.pImmutableSamplers = b.immutable.empty() ? nullptr : b.immutable.data();
            bindings.push_back(info);
            if (!info.descriptorCount) continue;
            counts[info.descriptorType] += info.descriptorCount;
            if (info.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && !b.immutable.empty()) continue;
            VkCopyDescriptorSet c{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
            c.srcSet = source; c.srcBinding = c.dstBinding = info.binding; c.descriptorCount = info.descriptorCount;
            copies.push_back(c);
        }
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkWriteDescriptorSet> writes;
        images.reserve(selection.size()); writes.reserve(selection.size());
        for (const auto& selected : selection) {
            const auto binding = set->inputs.find(selected.first);
            if (binding == set->inputs.end() || selected.second >= binding->second.size()) return VK_ERROR_FEATURE_NOT_PRESENT;
            const auto& input = binding->second[selected.second];
            // Undefined descriptors may be copied. select_all also visits
            // unused inputs: a statically accessed Core input must match the
            // framebuffer view, so it necessarily covers each active view.
            // Leave incompatible unused inputs unchanged instead of rejecting
            // an otherwise valid binding of a layout with unused entries.
            if (!input.view || !input.view->covers_view(view)) continue;
            if (input.view->image() && input.view->image()->emulated_attachment && !AttachmentReplayScope::lookup(input.view.get())) continue;
            VkImageView slice{};
            const auto status = whole ? input.view->whole(0,slice) : input.view->slice(view, 0, slice);
            if (status != VK_SUCCESS) return status;
            auto layout = input.layout;
            if (LayoutCompat::get().is_emulated(device)) layout = LayoutCompat::sanitize(layout);
            images.push_back({VK_NULL_HANDLE, slice, layout});
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = selected.first; w.dstArrayElement = selected.second; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; w.pImageInfo = &images.back();
            writes.push_back(w);
        }
        std::vector<VkDescriptorPoolSize> sizes;
        for (const auto& count : counts) {
            if (count.second > UINT32_MAX) return VK_ERROR_OUT_OF_POOL_MEMORY;
            sizes.push_back({count.first, uint32_t(count.second)});
        }
        // Reserve the final ownership slot before any pool is allocated.
        native_.reserve(native_.size() + 1);
        registry.arenas_[device].insert(this);
        device_ = device;
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = uint32_t(bindings.size()); lci.pBindings = bindings.data();
        VkDescriptorSetLayout layout{};
        auto status = create_layout(device, &lci, nullptr, &layout);
        if (status != VK_SUCCESS) return status;
        VkDescriptorPool pool{}; VkDescriptorSet shadow{};
        for (;;) {
            VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pci.maxSets = 1; pci.poolSizeCount = uint32_t(sizes.size()); pci.pPoolSizes = sizes.data();
            status = create_pool(device, &pci, nullptr, &pool);
            if (status != VK_SUCCESS) break;
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
            status = allocate(device, &ai, &shadow);
            if (status == VK_SUCCESS) break;
            destroy_pool_(device, pool, nullptr); pool = VK_NULL_HANDLE;
            // A YCbCr immutable combined sampler can consume multiple native
            // pool descriptors. Grow capacity rather than assuming a factor.
            bool grew = false;
            if (status == VK_ERROR_OUT_OF_POOL_MEMORY || status == VK_ERROR_FRAGMENTED_POOL)
                for (auto& size : sizes) if (size.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && size.descriptorCount <= UINT32_MAX / 2) {
                    size.descriptorCount *= 2; grew = true;
                }
            if (!grew) break;
        }
        destroy_layout(device, layout, nullptr);
        if (status != VK_SUCCESS) return status;
        for (auto& c : copies) c.dstSet = shadow;
        for (auto& w : writes) w.dstSet = shadow;
        // Vulkan applies writes before copies WITHIN a call, so the initial
        // native copy and the selected input-view overrides are separate calls.
        if (!copies.empty()) update(device, 0, nullptr, uint32_t(copies.size()), copies.data());
        if (!writes.empty()) update(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
        device_ = device;
        native_.push_back({set, set->revision, view, std::move(selection), pool, shadow,whole,AttachmentReplayScope::current()});
        result = shadow; return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
}

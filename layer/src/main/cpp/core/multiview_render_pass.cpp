#include "multiview_render_pass.h"
#include "driver_loader.h"
#include "layout_compat.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace vkfix {
namespace {
template<class T> std::vector<T> copy(uint32_t count, const T* data) {
    if (!count) return {};
    if (!data) throw std::invalid_argument("missing render-pass array");
    return {data, data + count};
}
template<class F> void references(const MultiviewPassPlan::Subpass& subpass, bool resolves, F visit) {
    for (const auto& ref : subpass.inputs) visit(ref);
    for (const auto& ref : subpass.colors) visit(ref);
    if (subpass.depth) visit(*subpass.depth);
    if (resolves) for (const auto& ref : subpass.resolves) visit(ref);
}
bool used(const MultiviewPassPlan::Subpass& subpass, uint32_t attachment, bool resolves = true) {
    bool result = false;
    references(subpass, resolves, [&](const VkAttachmentReference& ref) { result |= ref.attachment == attachment; });
    return result;
}
VkImageLayout layout(const MultiviewPassPlan::Subpass& subpass, uint32_t attachment) {
    VkImageLayout result = VK_IMAGE_LAYOUT_UNDEFINED;
    references(subpass, true, [&](const VkAttachmentReference& ref) {
        if (ref.attachment == attachment) result = ref.layout;
    });
    return result;
}
void sanitize(MultiviewPassPlan::Chunk& chunk) {
    for (auto& attachment : chunk.attachments) {
        attachment.initialLayout = LayoutCompat::sanitize(attachment.initialLayout);
        attachment.finalLayout = LayoutCompat::sanitize(attachment.finalLayout);
    }
    for (auto& ref : chunk.inputs) ref.layout = LayoutCompat::sanitize(ref.layout);
    for (auto& ref : chunk.colors) ref.layout = LayoutCompat::sanitize(ref.layout);
    for (auto& ref : chunk.resolves) ref.layout = LayoutCompat::sanitize(ref.layout);
    if (chunk.depth) chunk.depth->layout = LayoutCompat::sanitize(chunk.depth->layout);
}
}

std::shared_ptr<const MultiviewPassPlan> MultiviewPassPlan::capture(const VkRenderPassCreateInfo* info) {
    if (!info) return {};
    const VkRenderPassMultiviewCreateInfo* multiview = nullptr;
    const VkRenderPassInputAttachmentAspectCreateInfo* aspects = nullptr;
    bool core = true;
    std::set<const void*> visited;
    for (auto node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext) {
        if (!visited.insert(node).second) throw std::invalid_argument("cyclic render-pass pNext chain");
        if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)
            multiview = reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(node);
        else if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO)
            aspects = reinterpret_cast<const VkRenderPassInputAttachmentAspectCreateInfo*>(node);
        else core = false;
    }
    if (!multiview || !multiview->subpassCount) return {};
    uint32_t all_views = 0;
    if (!multiview->pViewMasks || multiview->subpassCount != info->subpassCount)
        throw std::invalid_argument("invalid multiview subpass masks");
    for (uint32_t i = 0; i < multiview->subpassCount; ++i) all_views |= multiview->pViewMasks[i];
    if (!all_views) return {};
    auto plan = std::make_shared<MultiviewPassPlan>();
    plan->flags = info->flags; plan->core_only = core && !info->flags;
    plan->attachments = copy(info->attachmentCount, info->pAttachments);
    if (!info->pSubpasses) throw std::invalid_argument("missing subpass descriptions");
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        const auto& src = info->pSubpasses[i];
        Subpass dst;
        dst.flags = src.flags; dst.bind_point = src.pipelineBindPoint;
        dst.view_mask = multiview->pViewMasks[i];
        dst.inputs = copy(src.inputAttachmentCount, src.pInputAttachments);
        dst.colors = copy(src.colorAttachmentCount, src.pColorAttachments);
        if (src.pResolveAttachments) dst.resolves = copy(src.colorAttachmentCount, src.pResolveAttachments);
        if (src.pDepthStencilAttachment) dst.depth = *src.pDepthStencilAttachment;
        dst.preserves = copy(src.preserveAttachmentCount, src.pPreserveAttachments);
        dst.input_aspects.resize(src.inputAttachmentCount, 0);
        plan->core_only &= !src.flags;
        plan->subpasses.push_back(std::move(dst));
    }
    if (multiview->dependencyCount && (multiview->dependencyCount != info->dependencyCount || !multiview->pViewOffsets))
        throw std::invalid_argument("invalid multiview dependency offsets");
    const auto deps = copy(info->dependencyCount, info->pDependencies);
    for (uint32_t i = 0; i < deps.size(); ++i)
        plan->dependencies.push_back({deps[i], multiview->dependencyCount ? multiview->pViewOffsets[i] : 0});
    plan->correlation_masks = copy(multiview->correlationMaskCount, multiview->pCorrelationMasks);
    if (aspects) for (const auto& ref : copy(aspects->aspectReferenceCount, aspects->pAspectReferences)) {
        if (ref.subpass >= plan->subpasses.size() || ref.inputAttachmentIndex >= plan->subpasses[ref.subpass].inputs.size())
            throw std::invalid_argument("input attachment aspect reference is out of range");
        plan->subpasses[ref.subpass].input_aspects[ref.inputAttachmentIndex] = ref.aspectMask;
    }
    plan->finish(); return plan;
}

std::shared_ptr<const MultiviewPassPlan> MultiviewPassPlan::capture(const VkRenderPassCreateInfo2* info) {
    if (!info || !info->subpassCount || !info->pSubpasses) return {};
    uint32_t views = 0;
    for (uint32_t i = 0; i < info->subpassCount; ++i) views |= info->pSubpasses[i].viewMask;
    if (!views) return {};
    auto plan = std::make_shared<MultiviewPassPlan>();
    plan->flags = info->flags; plan->core_only = !info->pNext && !info->flags;
    if (info->attachmentCount && !info->pAttachments) throw std::invalid_argument("missing attachments");
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const auto& a = info->pAttachments[i];
        plan->core_only &= !a.pNext;
        plan->attachments.push_back({a.flags, a.format, a.samples, a.loadOp, a.storeOp,
            a.stencilLoadOp, a.stencilStoreOp, a.initialLayout, a.finalLayout});
    }
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        const auto& src = info->pSubpasses[i];
        Subpass dst;
        dst.flags = src.flags; dst.bind_point = src.pipelineBindPoint; dst.view_mask = src.viewMask;
        auto refs = [&](uint32_t count, const VkAttachmentReference2* data, std::vector<VkImageAspectFlags>* aspects = nullptr) {
            std::vector<VkAttachmentReference> result;
            if (count && !data) throw std::invalid_argument("missing attachment references");
            for (uint32_t r = 0; r < count; ++r) {
                plan->core_only &= !data[r].pNext;
                result.push_back({data[r].attachment, data[r].layout});
                if (aspects) aspects->push_back(data[r].aspectMask);
            }
            return result;
        };
        dst.inputs = refs(src.inputAttachmentCount, src.pInputAttachments, &dst.input_aspects);
        dst.colors = refs(src.colorAttachmentCount, src.pColorAttachments);
        if (src.pResolveAttachments) dst.resolves = refs(src.colorAttachmentCount, src.pResolveAttachments);
        if (src.pDepthStencilAttachment) dst.depth = refs(1, src.pDepthStencilAttachment)[0];
        dst.preserves = copy(src.preserveAttachmentCount, src.pPreserveAttachments);
        plan->core_only &= !src.pNext && !src.flags;
        plan->subpasses.push_back(std::move(dst));
    }
    if (info->dependencyCount && !info->pDependencies) throw std::invalid_argument("missing dependencies");
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        const auto& d = info->pDependencies[i];
        plan->core_only &= !d.pNext;
        plan->dependencies.push_back({{d.srcSubpass, d.dstSubpass, d.srcStageMask, d.dstStageMask,
            d.srcAccessMask, d.dstAccessMask, d.dependencyFlags}, d.viewOffset});
    }
    plan->correlation_masks = copy(info->correlatedViewMaskCount, info->pCorrelatedViewMasks);
    plan->finish(); return plan;
}

void MultiviewPassPlan::finish() {
    lifetimes.resize(attachments.size());
    for (uint32_t s = 0; s < subpasses.size(); ++s) {
        const auto& subpass = subpasses[s]; all_views |= subpass.view_mask;
        if (!subpass.view_mask) throw std::invalid_argument("mixed multiview and ordinary subpasses");
        references(subpass, true, [&](const VkAttachmentReference& ref) {
            if (ref.attachment == VK_ATTACHMENT_UNUSED) return;
            if (ref.attachment >= attachments.size()) throw std::invalid_argument("attachment index is out of range");
            auto& life = lifetimes[ref.attachment];
            for (uint32_t v = 0; v < 32; ++v) if (subpass.view_mask & (uint32_t(1) << v)) {
                if (life.first[v] == VK_SUBPASS_EXTERNAL) life.first[v] = s;
                life.last[v] = s;
            }
        });
    }
}

uint32_t MultiviewPassPlan::source_views(const Dependency& dependency, uint32_t view) const {
    const auto& d = dependency.info;
    if (view >= 32 || d.srcSubpass == VK_SUBPASS_EXTERNAL || d.dstSubpass == VK_SUBPASS_EXTERNAL ||
        d.srcSubpass >= subpasses.size() || d.dstSubpass >= subpasses.size() ||
        !(subpasses[d.dstSubpass].view_mask & (uint32_t(1) << view))) return 0;
    const auto mask = subpasses[d.srcSubpass].view_mask;
    if (!(d.dependencyFlags & VK_DEPENDENCY_VIEW_LOCAL_BIT)) return mask;
    const int64_t source = int64_t(view) + dependency.view_offset;
    return source >= 0 && source < 32 ? mask & (uint32_t(1) << uint32_t(source)) : 0;
}

VkImageLayout MultiviewPassPlan::previous_layout(uint32_t attachment, uint32_t subpass, uint32_t view) const {
    while (subpass) {
        const auto& previous = subpasses[--subpass];
        if ((previous.view_mask & (uint32_t(1) << view)) && used(previous, attachment)) return layout(previous, attachment);
    }
    return attachments.at(attachment).initialLayout;
}

VkImageLayout MultiviewPassPlan::attachment_layout(uint32_t attachment, uint32_t subpass) const {
    return layout(subpasses.at(subpass), attachment);
}
MultiviewPassPlan::Chunk MultiviewPassPlan::chunk(uint32_t subpass, uint32_t view, bool first, bool last,
    const std::vector<uint32_t>& manual_layouts) const {
    if (!core_only) throw std::invalid_argument("extended multiview semantics require an additional lowering");
    const auto& src = subpasses.at(subpass);
    if (view >= 32 || !(src.view_mask & (uint32_t(1) << view))) throw std::invalid_argument("inactive multiview view");
    Chunk result;
    std::map<uint32_t, uint32_t> indices;
    references(src, last, [&](const VkAttachmentReference& ref) {
        if (ref.attachment == VK_ATTACHMENT_UNUSED || indices.count(ref.attachment)) return;
        const uint32_t attachment = ref.attachment;
        const auto& life = lifetimes[attachment];
        const bool resolve_only = !used(src, attachment, false);
        const bool starts = life.first[view] == subpass;
        const bool ends = life.last[view] == subpass;
        auto desc = attachments[attachment];
        desc.initialLayout = (first || resolve_only)
            ? (starts ? desc.initialLayout : previous_layout(attachment, subpass, view)) : layout(src, attachment);
        desc.finalLayout = last && ends ? desc.finalLayout : layout(src, attachment);
        // A 2D view of a 3D image changes the layout of the entire mip in Core
        // 1.1. Its transitions are emitted once around all views, not per slice.
        if (std::find(manual_layouts.begin(), manual_layouts.end(), attachment) != manual_layouts.end())
            desc.initialLayout = desc.finalLayout = layout(src, attachment);
        if (!(first && starts) || resolve_only) {
            desc.loadOp = resolve_only ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
            desc.stencilLoadOp = resolve_only ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        }
        if (!(last && ends)) { desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE; desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE; }
        indices[attachment] = uint32_t(result.attachments.size());
        result.attachments.push_back(desc); result.original_attachments.push_back(attachment);
    });
    auto refs = [&](const std::vector<VkAttachmentReference>& source) {
        auto mapped = source;
        for (auto& ref : mapped) if (ref.attachment != VK_ATTACHMENT_UNUSED) ref.attachment = indices.at(ref.attachment);
        return mapped;
    };
    result.inputs = refs(src.inputs); result.colors = refs(src.colors);
    if (last) result.resolves = refs(src.resolves);
    if (src.depth) {
        auto ref = *src.depth;
        if (ref.attachment != VK_ATTACHMENT_UNUSED) ref.attachment = indices.at(ref.attachment);
        result.depth = ref;
    }
    for (uint32_t i = 0; i < src.input_aspects.size(); ++i)
        if (src.input_aspects[i] && src.inputs[i].attachment != VK_ATTACHMENT_UNUSED)
            result.input_aspects.push_back({0, i, src.input_aspects[i]});
    for (const auto& dependency : dependencies) {
        auto d = dependency.info;
        if (d.srcSubpass != subpass && d.dstSubpass != subpass) continue;
        bool affects = true;
        if (d.dependencyFlags & VK_DEPENDENCY_VIEW_LOCAL_BIT) {
            if (d.dstSubpass == subpass) affects = source_views(dependency, view) != 0;
            else {
                const int64_t destination = int64_t(view) - dependency.view_offset;
                affects = destination >= 0 && destination < 32 &&
                    (source_views(dependency, uint32_t(destination)) & (uint32_t(1) << view));
            }
        }
        if (!affects) continue;
        d.srcSubpass = d.srcSubpass == subpass ? 0 : VK_SUBPASS_EXTERNAL;
        d.dstSubpass = d.dstSubpass == subpass ? 0 : VK_SUBPASS_EXTERNAL;
        d.dependencyFlags &= ~VK_DEPENDENCY_VIEW_LOCAL_BIT;
        result.dependencies.push_back(d);
    }
    result.subpass.pipelineBindPoint = src.bind_point;
    return result;
}

VkRenderPassCreateInfo MultiviewPassPlan::Chunk::info(bool native_input_aspects) {
    subpass.inputAttachmentCount = uint32_t(inputs.size()); subpass.pInputAttachments = inputs.data();
    subpass.colorAttachmentCount = uint32_t(colors.size()); subpass.pColorAttachments = colors.data();
    subpass.pResolveAttachments = resolves.empty() ? nullptr : resolves.data();
    subpass.pDepthStencilAttachment = depth ? &*depth : nullptr;
    aspects.aspectReferenceCount = uint32_t(input_aspects.size()); aspects.pAspectReferences = input_aspects.data();
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.pNext = native_input_aspects && !input_aspects.empty() ? &aspects : nullptr;
    ci.attachmentCount = uint32_t(attachments.size()); ci.pAttachments = attachments.data();
    ci.subpassCount = 1; ci.pSubpasses = &subpass;
    ci.dependencyCount = uint32_t(dependencies.size()); ci.pDependencies = dependencies.data();
    return ci;
}

std::vector<VkClearValue> MultiviewPassPlan::Chunk::clear_values(uint32_t count, const VkClearValue* original) const {
    std::vector<VkClearValue> result(attachments.size());
    if (original) for (uint32_t i = 0; i < original_attachments.size(); ++i)
        if (original_attachments[i] < count) result[i] = original[original_attachments[i]];
    return result;
}

MultiviewPassResources::MultiviewPassResources(VkDevice device, std::shared_ptr<const MultiviewPassPlan> plan, bool emulated)
    : device_(device), plan_(std::move(plan)), emulated_(emulated) {
    create_ = reinterpret_cast<PFN_vkCreateRenderPass>(get_real_proc(get_last_instance(), device, "vkCreateRenderPass"));
    destroy_ = reinterpret_cast<PFN_vkDestroyRenderPass>(get_real_proc(get_last_instance(), device, "vkDestroyRenderPass"));
}
MultiviewPassResources::~MultiviewPassResources() { release(); }
void MultiviewPassResources::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_ && destroy_) for (const auto& pass : native_) destroy_(device_, pass.second, nullptr);
    native_.clear(); device_ = VK_NULL_HANDLE;
}
VkResult MultiviewPassResources::create_base(const VkAllocationCallbacks* allocator,
                                            bool native_input_aspects, VkRenderPass& result) {
    result = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !create_ || !plan_->core_only) return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        auto subpasses = plan_->subpasses;
        auto attachments = plan_->attachments;
        std::vector<VkSubpassDescription> native(subpasses.size());
        std::vector<VkInputAttachmentAspectReference> input_aspects;
        const bool layouts = LayoutCompat::get().is_emulated(device_);
        if (layouts) for (auto& a : attachments) {
            a.initialLayout = LayoutCompat::sanitize(a.initialLayout);
            a.finalLayout = LayoutCompat::sanitize(a.finalLayout);
        }
        for (uint32_t i = 0; i < subpasses.size(); ++i) {
            auto& s = subpasses[i]; auto& n = native[i];
            auto refs = [&](std::vector<VkAttachmentReference>& values) {
                if (layouts) for (auto& ref : values) ref.layout = LayoutCompat::sanitize(ref.layout);
                return values.empty() ? nullptr : values.data();
            };
            n.pipelineBindPoint = s.bind_point;
            n.inputAttachmentCount = uint32_t(s.inputs.size()); n.pInputAttachments = refs(s.inputs);
            n.colorAttachmentCount = uint32_t(s.colors.size()); n.pColorAttachments = refs(s.colors);
            n.pResolveAttachments = refs(s.resolves);
            if (s.depth) {
                if (layouts) s.depth->layout = LayoutCompat::sanitize(s.depth->layout);
                n.pDepthStencilAttachment = &*s.depth;
            }
            n.preserveAttachmentCount = uint32_t(s.preserves.size()); n.pPreserveAttachments = s.preserves.data();
            for (uint32_t a = 0; a < s.input_aspects.size(); ++a)
                if (s.input_aspects[a] && s.inputs[a].attachment != VK_ATTACHMENT_UNUSED)
                    input_aspects.push_back({i, a, s.input_aspects[a]});
        }
        std::vector<VkSubpassDependency> dependencies;
        for (const auto& d : plan_->dependencies) {
            auto native = d.info; native.dependencyFlags &= ~VK_DEPENDENCY_VIEW_LOCAL_BIT;
            dependencies.push_back(native);
        }
        VkRenderPassInputAttachmentAspectCreateInfo aspects{VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO};
        aspects.aspectReferenceCount = uint32_t(input_aspects.size()); aspects.pAspectReferences = input_aspects.data();
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.pNext = native_input_aspects && !input_aspects.empty() ? &aspects : nullptr;
        info.attachmentCount = uint32_t(attachments.size()); info.pAttachments = attachments.data();
        info.subpassCount = uint32_t(native.size()); info.pSubpasses = native.data();
        info.dependencyCount = uint32_t(dependencies.size()); info.pDependencies = dependencies.data();
        // Public representative: the caller, rather than this resource cache,
        // owns its lifetime and allocation callbacks.
        return create_(device_, &info, allocator, &result);
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult MultiviewPassResources::native_pass(uint32_t subpass, uint32_t view, bool first, bool last,
                                           bool native_input_aspects, VkRenderPass& result, const std::vector<uint32_t>& manual_layouts) {
    result = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !create_ || !destroy_ || !plan_->core_only) return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        const auto key = std::make_tuple(subpass, view, first, last, native_input_aspects, manual_layouts);
        const auto cached = native_.find(key);
        if (cached != native_.end()) { result = cached->second; return VK_SUCCESS; }
        auto chunk = plan_->chunk(subpass, view, first, last, manual_layouts);
        if (LayoutCompat::get().is_emulated(device_)) sanitize(chunk);
        auto info = chunk.info(native_input_aspects);
        VkRenderPass pass{};
        const auto status = create_(device_, &info, nullptr, &pass);
        if (status != VK_SUCCESS) return status;
        try { native_.emplace(key, pass); }
        catch (...) { destroy_(device_, pass, nullptr); throw; }
        result = pass; return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    catch (const std::invalid_argument&) { return VK_ERROR_FEATURE_NOT_PRESENT; }
    catch (const std::out_of_range&) { return VK_ERROR_FEATURE_NOT_PRESENT; }
}

MultiviewRenderPasses& MultiviewRenderPasses::get() { static MultiviewRenderPasses registry; return registry; }
void MultiviewRenderPasses::remember(VkDevice device, VkRenderPass pass, const VkRenderPassCreateInfo* info) {
    remember(device, pass, MultiviewPassPlan::capture(info));
}
void MultiviewRenderPasses::remember(VkDevice device, VkRenderPass pass, const VkRenderPassCreateInfo2* info) {
    remember(device, pass, MultiviewPassPlan::capture(info));
}
void MultiviewRenderPasses::remember(VkDevice device, VkRenderPass pass, std::shared_ptr<const MultiviewPassPlan> plan) {
    if (!plan) return;
    auto resources = std::make_shared<MultiviewPassResources>(device, std::move(plan));
    remember_resources(device, pass, std::move(resources));
}
void MultiviewRenderPasses::remember_resources(VkDevice device, VkRenderPass pass, std::shared_ptr<MultiviewPassResources> resources) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& live = live_[device];
    live.erase(std::remove_if(live.begin(), live.end(), [](const auto& ref) { return ref.expired(); }), live.end());
    live.push_back(resources);
    passes_[DeviceObjectKey::make(device, pass)] = std::move(resources);
}
bool MultiviewRenderPasses::create_fallback(VkDevice device, const VkRenderPassCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* pass, bool aspects, VkResult& result) {
    try { return create_fallback(device, MultiviewPassPlan::capture(info), allocator, pass, aspects, result); }
    catch (const std::bad_alloc&) { result = VK_ERROR_OUT_OF_HOST_MEMORY; }
    catch (const std::invalid_argument&) { result = VK_ERROR_UNKNOWN; }
    if (pass) *pass = VK_NULL_HANDLE;
    return true;
}
bool MultiviewRenderPasses::create_fallback(VkDevice device, const VkRenderPassCreateInfo2* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* pass, bool aspects, VkResult& result) {
    try { return create_fallback(device, MultiviewPassPlan::capture(info), allocator, pass, aspects, result); }
    catch (const std::bad_alloc&) { result = VK_ERROR_OUT_OF_HOST_MEMORY; }
    catch (const std::invalid_argument&) { result = VK_ERROR_UNKNOWN; }
    if (pass) *pass = VK_NULL_HANDLE;
    return true;
}
bool MultiviewRenderPasses::create_fallback(VkDevice device, std::shared_ptr<const MultiviewPassPlan> plan,
    const VkAllocationCallbacks* allocator, VkRenderPass* pass, bool aspects, VkResult& result) {
    if (!plan) return false;
    result = VK_ERROR_INITIALIZATION_FAILED;
    if (!pass) return true;
    *pass = VK_NULL_HANDLE;
    auto resources = std::make_shared<MultiviewPassResources>(device, std::move(plan), true);
    result = resources->create_base(allocator, aspects, *pass);
    if (result == VK_SUCCESS && !*pass) result = VK_ERROR_INITIALIZATION_FAILED;
    if (result != VK_SUCCESS || !*pass) return true;
    try { remember_resources(device, *pass, std::move(resources)); }
    catch (...) {
        forget(device, *pass);
        auto destroy = (PFN_vkDestroyRenderPass)get_real_proc(get_last_instance(), device, "vkDestroyRenderPass");
        if (destroy) destroy(device, *pass, allocator);
        *pass = VK_NULL_HANDLE; throw;
    }
    return true;
}
std::shared_ptr<MultiviewPassResources> MultiviewRenderPasses::acquire(VkDevice device, VkRenderPass pass) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = passes_.find(DeviceObjectKey::make(device, pass));
    return found == passes_.end() ? nullptr : found->second;
}
void MultiviewRenderPasses::forget(VkDevice device, VkRenderPass pass) {
    std::lock_guard<std::mutex> lock(mutex_);
    passes_.erase(DeviceObjectKey::make(device, pass));
    const auto found = live_.find(device);
    if (found != live_.end()) {
        auto& live = found->second;
        live.erase(std::remove_if(live.begin(), live.end(), [](const auto& ref) { return ref.expired(); }), live.end());
        if (live.empty()) live_.erase(found);
    }
}
void MultiviewRenderPasses::forget_device(VkDevice device) {
    // Release while holding the registry lock so device teardown cannot race
    // publication of another resource record. No resource calls the registry.
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = live_.find(device);
    if (found != live_.end()) {
        for (auto& weak : found->second) if (auto resources = weak.lock()) resources->release();
        live_.erase(found);
    }
    for (auto it = passes_.begin(); it != passes_.end();)
        if (it->first.device == device) it = passes_.erase(it); else ++it;
}
}

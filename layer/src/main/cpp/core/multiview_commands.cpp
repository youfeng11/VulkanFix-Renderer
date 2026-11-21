#include "multiview_commands.h"
#include "multiview_pipeline_layouts.h"
#include "layout_compat.h"
#include "volume_attachment.h"
#include <algorithm>
#include <stdexcept>

namespace vkfix {
namespace {
thread_local unsigned replay_depth = 0;
thread_local bool ordinary_replay = false;
struct ReplayGuard { ReplayGuard() { ++replay_depth; } ~ReplayGuard() { --replay_depth; } };
struct PassReplayGuard {
    bool previous = ordinary_replay;
    explicit PassReplayGuard(bool ordinary) { ordinary_replay = ordinary; }
    ~PassReplayGuard() { ordinary_replay = previous; }
};
}
struct MultiviewCommands::Recording {
    struct VolumePass {
        std::vector<std::unique_ptr<VolumeAttachment>> backings;
        MultiviewFramebufferResources::Views native_attachments;
        AttachmentReplayScope::Bindings bindings;
        std::vector<uint32_t> indices;
        std::shared_ptr<MultiviewFramebufferResources> framebuffer;
        ~VolumePass() {
            if (framebuffer) framebuffer->release();
            for (const auto& binding : bindings) binding.second->release();
        }
    };
    struct Action {
        bool state; Replay replay; bool split = false;
        Replay within; // Equivalent in-subpass operation when a query is active.
        int query_delta = 0;
        VkQueryPool query_pool = VK_NULL_HANDLE;
        uint32_t query_index = 0;
        Replay cleanup;
    };
    std::mutex mutex;
    VkDevice device;
    VkCommandPool pool;
    Callbacks callbacks;
    bool native_aspects, alive = true, scope = false, inherited = false;
    VkResult error = VK_SUCCESS;
    uint32_t subpass = 0;
    std::shared_ptr<MultiviewPassResources> pass;
    std::shared_ptr<MultiviewFramebufferResources> framebuffer;
    MultiviewFramebufferResources::Views attachments;
    VkRect2D area{};
    std::vector<VkClearValue> clears;
    std::vector<VkImageMemoryBarrier> final_transitions;
    std::vector<std::vector<VkImageMemoryBarrier>> subpass_transitions;
    std::vector<std::vector<VkImageMemoryBarrier>> post_subpass_transitions;
    std::vector<uint32_t> manual_layouts;
    bool alias_dependencies = false;
    std::optional<VkDeviceGroupRenderPassBeginInfo> group;
    std::vector<VkRect2D> device_areas;
    std::vector<Action> actions;
    // State set outside a logical pass still applies to every replayed view.
    // Keep the ordered history to preserve partial/overlapping state writes.
    std::vector<Action> state_actions;
    MultiviewDescriptorArena arena;
    std::vector<std::unique_ptr<VolumePass>> volumes;
    VolumePass* volume = nullptr;
    void fail(VkResult status) { if (error == VK_SUCCESS) error = status; }
    void prepare_volumes(VkCommandBuffer cmd) {
        if (std::none_of(attachments.begin(),attachments.end(),[](const auto& source) {
            return source && source->image() && source->image()->emulated_attachment;
        })) return;
        auto owned = std::make_unique<VolumePass>();
        const auto plan = pass->plan(); owned->native_attachments = attachments;
        const uint32_t layers = plan->multiview ? 32u - uint32_t(__builtin_clz(plan->all_views)) : framebuffer->layers();
        for (uint32_t a = 0; a < attachments.size(); ++a) {
            const auto& source = attachments[a];
            if (!source || !source->image() || !source->image()->emulated_attachment) continue;
            if (std::all_of(plan->lifetimes[a].first.begin(),plan->lifetimes[a].first.end(),[](uint32_t first) { return first == VK_SUBPASS_EXTERNAL; })) continue;
            if (!callbacks.physical) { fail(VK_ERROR_INITIALIZATION_FAILED); return; }
            VolumeAttachmentPlan backing_plan;
            auto status = VolumeAttachmentPlan::capture(*source->image(),source->info(),framebuffer->extent(),layers,backing_plan);
            if (status != VK_SUCCESS) { fail(status); return; }
            std::unique_ptr<VolumeAttachment> backing;
            status = VolumeAttachment::create(device,callbacks.physical,backing_plan,backing,callbacks.requirements2);
            if (status != VK_SUCCESS) { fail(status); return; }
            VkImageView view{};
            status = backing->view(0,layers,source->info().subresourceRange.aspectMask,
                layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,view);
            if (status != VK_SUCCESS) { fail(status); return; }
            auto image = std::make_shared<MultiviewImage>(backing->image());
            VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; info.image = image->handle;
            info.viewType = layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY; info.format = image->format;
            info.subresourceRange = {source->info().subresourceRange.aspectMask,0,1,0,layers};
            auto native = std::make_shared<MultiviewViewResources>(device,info,image,nullptr);
            owned->native_attachments[a] = native;
            owned->bindings.emplace_back(source,native); owned->indices.push_back(a); owned->backings.push_back(std::move(backing));
        }
        if (owned->indices.empty()) return;
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; info.width = framebuffer->extent().width;
        info.height = framebuffer->extent().height; info.layers = framebuffer->layers(); info.attachmentCount = uint32_t(attachments.size());
        owned->framebuffer = std::make_shared<MultiviewFramebufferResources>(device,info,owned->native_attachments);
        volume = owned.get(); volumes.push_back(std::move(owned));
        framebuffer = volume->framebuffer;
        for (auto& backing : volume->backings) {
            const auto status = backing->initialize(cmd); if (status != VK_SUCCESS) { fail(status); return; }
        }
    }
    VkResult transfer_volumes(VkCommandBuffer cmd, uint32_t view, const MultiviewPassPlan::Chunk& chunk, bool importing) {
        if (!volume) return VK_SUCCESS;
        const auto plan = pass->plan();
        for (uint32_t i = 0; i < volume->indices.size(); ++i) {
            const auto original = volume->indices[i];
            const auto found = std::find(chunk.original_attachments.begin(),chunk.original_attachments.end(),original);
            if (found == chunk.original_attachments.end()) continue;
            const uint32_t local = uint32_t(found - chunk.original_attachments.begin());
            auto& backing = *volume->backings[i]; const auto& desc = chunk.attachments[local];
            const auto layout = volume_native_layout(plan->attachment_layout(original,subpass));
            const uint32_t first = plan->multiview ? view : 0, count = plan->multiview ? 1 : backing.plan().layers;
            auto aspects = backing.plan().aspects;
            if (importing) {
                if (desc.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) aspects &= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (desc.stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD) aspects &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
            } else {
                const auto writes = [&](const auto& refs) { return std::any_of(refs.begin(),refs.end(),[&](const auto& ref) { return ref.attachment == local; }); };
                const bool output = writes(chunk.colors) || writes(chunk.resolves) || (chunk.depth && chunk.depth->attachment == local);
                // Read-only LOAD inputs must not overwrite an alias with an
                // old snapshot. CLEAR load operations also modify inputs.
                if (!output) {
                    if (desc.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) aspects &= VK_IMAGE_ASPECT_STENCIL_BIT;
                    if (desc.stencilLoadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) aspects &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
                }
            }
            if (!aspects) continue;
            const auto status = importing ? backing.import(cmd,layout,area,first,count,aspects) : backing.store(cmd,layout,area,first,count,aspects);
            if (status != VK_SUCCESS) return status;
        }
        return VK_SUCCESS;
    }
    void prepare_transitions() {
        const auto plan = pass->plan();
        if (attachments.size() != plan->attachments.size()) { fail(VK_ERROR_FEATURE_NOT_PRESENT); return; }
        for (uint32_t a = 0; a < attachments.size(); ++a) {
            alias_dependencies |= (plan->attachments[a].flags & VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT) != 0;
            uint32_t used = 0;
            uint32_t last_usage = 0;
            for (uint32_t view = 0; view < 32; ++view)
                if (plan->lifetimes[a].first[view] != VK_SUBPASS_EXTERNAL) {
                    used |= uint32_t(1) << view;
                    last_usage = std::max(last_usage, plan->lifetimes[a].last[view]);
                }
            auto initial = plan->attachments[a].initialLayout, final = plan->attachments[a].finalLayout;
            if (LayoutCompat::get().is_emulated(device)) {
                initial = LayoutCompat::sanitize(initial); final = LayoutCompat::sanitize(final);
            }
            const auto mask = plan->all_views & ~used;
            if (attachments[a] && attachments[a]->image() && attachments[a]->image()->type == VK_IMAGE_TYPE_3D && used) {
                manual_layouts.push_back(a);
                subpass_transitions.resize(plan->subpasses.size());
                post_subpass_transitions.resize(plan->subpasses.size());
                auto previous = initial;
                for (uint32_t s = 0; s < plan->subpasses.size(); ++s) {
                    auto next = plan->attachment_layout(a, s);
                    if (next == VK_IMAGE_LAYOUT_UNDEFINED) continue;
                    if (LayoutCompat::get().is_emulated(device)) next = LayoutCompat::sanitize(next);
                    const auto status = attachments[a]->layout_barriers(plan->all_views, false, previous, next, subpass_transitions[s]);
                    if (status != VK_SUCCESS) { fail(status); return; }
                    previous = next;
                }
                // Finish before a later subpass can reuse an alias. Deferring
                // this transition to logical pass end would overwrite the
                // later alias's layout using this attachment's stale layout.
                const auto status = attachments[a]->layout_barriers(plan->all_views, false, previous, final, post_subpass_transitions[last_usage]);
                if (status != VK_SUCCESS) { fail(status); return; }
                continue;
            }
            if (!mask || initial == final) continue;
            if (!attachments[a]) { fail(VK_ERROR_FEATURE_NOT_PRESENT); return; }
            const auto status = attachments[a]->layout_barriers(mask, used != 0, initial, final, final_transitions,
                plan->multiview ? 0 : framebuffer->layers());
            if (status != VK_SUCCESS) { fail(status); return; }
        }
        const auto any = [](const auto& batches) {
            return std::any_of(batches.begin(), batches.end(), [](const auto& barriers) { return !barriers.empty(); });
        };
        if (!callbacks.barrier && (alias_dependencies || !final_transitions.empty() || any(subpass_transitions) || any(post_subpass_transitions)))
            fail(VK_ERROR_INITIALIZATION_FAILED);
    }
    void emit_transitions(VkCommandBuffer cmd, const std::vector<VkImageMemoryBarrier>& barriers) {
        if (error != VK_SUCCESS || barriers.empty()) return;
        ReplayGuard guard;
        // Strengthen external synchronization to cover automatic transitions
        // of attachments absent from the compact physical render passes.
        callbacks.barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
    }
    void finish_subpass(VkCommandBuffer cmd) {
        if (error != VK_SUCCESS) return;
        if (!post_subpass_transitions.empty()) emit_transitions(cmd, post_subpass_transitions.at(subpass));
        if (alias_dependencies) {
            // Distinct images may alias the same bound device memory. A global
            // dependency covers that case without relying on image identity
            // or on framebuffer-local synchronization between native chunks.
            VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            callbacks.barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &memory, 0, nullptr, 0, nullptr);
        }
    }
    VkResult replay(VkCommandBuffer cmd, uint32_t view, size_t begin, size_t end) {
        if (!alive) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (error != VK_SUCCESS) return error;
        // Restore the logical state at a continuation boundary for this view.
        // A preceding view may have left different native state behind.
        std::vector<const Action*> active_queries;
        const auto count = std::count_if(actions.begin() + begin, actions.begin() + end, [](const auto& a) { return a.query_delta > 0; });
        active_queries.reserve(size_t(count)); // Allocate before issuing any begin-query command.
        auto close = [&]() noexcept {
            for (auto it = active_queries.rbegin(); it != active_queries.rend(); ++it)
                try { if ((*it)->cleanup) (*it)->cleanup(cmd, view); } catch (...) {}
            active_queries.clear();
        };
        try { for (size_t i = 0; i < end; ++i) {
            const auto& action = actions[i];
            if (i < begin && !action.state) continue;
            const auto& call = action.split ? action.within : action.replay;
            if (!call) { close(); return VK_ERROR_FEATURE_NOT_PRESENT; }
            const auto status = call(cmd, view);
            if (status != VK_SUCCESS) { close(); return status; }
            if (action.query_delta > 0) active_queries.push_back(&action);
            if (action.query_delta < 0) {
                const auto found = std::find_if(active_queries.begin(), active_queries.end(), [&](const auto* a) {
                    return a->query_pool == action.query_pool && a->query_index == action.query_index;
                });
                if (found != active_queries.end()) active_queries.erase(found);
            }
        } } catch (...) { close(); throw; }
        if (!active_queries.empty()) { close(); return VK_ERROR_FEATURE_NOT_PRESENT; }
        return VK_SUCCESS;
    }
    void repair_resolves(VkCommandBuffer cmd, uint32_t view, const MultiviewPassPlan::Chunk& chunk) {
        if (!callbacks.repair_resolve || !callbacks.barrier || chunk.resolves.empty()) return;
        auto rect = area;
        if (group) {
            // The software device-group fallback is singleton. Do not issue a
            // transfer on a different native physical-device mask.
            if (group->deviceMask != 1 || group->deviceRenderAreaCount > 1) return;
            if (group->deviceRenderAreaCount) {
                const auto& device_area = device_areas.at(0);
                const int64_t x = std::max<int64_t>(rect.offset.x, device_area.offset.x);
                const int64_t y = std::max<int64_t>(rect.offset.y, device_area.offset.y);
                const int64_t right = std::min(int64_t(rect.offset.x) + rect.extent.width,
                    int64_t(device_area.offset.x) + device_area.extent.width);
                const int64_t bottom = std::min(int64_t(rect.offset.y) + rect.extent.height,
                    int64_t(device_area.offset.y) + device_area.extent.height);
                if (right <= x || bottom <= y) return;
                rect = {{int32_t(x),int32_t(y)},{uint32_t(right-x),uint32_t(bottom-y)}};
            }
        }
        if (!rect.extent.width || !rect.extent.height) return;
        for (uint32_t i = 0; i < chunk.colors.size(); ++i) {
            const auto source = chunk.colors[i].attachment;
            const auto target = chunk.resolves[i].attachment;
            if (source == VK_ATTACHMENT_UNUSED || target == VK_ATTACHMENT_UNUSED) continue;
            const auto& source_desc = chunk.attachments.at(source);
            if (source_desc.storeOp != VK_ATTACHMENT_STORE_OP_STORE ||
                source_desc.format != VK_FORMAT_R32G32B32A32_SFLOAT || source_desc.samples != VK_SAMPLE_COUNT_4_BIT) continue;
            const auto& src = attachments.at(chunk.original_attachments.at(source));
            const auto& dst = attachments.at(chunk.original_attachments.at(target));
            if (!src || !dst || !src->image() || !dst->image()) continue;
            const auto& source_image = *src->image(); const auto& target_image = *dst->image();
            // Transfer permissions belong to the images, not the attachment
            // views. Never synthesize usage bits at command-recording time.
            // Images lacking them still need another source-preserving route.
            if (source_image.layers <= 1 || source_image.format != source_desc.format ||
                source_image.samples != VK_SAMPLE_COUNT_4_BIT || target_image.format != source_image.format ||
                target_image.samples != VK_SAMPLE_COUNT_1_BIT ||
                !(source_image.native_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
                !(target_image.native_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) continue;
            VkImageResolve region{};
            if (!src->transfer_subresource(view,region.srcSubresource) ||
                !dst->transfer_subresource(view,region.dstSubresource)) continue;
            region.srcOffset = region.dstOffset = {rect.offset.x,rect.offset.y,0};
            region.extent = {rect.extent.width,rect.extent.height,1};
            VkImageMemoryBarrier barriers[2]{};
            const bool sanitize = LayoutCompat::get().is_emulated(device);
            const auto src_layout = sanitize ? LayoutCompat::sanitize(source_desc.finalLayout) : source_desc.finalLayout;
            const auto target_layout = chunk.attachments.at(target).finalLayout;
            const auto dst_layout = sanitize ? LayoutCompat::sanitize(target_layout) : target_layout;
            const VkImageLayout transfer_layouts[]{VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
            const VkImageLayout original_layouts[]{src_layout,dst_layout};
            for (unsigned b = 0; b < 2; ++b) {
                auto& barrier = barriers[b]; barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = b ? target_image.handle : source_image.handle;
                const auto& subresource = b ? region.dstSubresource : region.srcSubresource;
                barrier.subresourceRange = {subresource.aspectMask,subresource.mipLevel,1,subresource.baseArrayLayer,1};
                barrier.oldLayout = original_layouts[b]; barrier.newLayout = transfer_layouts[b];
            }
            // Strong scopes also make the transfer result available to an
            // application's subsequent color-attachment dependencies.
            callbacks.barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0,0,nullptr,0,nullptr,2,barriers);
            callbacks.repair_resolve(cmd,source_image.handle,transfer_layouts[0],target_image.handle,transfer_layouts[1],1,&region);
            for (auto& barrier : barriers) std::swap(barrier.oldLayout,barrier.newLayout);
            callbacks.barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0,0,nullptr,0,nullptr,2,barriers);
        }
    }
    void flush(VkCommandBuffer cmd) {
        if (error != VK_SUCCESS || !alive || inherited) return;
        if (!callbacks.begin || !callbacks.end || !pass || !framebuffer) { fail(VK_ERROR_INITIALIZATION_FAILED); return; }
        ReplayGuard guard;
        PassReplayGuard pass_guard(!pass->plan()->multiview);
        AttachmentReplayScope attachment_scope(volume ? &volume->bindings : nullptr);
        std::vector<const Action*> outside_queries;
        auto close_queries = [&]() noexcept {
            for (auto it = outside_queries.rbegin(); it != outside_queries.rend(); ++it)
                try { if ((*it)->cleanup) (*it)->cleanup(cmd,0); } catch (...) {}
            outside_queries.clear();
        };
        try {
            outside_queries.reserve(size_t(std::count_if(actions.begin(),actions.end(),[](const auto& a) {
                return a.split && a.query_delta > 0;
            })));
            if (!subpass_transitions.empty()) emit_transitions(cmd, subpass_transitions.at(subpass));
            const auto mask = pass->plan()->replay_mask(subpass);
            size_t begin = 0;
            for (;;) {
                size_t end = begin;
                int queries = 0;
                while (end < actions.size()) {
                    const auto& action = actions[end];
                    if (action.split && !queries) break;
                    queries += action.query_delta;
                    if (queries < 0) { fail(VK_ERROR_FEATURE_NOT_PRESENT); return; }
                    ++end;
                }
                if (queries) { fail(VK_ERROR_FEATURE_NOT_PRESENT); return; }
                const bool first = begin == 0, last = end == actions.size();
                for (uint32_t view = 0; view < 32; ++view) if (mask & (uint32_t(1) << view)) {
                VkRenderPass native_pass{}; VkFramebuffer native_fb{};
                const std::vector<uint32_t> no_proxies;
                const auto& proxies = volume ? volume->indices : no_proxies;
                auto status = pass->native_pass(subpass, view, first, last, native_aspects, native_pass, manual_layouts,proxies);
                if (status != VK_SUCCESS) { fail(status); break; }
                status = framebuffer->native_framebuffer(pass, subpass, view, first, last, native_aspects,
                    volume ? volume->native_attachments : attachments,native_fb,manual_layouts,proxies);
                if (status != VK_SUCCESS) { fail(status); break; }
                const auto chunk = pass->plan()->chunk(subpass, view, first, last, manual_layouts,proxies);
                status = transfer_volumes(cmd,view,chunk,true);
                if (status != VK_SUCCESS) { fail(status); break; }
                auto values = chunk.clear_values(uint32_t(clears.size()), clears.data());
                VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                VkDeviceGroupRenderPassBeginInfo devices{};
                if (group) { devices = *group; devices.pDeviceRenderAreas = device_areas.data(); info.pNext = &devices; }
                info.renderPass = native_pass; info.framebuffer = native_fb; info.renderArea = area;
                info.clearValueCount = uint32_t(values.size()); info.pClearValues = values.data();
                callbacks.begin(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
                // Always close the physical pass, including descriptor/pipeline
                // preparation failures, so the real EndCommandBuffer can run.
                try { status = replay(cmd, view, begin, end); }
                catch (...) { callbacks.end(cmd); throw; }
                callbacks.end(cmd);
                if (status != VK_SUCCESS) { fail(status); break; }
                status = transfer_volumes(cmd,view,chunk,false);
                if (status != VK_SUCCESS) { fail(status); break; }
                repair_resolves(cmd,view,chunk);
                }
                if (error != VK_SUCCESS || last) break;
                // All views finish this segment before any view enters the
                // next one. The native barrier is outside physical passes.
                const auto status = actions[end].replay(cmd, 0);
                if (status != VK_SUCCESS) { fail(status); break; }
                const auto& action = actions[end];
                if (action.query_delta > 0) outside_queries.push_back(&action);
                if (action.query_delta < 0) {
                    const auto found = std::find_if(outside_queries.begin(),outside_queries.end(),[&](const auto* begin) {
                        return begin->query_pool == action.query_pool && begin->query_index == action.query_index;
                    });
                    if (found != outside_queries.end()) outside_queries.erase(found);
                }
                begin = end + 1;
            }
            if (!outside_queries.empty() && error == VK_SUCCESS) fail(VK_ERROR_FEATURE_NOT_PRESENT);
            close_queries();
            finish_subpass(cmd);
        } catch (const std::bad_alloc&) { close_queries(); fail(VK_ERROR_OUT_OF_HOST_MEMORY); }
        catch (const std::exception&) { close_queries(); fail(VK_ERROR_FEATURE_NOT_PRESENT); }
    }
};
MultiviewCommands& MultiviewCommands::get() { static MultiviewCommands registry; return registry; }
bool MultiviewCommands::replaying() { return replay_depth != 0; }
bool MultiviewCommands::replaying_multiview() { return replay_depth != 0 && !ordinary_replay; }
bool MultiviewCommands::record_error(VkCommandBuffer cmd, VkResult error) {
    auto record = acquire(cmd);
    if (!record) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    record->fail(error);
    if (!record->scope) { record->scope = true; active_.fetch_add(1, std::memory_order_release); }
    return true;
}
std::shared_ptr<MultiviewCommands::Recording> MultiviewCommands::acquire(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = recordings_.find(cmd); return found == recordings_.end() ? nullptr : found->second;
}
void MultiviewCommands::retire(const std::shared_ptr<Recording>& record) {
    std::lock_guard<std::mutex> lock(record->mutex);
    if (record->scope) { record->scope = false; active_.fetch_sub(1, std::memory_order_release); }
    record->alive = false; record->actions.clear(); record->state_actions.clear(); record->arena.release();
    record->pass.reset(); record->framebuffer.reset(); record->attachments.clear();
    record->final_transitions.clear(); record->subpass_transitions.clear(); record->post_subpass_transitions.clear(); record->manual_layouts.clear();
    record->volume = nullptr; record->volumes.clear();
}
VkResult MultiviewCommands::begin(VkDevice device, VkCommandBuffer cmd, VkCommandPool pool,
    const VkCommandBufferBeginInfo& info, bool aspects, Callbacks callbacks) {
    forget(cmd);
    try {
        auto record = std::make_shared<Recording>();
        record->device = device; record->pool = pool; record->native_aspects = aspects; record->callbacks = callbacks;
        if ((info.flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) && info.pInheritanceInfo) {
            record->pass = MultiviewRenderPasses::get().acquire(device, info.pInheritanceInfo->renderPass);
            if (record->pass && record->pass->emulated()) {
                record->inherited = record->scope = true; record->subpass = info.pInheritanceInfo->subpass;
                if (info.pInheritanceInfo->pNext) record->fail(VK_ERROR_FEATURE_NOT_PRESENT);
            } else record->pass.reset();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto& live = live_[device];
        live.erase(std::remove_if(live.begin(), live.end(), [](const auto& r) { return r.expired(); }), live.end());
        live.push_back(record); recordings_[cmd] = record;
        tracked_.store(uint32_t(recordings_.size()), std::memory_order_release);
        if (record->scope) active_.fetch_add(1, std::memory_order_release);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
bool MultiviewCommands::begin_pass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents) {
    if (replaying() || !info) return false;
    auto record = acquire(cmd); if (!record) return false;
    auto pass = MultiviewRenderPasses::get().acquire(record->device, info->renderPass);
    if (!pass || !pass->emulated()) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (!record->scope) { record->scope = true; active_.fetch_add(1, std::memory_order_release); }
    record->pass = std::move(pass); record->subpass = 0; record->area = info->renderArea; record->actions.clear();
    record->clears.clear(); record->group.reset(); record->device_areas.clear();
    record->final_transitions.clear(); record->subpass_transitions.clear(); record->post_subpass_transitions.clear(); record->manual_layouts.clear();
    record->alias_dependencies = false;
    record->volume = nullptr;
    try {
        record->actions = record->state_actions;
        if (info->clearValueCount) record->clears.assign(info->pClearValues, info->pClearValues + info->clearValueCount);
        for (auto p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO) {
                record->group = *reinterpret_cast<const VkDeviceGroupRenderPassBeginInfo*>(p);
                auto& group = *record->group;
                if (group.deviceRenderAreaCount) record->device_areas.assign(group.pDeviceRenderAreas, group.pDeviceRenderAreas + group.deviceRenderAreaCount);
                group.pNext = nullptr; group.pDeviceRenderAreas = nullptr;
            } else if (p->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) record->fail(VK_ERROR_FEATURE_NOT_PRESENT);
        }
        if (record->group && !record->pass->native_device_group()) {
            // Singleton fallback has one implicit device. Honor its explicit
            // render area before removing the unsupported native pNext node.
            const auto& group = *record->group;
            if (group.deviceMask != 1 || group.deviceRenderAreaCount > 1) record->fail(VK_ERROR_FEATURE_NOT_PRESENT);
            else if (group.deviceRenderAreaCount == 1) record->area = record->device_areas.front();
            record->group.reset(); record->device_areas.clear();
        }
        record->fail(MultiviewAttachments::get().capture_framebuffer(record->device, *info, record->framebuffer, record->attachments));
        if (record->error == VK_SUCCESS) record->prepare_transitions();
        if (record->error == VK_SUCCESS) record->prepare_volumes(cmd);
    } catch (const std::bad_alloc&) { record->fail(VK_ERROR_OUT_OF_HOST_MEMORY); }
    return true;
}
bool MultiviewCommands::next_subpass(VkCommandBuffer cmd, VkSubpassContents) {
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
    auto record = acquire(cmd); if (!record) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (!record->scope || record->inherited) return false;
    record->flush(cmd); record->actions.clear(); ++record->subpass;
    try { record->actions = record->state_actions; }
    catch (const std::bad_alloc&) { record->fail(VK_ERROR_OUT_OF_HOST_MEMORY); }
    return true;
}
bool MultiviewCommands::end_pass(VkCommandBuffer cmd) {
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
    auto record = acquire(cmd); if (!record) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (!record->scope || record->inherited) return false;
    record->flush(cmd); record->emit_transitions(cmd, record->final_transitions); record->actions.clear(); record->scope = false;
    record->pass.reset(); record->framebuffer.reset(); record->attachments.clear();
    record->final_transitions.clear(); record->subpass_transitions.clear(); record->post_subpass_transitions.clear(); record->manual_layouts.clear();
    record->alias_dependencies = false;
    active_.fetch_sub(1, std::memory_order_release); return true;
}
bool MultiviewCommands::capture_impl(VkCommandBuffer cmd, bool state, void* data, Replay(*factory)(void*, MultiviewDescriptorArena&), bool split, int query_delta) {
    if (replaying() || !(state ? tracked_ : active_).load(std::memory_order_acquire)) return false;
    auto record = acquire(cmd); if (!record) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    const bool capturing = record->scope;
    if ((!capturing && !state) || !record->alive) return false;
    if (record->error != VK_SUCCESS) return capturing;
    try {
        Recording::Action action{state, factory(data, record->arena), split, {}, query_delta};
        if (state) record->state_actions.push_back(action);
        if (capturing) record->actions.push_back(std::move(action));
    }
    catch (const std::bad_alloc&) { record->fail(VK_ERROR_OUT_OF_HOST_MEMORY); }
    catch (const std::exception& failure) {
        LOGE("Software multiview command capture %p: %s", VK_HANDLE(cmd), failure.what());
        record->fail(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    return capturing;
}
bool MultiviewCommands::unsupported(VkCommandBuffer cmd) {
    return capture(cmd, false, [] { throw std::runtime_error("multiview command requires an additional lowering"); return Replay{}; });
}
bool MultiviewCommands::execute(VkCommandBuffer cmd, uint32_t count, const VkCommandBuffer* children) {
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
    auto parent = acquire(cmd); if (!parent) return false;
    { std::lock_guard<std::mutex> lock(parent->mutex); if (!parent->scope) return false; }
    try {
        std::vector<Recording::Action> expanded;
        for (uint32_t i = 0; i < count; ++i) {
            auto child = acquire(children[i]);
            if (!child || child == parent) throw std::runtime_error("untracked multiview secondary");
            std::lock_guard<std::mutex> lock(child->mutex);
            if (!child->inherited || !child->alive) throw std::runtime_error("invalid multiview secondary");
            // Also retain/validate empty secondary streams. Each wrapper owns
            // the source identity and serializes access to its descriptor arena.
            expanded.push_back({true, [child](VkCommandBuffer, uint32_t) {
                std::lock_guard<std::mutex> lock(child->mutex);
                return child->alive ? child->error : VK_ERROR_FEATURE_NOT_PRESENT;
            }});
            for (const auto& action : child->actions) {
                auto wrap = [child](Replay replay) -> Replay {
                    if (!replay) return {};
                    return [child, replay = std::move(replay)](VkCommandBuffer target, uint32_t view) {
                        std::lock_guard<std::mutex> lock(child->mutex);
                        if (!child->alive) return VK_ERROR_FEATURE_NOT_PRESENT;
                        if (child->error != VK_SUCCESS) return child->error;
                        return replay(target, view);
                    };
                };
                expanded.push_back({action.state, wrap(action.replay), action.split, wrap(action.within), action.query_delta,
                    action.query_pool, action.query_index, action.cleanup});
            }
        }
        std::lock_guard<std::mutex> lock(parent->mutex);
        if (parent->inherited) { parent->fail(VK_ERROR_FEATURE_NOT_PRESENT); return true; }
        parent->actions.reserve(parent->actions.size() + expanded.size());
        for (auto& action : expanded) parent->actions.push_back(std::move(action));
    } catch (const std::bad_alloc&) {
        std::lock_guard<std::mutex> lock(parent->mutex); parent->fail(VK_ERROR_OUT_OF_HOST_MEMORY);
    } catch (const std::exception&) {
        std::lock_guard<std::mutex> lock(parent->mutex); parent->fail(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    return true;
}
bool MultiviewCommands::pipeline_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
    uint32_t mc, const VkMemoryBarrier* memory, uint32_t bc, const VkBufferMemoryBarrier*,
    uint32_t ic, const VkImageMemoryBarrier* images, bool native_group, PFN_vkCmdPipelineBarrier callback) {
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
    auto record = acquire(cmd); if (!record) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (!record->scope || !record->alive) return false;
    if (record->error != VK_SUCCESS) return true;
    try {
        if (!callback || bc || (flags & ~(VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT | VK_DEPENDENCY_DEVICE_GROUP_BIT)))
            throw std::runtime_error("extended multiview barrier requires an additional lowering");
        for (uint32_t i = 0; i < mc; ++i) if (memory[i].pNext) throw std::runtime_error("extended memory barrier");
        for (uint32_t i = 0; i < ic; ++i)
            if (images[i].pNext || images[i].oldLayout != images[i].newLayout || images[i].srcQueueFamilyIndex != images[i].dstQueueFamilyIndex)
                throw std::runtime_error("image transitions/ownership transfers are not Core in-subpass barriers");
        const auto native_flags = native_group ? flags & VK_DEPENDENCY_DEVICE_GROUP_BIT : 0;
        // Closing/reopening a physical pass introduces attachment STORE/LOAD
        // accesses. Include them as well as the original shader memory scopes.
        // Global synchronization is a permitted strengthening of VIEW_LOCAL
        // and BY_REGION dependencies; neither flag is forwarded outside a pass.
        Replay outside = [callback, native_flags](VkCommandBuffer target, uint32_t) {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            callback(target, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                native_flags, 1, &barrier, 0, nullptr, 0, nullptr);
            return VK_SUCCESS;
        };
        std::vector<VkMemoryBarrier> owned_memory;
        std::vector<VkImageMemoryBarrier> owned_images;
        if (mc) owned_memory.assign(memory, memory + mc);
        if (ic) owned_images.assign(images, images + ic);
        const auto local_flags = (flags & VK_DEPENDENCY_BY_REGION_BIT) | native_flags;
        Replay within = [callback, src, dst, local_flags, memory = std::move(owned_memory), images = std::move(owned_images)](VkCommandBuffer target, uint32_t) {
            callback(target, src, dst, local_flags, uint32_t(memory.size()), memory.data(), 0, nullptr, uint32_t(images.size()), images.data());
            return VK_SUCCESS;
        };
        record->actions.push_back({false, std::move(outside), true, std::move(within)});
    } catch (const std::bad_alloc&) { record->fail(VK_ERROR_OUT_OF_HOST_MEMORY); }
    catch (const std::exception&) { record->fail(VK_ERROR_FEATURE_NOT_PRESENT); }
    return true;
}
bool MultiviewCommands::capture_query(VkCommandBuffer cmd, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags,
    PFN_vkCmdBeginQuery start, PFN_vkCmdEndQuery finish, bool beginning) {
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
    auto record = acquire(cmd); if (!record) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (!record->scope || !record->alive) return false;
    if (record->error != VK_SUCCESS) return true;
    try {
        if (!finish || (beginning && !start)) throw std::runtime_error("missing query callback");
        const auto mask = record->pass->plan()->replay_mask(record->subpass);
        auto index = [=](uint32_t view) { return query + uint32_t(__builtin_popcount(mask & ((uint32_t(1) << view) - 1))); };
        Replay cleanup = [=](VkCommandBuffer target, uint32_t view) { finish(target, pool, index(view)); return VK_SUCCESS; };
        Replay replay = beginning ? Replay([=](VkCommandBuffer target, uint32_t view) { start(target, pool, index(view), flags); return VK_SUCCESS; }) : cleanup;
        // Ordinary layered replay has one query slot. Start/end it outside
        // native passes so the same query can span transfer-backed attachment
        // synchronization between physical chunks without restarting/count loss.
        // Multiview retains independent, in-pass query scopes for each view.
        const bool outside = !record->pass->plan()->multiview;
        record->actions.push_back({false, std::move(replay), outside, {}, beginning ? 1 : -1, pool, query, beginning ? cleanup : Replay{}});
    } catch (const std::bad_alloc&) { record->fail(VK_ERROR_OUT_OF_HOST_MEMORY); }
    catch (const std::exception&) { record->fail(VK_ERROR_FEATURE_NOT_PRESENT); }
    return true;
}
bool MultiviewCommands::begin_query(VkCommandBuffer cmd, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags, PFN_vkCmdBeginQuery callback, PFN_vkCmdEndQuery end) {
    return capture_query(cmd, pool, query, flags, callback, end, true);
}
bool MultiviewCommands::end_query(VkCommandBuffer cmd, VkQueryPool pool, uint32_t query, PFN_vkCmdEndQuery callback) {
    return capture_query(cmd, pool, query, 0, nullptr, callback, false);
}
bool MultiviewCommands::timestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query, PFN_vkCmdWriteTimestamp callback) {
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
    auto record = acquire(cmd); if (!record) return false;
    return capture(cmd, false, [&] {
        if (!callback) throw std::runtime_error("missing timestamp callback");
        const auto mask = record->pass->plan()->replay_mask(record->subpass);
        return [=](VkCommandBuffer target, uint32_t view) {
            callback(target, stage, pool, query + uint32_t(__builtin_popcount(mask & ((uint32_t(1) << view) - 1))));
            return VK_SUCCESS;
        };
    });
}
VkResult MultiviewCommands::error(VkCommandBuffer cmd) {
    auto record = acquire(cmd); if (!record) return VK_SUCCESS;
    std::lock_guard<std::mutex> lock(record->mutex); return record->error;
}
void MultiviewCommands::forget(VkCommandBuffer cmd) {
    std::shared_ptr<Recording> record;
    { std::lock_guard<std::mutex> lock(mutex_); const auto found = recordings_.find(cmd);
      if (found != recordings_.end()) { record = std::move(found->second); recordings_.erase(found); }
      tracked_.store(uint32_t(recordings_.size()), std::memory_order_release); }
    if (record) retire(record);
}
void MultiviewCommands::forget_pool(VkDevice device, VkCommandPool pool) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = recordings_.begin(); it != recordings_.end();)
        if (it->second->device == device && it->second->pool == pool) { retire(it->second); it = recordings_.erase(it); } else ++it;
    tracked_.store(uint32_t(recordings_.size()), std::memory_order_release);
}
void MultiviewCommands::forget_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = live_.find(device);
    if (found != live_.end()) {
        for (const auto& weak : found->second) if (auto record = weak.lock()) retire(record);
        live_.erase(found);
    }
    for (auto it = recordings_.begin(); it != recordings_.end();) if (it->second->device == device) it = recordings_.erase(it); else ++it;
    tracked_.store(uint32_t(recordings_.size()), std::memory_order_release);
}
bool MultiviewCommands::bind_descriptors(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t dynamic_count, const uint32_t* offsets,
    PFN_vkCmdBindDescriptorSets callback) {
    if (replaying() || !tracked_.load(std::memory_order_acquire)) return false;
    auto recording = acquire(cmd); if (!recording) return false;
    const auto device = recording->device;
    return capture(cmd, true, [&](MultiviewDescriptorArena& arena) {
        auto owned = MultiviewPipelineLayouts::get().acquire(device, layout);
        if (!owned || !callback) throw std::runtime_error("missing multiview binding layout");
        std::vector<VkDescriptorSet> source;
        std::vector<uint32_t> dynamic;
        if (count) source.assign(sets, sets + count);
        if (dynamic_count) dynamic.assign(offsets, offsets + dynamic_count);
        auto selected = source;
        return [owned = std::move(owned), source = std::move(source), selected = std::move(selected),
                dynamic = std::move(dynamic), device, point, first, callback, arena = &arena](VkCommandBuffer target, uint32_t view) mutable {
            const auto native = owned->native(); if (!native) return VK_ERROR_FEATURE_NOT_PRESENT;
            for (uint32_t i = 0; i < source.size(); ++i) {
                const auto status = arena->select_all(device, source[i], view, selected[i],!MultiviewCommands::replaying_multiview());
                if (status != VK_SUCCESS) return status;
            }
            callback(target, point, native, first, uint32_t(selected.size()), selected.data(), uint32_t(dynamic.size()), dynamic.data());
            return VK_SUCCESS;
        };
    });
}
bool MultiviewCommands::push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
    uint32_t offset, uint32_t size, const void* data, PFN_vkCmdPushConstants callback) {
    if (replaying() || !tracked_.load(std::memory_order_acquire)) return false;
    auto recording = acquire(cmd); if (!recording) return false;
    return capture(cmd, true, [&] {
        auto owned = MultiviewPipelineLayouts::get().acquire(recording->device, layout);
        if (!owned || !callback) throw std::runtime_error("missing multiview push-constant layout");
        std::vector<uint8_t> bytes(size);
        if (size) std::memcpy(bytes.data(), data, size);
        return [owned = std::move(owned), bytes = std::move(bytes), stages, offset, callback](VkCommandBuffer target, uint32_t) {
            const auto native = owned->native(); if (!native) return VK_ERROR_FEATURE_NOT_PRESENT;
            callback(target, native, stages, offset, uint32_t(bytes.size()), bytes.data()); return VK_SUCCESS;
        };
    });
}
}

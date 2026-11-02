#include "multiview_commands.h"
#include "multiview_pipeline_layouts.h"
#include "layout_compat.h"
#include <algorithm>
#include <stdexcept>

namespace vkfix {
namespace {
thread_local unsigned replay_depth = 0;
struct ReplayGuard { ReplayGuard() { ++replay_depth; } ~ReplayGuard() { --replay_depth; } };
}
struct MultiviewCommands::Recording {
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
    MultiviewDescriptorArena arena;
    void fail(VkResult status) { if (error == VK_SUCCESS) error = status; }
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
            const auto status = attachments[a]->layout_barriers(mask, used != 0, initial, final, final_transitions);
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
    void flush(VkCommandBuffer cmd) {
        if (error != VK_SUCCESS || !alive || inherited) return;
        if (!callbacks.begin || !callbacks.end || !pass || !framebuffer) { fail(VK_ERROR_INITIALIZATION_FAILED); return; }
        ReplayGuard guard;
        try {
            if (!subpass_transitions.empty()) emit_transitions(cmd, subpass_transitions.at(subpass));
            const auto mask = pass->plan()->subpasses.at(subpass).view_mask;
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
                auto status = pass->native_pass(subpass, view, first, last, native_aspects, native_pass, manual_layouts);
                if (status != VK_SUCCESS) { fail(status); break; }
                status = framebuffer->native_framebuffer(pass, subpass, view, first, last, native_aspects, attachments, native_fb, manual_layouts);
                if (status != VK_SUCCESS) { fail(status); break; }
                const auto chunk = pass->plan()->chunk(subpass, view, first, last, manual_layouts);
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
                }
                if (error != VK_SUCCESS || last) break;
                // All views finish this segment before any view enters the
                // next one. The native barrier is outside physical passes.
                const auto status = actions[end].replay(cmd, 0);
                if (status != VK_SUCCESS) { fail(status); break; }
                begin = end + 1;
            }
            finish_subpass(cmd);
        } catch (const std::bad_alloc&) { fail(VK_ERROR_OUT_OF_HOST_MEMORY); }
        catch (const std::exception&) { fail(VK_ERROR_FEATURE_NOT_PRESENT); }
    }
};
MultiviewCommands& MultiviewCommands::get() { static MultiviewCommands registry; return registry; }
bool MultiviewCommands::replaying() { return replay_depth != 0; }
std::shared_ptr<MultiviewCommands::Recording> MultiviewCommands::acquire(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = recordings_.find(cmd); return found == recordings_.end() ? nullptr : found->second;
}
void MultiviewCommands::retire(const std::shared_ptr<Recording>& record) {
    std::lock_guard<std::mutex> lock(record->mutex);
    if (record->scope) { record->scope = false; active_.fetch_sub(1, std::memory_order_release); }
    record->alive = false; record->actions.clear(); record->arena.release();
    record->pass.reset(); record->framebuffer.reset(); record->attachments.clear();
    record->final_transitions.clear(); record->subpass_transitions.clear(); record->post_subpass_transitions.clear(); record->manual_layouts.clear();
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
    try {
        if (info->clearValueCount) record->clears.assign(info->pClearValues, info->pClearValues + info->clearValueCount);
        for (auto p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO) {
                record->group = *reinterpret_cast<const VkDeviceGroupRenderPassBeginInfo*>(p);
                auto& group = *record->group;
                if (group.deviceRenderAreaCount) record->device_areas.assign(group.pDeviceRenderAreas, group.pDeviceRenderAreas + group.deviceRenderAreaCount);
                group.pNext = nullptr; group.pDeviceRenderAreas = nullptr;
            } else if (p->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) record->fail(VK_ERROR_FEATURE_NOT_PRESENT);
        }
        record->fail(MultiviewAttachments::get().capture_framebuffer(record->device, *info, record->framebuffer, record->attachments));
        if (record->error == VK_SUCCESS) record->prepare_transitions();
    } catch (const std::bad_alloc&) { record->fail(VK_ERROR_OUT_OF_HOST_MEMORY); }
    return true;
}
bool MultiviewCommands::next_subpass(VkCommandBuffer cmd, VkSubpassContents) {
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
    auto record = acquire(cmd); if (!record) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (!record->scope || record->inherited) return false;
    record->flush(cmd); record->actions.clear(); ++record->subpass;
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
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
    auto record = acquire(cmd); if (!record) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (!record->scope || !record->alive) return false;
    if (record->error != VK_SUCCESS) return true;
    try { record->actions.push_back({state, factory(data, record->arena), split, {}, query_delta}); }
    catch (const std::bad_alloc&) { record->fail(VK_ERROR_OUT_OF_HOST_MEMORY); }
    catch (const std::exception& failure) {
        LOGE("Software multiview command capture %p: %s", VK_HANDLE(cmd), failure.what());
        record->fail(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    return true;
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
        const auto mask = record->pass->plan()->subpasses.at(record->subpass).view_mask;
        auto index = [=](uint32_t view) { return query + uint32_t(__builtin_popcount(mask & ((uint32_t(1) << view) - 1))); };
        Replay cleanup = [=](VkCommandBuffer target, uint32_t view) { finish(target, pool, index(view)); return VK_SUCCESS; };
        Replay replay = beginning ? Replay([=](VkCommandBuffer target, uint32_t view) { start(target, pool, index(view), flags); return VK_SUCCESS; }) : cleanup;
        record->actions.push_back({false, std::move(replay), false, {}, beginning ? 1 : -1, pool, query, beginning ? cleanup : Replay{}});
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
        const auto mask = record->pass->plan()->subpasses.at(record->subpass).view_mask;
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
      if (found != recordings_.end()) { record = std::move(found->second); recordings_.erase(found); } }
    if (record) retire(record);
}
void MultiviewCommands::forget_pool(VkDevice device, VkCommandPool pool) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = recordings_.begin(); it != recordings_.end();)
        if (it->second->device == device && it->second->pool == pool) { retire(it->second); it = recordings_.erase(it); } else ++it;
}
void MultiviewCommands::forget_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = live_.find(device);
    if (found != live_.end()) {
        for (const auto& weak : found->second) if (auto record = weak.lock()) retire(record);
        live_.erase(found);
    }
    for (auto it = recordings_.begin(); it != recordings_.end();) if (it->second->device == device) it = recordings_.erase(it); else ++it;
}
bool MultiviewCommands::bind_descriptors(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t dynamic_count, const uint32_t* offsets,
    PFN_vkCmdBindDescriptorSets callback) {
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
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
                const auto status = arena->select_all(device, source[i], view, selected[i]);
                if (status != VK_SUCCESS) return status;
            }
            callback(target, point, native, first, uint32_t(selected.size()), selected.data(), uint32_t(dynamic.size()), dynamic.data());
            return VK_SUCCESS;
        };
    });
}
bool MultiviewCommands::push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
    uint32_t offset, uint32_t size, const void* data, PFN_vkCmdPushConstants callback) {
    if (replaying() || !active_.load(std::memory_order_acquire)) return false;
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

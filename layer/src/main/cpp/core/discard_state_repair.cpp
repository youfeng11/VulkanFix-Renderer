#include "discard_state_repair.h"
#include "driver_trust.h"
#include "vk_common.h"
#include <algorithm>
#include <new>

namespace vkfix {
namespace {
bool dynamic(const VkGraphicsPipelineCreateInfo &info, VkDynamicState state) {
    if (!info.pDynamicState) return false;
    const auto &states = *info.pDynamicState;
    return states.dynamicStateCount && std::find(states.pDynamicStates,
        states.pDynamicStates + states.dynamicStateCount, state) != states.pDynamicStates + states.dynamicStateCount;
}
template<class T>
void remember(std::vector<std::optional<T>> &slots, uint32_t first, uint32_t count, const T *values) {
    if (slots.size() < first + count) slots.resize(first + count);
    for (uint32_t i = 0; i < count; ++i) slots[first + i] = values[i];
}
template<class T>
std::vector<T> snapshot(const std::vector<std::optional<T>> &slots, uint32_t count) {
    if (!count || slots.size() < count) return {};
    std::vector<T> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!slots[i]) return {};
        values.push_back(*slots[i]);
    }
    return values;
}
} // namespace

DiscardStateRepair &DiscardStateRepair::get() {
    static DiscardStateRepair repair;
    return repair;
}
void DiscardStateRepair::configure_device(VkInstance instance, VkPhysicalDevice physical,
                                          PFN_vkGetInstanceProcAddr gipa, VkDevice device,
                                          const VkDeviceCreateInfo &info) {
    if (!gipa || !instance || !physical || !device) return;
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    if (!query || !properties) return;
    try {
        uint32_t count = 0;
        query(physical, &count, nullptr);
        if (!count || count > 65536) return;
        std::vector<VkQueueFamilyProperties> families(count);
        query(physical, &count, families.data());
        if (count > families.size()) return;
        const bool graphics = std::any_of(info.pQueueCreateInfos, info.pQueueCreateInfos + info.queueCreateInfoCount,
            [&](const auto &queue) { return queue.queueCount && queue.queueFamilyIndex < count &&
                (families[queue.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT); });
        if (!graphics) return;
        const NativeDriverProbes probes(instance, physical, gipa);
        const auto result = probes.measure(DriverProbe::RasterizerDiscardState);
        LOGI("Driver probe rasterizer discard state: %s (%s)", driver_probe_verdict_name(result.verdict), result.detail.c_str());
        if (result.verdict != DriverProbeVerdict::RepairConfirmed) return;
        VkPhysicalDeviceProperties limits{};
        properties(physical, &limits);
        enable(device, limits.limits.maxViewports);
        LOGI("Measured driver repair: viewport/scissor restored after discard pipeline transitions");
    } catch (const std::bad_alloc &) {}
}
void DiscardStateRepair::enable(VkDevice device, uint32_t max_viewports) {
    if (!device || !max_viewports) return;
    std::lock_guard lock(mutex_);
    devices_.insert_or_assign(device, max_viewports);
    any_enabled_.store(true, std::memory_order_release);
}
bool DiscardStateRepair::enabled(VkDevice device) const {
    if (!any_enabled_.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mutex_);
    return devices_.count(device) != 0;
}
void DiscardStateRepair::forget_device(VkDevice device) {
    if (!any_enabled_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex_);
    devices_.erase(device);
    for (auto it = pipelines_.begin(); it != pipelines_.end();)
        if (it->first.device == device) it = pipelines_.erase(it); else ++it;
    for (auto it = commands_.begin(); it != commands_.end();)
        if (it->second.device == device) it = commands_.erase(it); else ++it;
    any_enabled_.store(!devices_.empty(), std::memory_order_release);
}
void DiscardStateRepair::remember_pipelines(VkDevice device, uint32_t count,
                                           const VkGraphicsPipelineCreateInfo *infos, const VkPipeline *pipelines) {
    if (!any_enabled_.load(std::memory_order_acquire) || !infos || !pipelines) return;
    std::lock_guard lock(mutex_);
    if (!devices_.count(device)) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (!pipelines[i]) continue;
        const auto key = DeviceObjectKey::make(device, pipelines[i]);
        pipelines_.erase(key);
        const auto &info = infos[i];
        VkPipelineCreateFlags2 flags = info.flags;
        for (auto *chain = static_cast<const VkBaseInStructure *>(info.pNext); chain; chain = chain->pNext)
            if (chain->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)
                flags = reinterpret_cast<const VkPipelineCreateFlags2CreateInfo *>(chain)->flags;
        if ((flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) || !info.pRasterizationState ||
            dynamic(info, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE)) continue;
        Pipeline state;
        state.discard = info.pRasterizationState->rasterizerDiscardEnable;
        state.viewport_dynamic = dynamic(info, VK_DYNAMIC_STATE_VIEWPORT);
        state.scissor_dynamic = dynamic(info, VK_DYNAMIC_STATE_SCISSOR);
        // The viewport state pointer is ignored for a static discard pipeline.
        if (!state.discard) {
            if (!info.pViewportState) continue;
            state.viewports = info.pViewportState->viewportCount;
            state.scissors = info.pViewportState->scissorCount;
        }
        try { pipelines_.emplace(key, state); }
        catch (const std::bad_alloc &) { /* An untracked bind invalidates cached state. */ }
    }
}
void DiscardStateRepair::forget_pipeline(VkDevice device, VkPipeline pipeline) {
    if (!any_enabled_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex_);
    pipelines_.erase(DeviceObjectKey::make(device, pipeline));
}
void DiscardStateRepair::viewport(VkDevice device, VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                  const VkViewport *values) {
    if (!any_enabled_.load(std::memory_order_acquire) || !count || !values) return;
    std::lock_guard lock(mutex_);
    const auto found = devices_.find(device);
    if (found == devices_.end() || first >= found->second || count > found->second - first) return;
    try {
        auto &state = commands_[cmd];
        state.device = device;
        remember(state.viewports, first, count, values);
    } catch (const std::bad_alloc &) { commands_.erase(cmd); }
}
void DiscardStateRepair::scissor(VkDevice device, VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                 const VkRect2D *values) {
    if (!any_enabled_.load(std::memory_order_acquire) || !count || !values) return;
    std::lock_guard lock(mutex_);
    const auto found = devices_.find(device);
    if (found == devices_.end() || first >= found->second || count > found->second - first) return;
    try {
        auto &state = commands_[cmd];
        state.device = device;
        remember(state.scissors, first, count, values);
    } catch (const std::bad_alloc &) { commands_.erase(cmd); }
}
void DiscardStateRepair::forget_command(VkCommandBuffer cmd) {
    if (!any_enabled_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex_);
    commands_.erase(cmd);
}
void DiscardStateRepair::invalidate_viewport(VkCommandBuffer cmd) {
    if (!any_enabled_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex_);
    const auto state = commands_.find(cmd);
    if (state != commands_.end()) state->second.viewports.clear();
}
void DiscardStateRepair::invalidate_scissor(VkCommandBuffer cmd) {
    if (!any_enabled_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex_);
    const auto state = commands_.find(cmd);
    if (state != commands_.end()) state->second.scissors.clear();
}
void DiscardStateRepair::bound(VkDevice device, VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline,
                               PFN_vkCmdSetViewport set_viewport, PFN_vkCmdSetScissor set_scissor) {
    if (!any_enabled_.load(std::memory_order_acquire) || point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    {
        std::lock_guard lock(mutex_);
        if (!devices_.count(device)) return;
        const auto meta = pipelines_.find(DeviceObjectKey::make(device, pipeline));
        if (meta == pipelines_.end()) { commands_.erase(cmd); return; }
        try {
            auto &state = commands_[cmd];
            state.device = device;
            const auto &bound = meta->second;
            if (!bound.viewport_dynamic) state.viewports.clear();
            if (!bound.scissor_dynamic) state.scissors.clear();
            if (bound.discard) { state.restore = true; return; }
            if (!state.restore) return;
            state.restore = false;
            if (bound.viewport_dynamic) viewports = snapshot(state.viewports, bound.viewports);
            if (bound.scissor_dynamic) scissors = snapshot(state.scissors, bound.scissors);
        } catch (const std::bad_alloc &) { commands_.erase(cmd); return; }
    }
    if (!viewports.empty() && set_viewport) set_viewport(cmd, 0, static_cast<uint32_t>(viewports.size()), viewports.data());
    if (!scissors.empty() && set_scissor) set_scissor(cmd, 0, static_cast<uint32_t>(scissors.size()), scissors.data());
}
} // namespace vkfix

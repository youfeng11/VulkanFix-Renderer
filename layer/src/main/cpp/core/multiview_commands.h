#pragma once
#include "multiview_descriptors.h"
#include <functional>
#include <type_traits>

namespace vkfix {
class MultiviewCommands {
public:
    using Replay = std::function<VkResult(VkCommandBuffer, uint32_t)>;
    struct Callbacks {
        PFN_vkCmdBeginRenderPass begin;
        PFN_vkCmdEndRenderPass end;
        PFN_vkCmdPipelineBarrier barrier = nullptr;
    };
    static MultiviewCommands& get();
    static bool replaying();
    bool has_active_recordings() const { return !replaying() && active_.load(std::memory_order_acquire) != 0; }
    VkResult begin(VkDevice, VkCommandBuffer, VkCommandPool, const VkCommandBufferBeginInfo&, bool native_aspects, Callbacks);
    bool begin_pass(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
    bool next_subpass(VkCommandBuffer, VkSubpassContents);
    bool end_pass(VkCommandBuffer);
    bool execute(VkCommandBuffer, uint32_t, const VkCommandBuffer*);
    VkResult error(VkCommandBuffer);
    void forget(VkCommandBuffer);
    void forget_pool(VkDevice, VkCommandPool);
    void forget_device(VkDevice);
    bool unsupported(VkCommandBuffer);
    // The factory executes only when capturing. Native/ordinary command paths
    // allocate no temporary vectors or std::functions for this test.
    template<class Factory> bool capture(VkCommandBuffer cmd, bool state, Factory&& factory, bool split = false, int query_delta = 0) {
        using F = std::remove_reference_t<Factory>;
        return capture_impl(cmd, state, const_cast<void*>(static_cast<const void*>(&factory)),
            [](void* ptr, MultiviewDescriptorArena& arena) -> Replay {
                if constexpr (std::is_invocable_v<F, MultiviewDescriptorArena&>) return (*static_cast<F*>(ptr))(arena);
                else return (*static_cast<F*>(ptr))();
            }, split, query_delta);
    }
    bool pipeline_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*,
        uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*,
        bool native_device_group, PFN_vkCmdPipelineBarrier);
    bool begin_query(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags, PFN_vkCmdBeginQuery, PFN_vkCmdEndQuery);
    bool end_query(VkCommandBuffer, VkQueryPool, uint32_t, PFN_vkCmdEndQuery);
    bool timestamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t, PFN_vkCmdWriteTimestamp);
    bool bind_descriptors(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t,
        uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*, PFN_vkCmdBindDescriptorSets);
    bool push_constants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t,
        uint32_t, const void*, PFN_vkCmdPushConstants);
private:
    struct Recording;
    std::shared_ptr<Recording> acquire(VkCommandBuffer);
    bool capture_impl(VkCommandBuffer, bool, void*, Replay(*)(void*, MultiviewDescriptorArena&), bool split, int query_delta);
    bool capture_query(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags, PFN_vkCmdBeginQuery, PFN_vkCmdEndQuery, bool);
    void retire(const std::shared_ptr<Recording>&);
    std::mutex mutex_;
    std::unordered_map<VkCommandBuffer, std::shared_ptr<Recording>> recordings_;
    std::map<VkDevice, std::vector<std::weak_ptr<Recording>>> live_;
    std::atomic<uint32_t> active_{0};
};
}

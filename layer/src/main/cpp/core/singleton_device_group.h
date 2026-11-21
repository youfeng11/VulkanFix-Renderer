#pragma once
#include "input_pnext_copy.h"
#include <tuple>

namespace vkfix {
// Call only when the device has no enabled native device-group route. Owned
// links preserve unrelated native input nodes without modifying caller memory.
VkResult lower_singleton_group(const VkCommandBufferBeginInfo&, VkCommandBufferBeginInfo&, InputPNextCopy&);
// The void render-pass command must not gain a heap-allocation failure path.
// These are all non-group nodes allowed in its pinned registry input chain.
class SingletonRenderPassBeginChain {
public:
    VkResult copy(const void*);
    const void* head() const { return head_; }
private:
    std::tuple<VkRenderPassAttachmentBeginInfo,VkRenderPassSampleLocationsBeginInfoEXT,
        VkRenderPassTransformBeginInfoQCOM,VkMultiviewPerViewRenderAreasRenderPassBeginInfoQCOM,
        VkRenderPassStripeBeginInfoARM,VkRenderPassPerformanceCountersByRegionBeginInfoARM> nodes_;
    VkBaseOutStructure* head_{};
};
VkResult lower_singleton_group(const VkRenderPassBeginInfo&, VkRenderPassBeginInfo&, SingletonRenderPassBeginChain&);
VkResult lower_singleton_group(const VkMemoryAllocateInfo&, VkMemoryAllocateInfo&, InputPNextCopy&);
VkResult native_device_group_support(VkPhysicalDevice, PFN_vkGetPhysicalDeviceProperties,
    PFN_vkEnumerateDeviceExtensionProperties, bool&, uint32_t api_ceiling = UINT32_MAX);
template<class Info, class Dependency>
void lower_singleton_dependencies(const Info& source, Info& native, std::vector<Dependency>& storage) {
    native = source;
    bool needed = false;
    for (uint32_t i = 0; i < source.dependencyCount; ++i)
        needed |= (source.pDependencies[i].dependencyFlags & VK_DEPENDENCY_DEVICE_GROUP_BIT) != 0;
    if (!needed) return;
    storage.assign(source.pDependencies,source.pDependencies+source.dependencyCount);
    for (auto& dependency : storage) dependency.dependencyFlags &= ~VK_DEPENDENCY_DEVICE_GROUP_BIT;
    native.pDependencies = storage.data();
}
// One immediate submission call. A zero command mask is valid and executes
// no command buffer, while all semaphore operations and the fence still apply.
class SingletonGroupBatch {
public:
    VkResult submits(uint32_t, const VkSubmitInfo*, const VkSubmitInfo*&);
    VkResult sparse(uint32_t, const VkBindSparseInfo*, const VkBindSparseInfo*&);
    VkResult buffers(uint32_t, const VkBindBufferMemoryInfo*, const VkBindBufferMemoryInfo*&);
    VkResult images(uint32_t, const VkBindImageMemoryInfo*, const VkBindImageMemoryInfo*&);
private:
    struct Entry { InputPNextCopy chain; std::vector<VkCommandBuffer> commands; };
    std::vector<Entry> storage_;
    std::vector<VkSubmitInfo> submits_;
    std::vector<VkBindSparseInfo> sparse_;
    std::vector<VkBindBufferMemoryInfo> buffers_;
    std::vector<VkBindImageMemoryInfo> images_;
};
}

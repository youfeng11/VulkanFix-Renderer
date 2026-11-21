#include "singleton_device_group.h"
#include "vk_pnext.h"
#include <new>
#include <array>
#include <type_traits>
#include <algorithm>

namespace vkfix {
VkResult native_device_group_support(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceProperties properties,
    PFN_vkEnumerateDeviceExtensionProperties extensions, bool& supported, uint32_t api_ceiling) {
    supported = false;
    if (!properties || !extensions) return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDeviceProperties info{}; properties(physical,&info);
    if (std::min(info.apiVersion,api_ceiling) >= VK_API_VERSION_1_1) { supported = true; return VK_SUCCESS; }
    try {
        for (;;) {
            uint32_t count = 0; auto status = extensions(physical,nullptr,&count,nullptr);
            if (status != VK_SUCCESS) return status;
            if (!count) return VK_SUCCESS;
            std::vector<VkExtensionProperties> list(count);
            status = extensions(physical,nullptr,&count,list.data());
            if (status == VK_INCOMPLETE) continue;
            if (status != VK_SUCCESS) return status;
            for (uint32_t i = 0; i < count; ++i)
                supported |= std::strcmp(list[i].extensionName,VK_KHR_DEVICE_GROUP_EXTENSION_NAME) == 0;
            return VK_SUCCESS;
        }
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult lower_singleton_group(const VkCommandBufferBeginInfo& source, VkCommandBufferBeginInfo& native, InputPNextCopy& chain) {
    native = source;
    const auto* group = vku::find_pnext<VkDeviceGroupCommandBufferBeginInfo>(source.pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_COMMAND_BUFFER_BEGIN_INFO);
    if (!group) return VK_SUCCESS;
    if (group->deviceMask != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto result = chain.copy(source.pNext,InputChainKind::CommandBegin,false,group->sType);
    if (result == VK_SUCCESS) native.pNext = chain.head();
    return result;
}
VkResult SingletonRenderPassBeginChain::copy(const void* source) {
    head_ = nullptr; auto** tail = &head_;
    std::array<VkStructureType,7> seen{}; size_t count = 0;
    auto fail = [&](VkResult result) { head_ = nullptr; return result; };
    for (auto* p = static_cast<const VkBaseInStructure*>(source); p; p = p->pNext) {
        if (count == seen.size()) return fail(VK_ERROR_INITIALIZATION_FAILED);
        for (size_t i = 0; i < count; ++i) if (seen[i] == p->sType) return fail(VK_ERROR_INITIALIZATION_FAILED);
        seen[count++] = p->sType;
        auto own = [&](auto* type) {
            using T = std::remove_pointer_t<decltype(type)>;
            auto& node = std::get<T>(nodes_); node = *reinterpret_cast<const T*>(p);
            *tail = reinterpret_cast<VkBaseOutStructure*>(&node); tail = &(*tail)->pNext; *tail = nullptr;
        };
#define NODE(stype, type) case stype: own(static_cast<type*>(nullptr)); break
        switch (p->sType) {
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO: break;
        NODE(VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO,VkRenderPassAttachmentBeginInfo);
        NODE(VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT,VkRenderPassSampleLocationsBeginInfoEXT);
        NODE(VK_STRUCTURE_TYPE_RENDER_PASS_TRANSFORM_BEGIN_INFO_QCOM,VkRenderPassTransformBeginInfoQCOM);
        NODE(VK_STRUCTURE_TYPE_MULTIVIEW_PER_VIEW_RENDER_AREAS_RENDER_PASS_BEGIN_INFO_QCOM,VkMultiviewPerViewRenderAreasRenderPassBeginInfoQCOM);
        NODE(VK_STRUCTURE_TYPE_RENDER_PASS_STRIPE_BEGIN_INFO_ARM,VkRenderPassStripeBeginInfoARM);
        NODE(VK_STRUCTURE_TYPE_RENDER_PASS_PERFORMANCE_COUNTERS_BY_REGION_BEGIN_INFO_ARM,VkRenderPassPerformanceCountersByRegionBeginInfoARM);
        default: return fail(VK_ERROR_EXTENSION_NOT_PRESENT);
        }
#undef NODE
    }
    return VK_SUCCESS;
}
VkResult lower_singleton_group(const VkRenderPassBeginInfo& source, VkRenderPassBeginInfo& native, SingletonRenderPassBeginChain& chain) {
    native = source;
    const auto* group = vku::find_pnext<VkDeviceGroupRenderPassBeginInfo>(source.pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO);
    if (!group) return VK_SUCCESS;
    if (group->deviceMask != 1 || group->deviceRenderAreaCount > 1) return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto result = chain.copy(source.pNext);
    if (result != VK_SUCCESS) return result;
    native.pNext = chain.head();
    if (group->deviceRenderAreaCount == 1) native.renderArea = group->pDeviceRenderAreas[0];
    return VK_SUCCESS;
}
VkResult lower_singleton_group(const VkMemoryAllocateInfo& source, VkMemoryAllocateInfo& native, InputPNextCopy& chain) {
    native = source;
    const auto* flags = vku::find_pnext<VkMemoryAllocateFlagsInfo>(source.pNext,VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
    if (!flags) return VK_SUCCESS;
    if ((flags->flags & VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT) && flags->deviceMask != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
    // The struct is also used by independently enabled newer extensions.
    // Keep their flags; only an empty node can be removed entirely.
    const bool empty = !(flags->flags & ~VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT);
    const auto result = chain.copy(source.pNext,InputChainKind::MemoryAllocate,false,
        empty ? flags->sType : VK_STRUCTURE_TYPE_MAX_ENUM);
    if (result != VK_SUCCESS) return result;
    native.pNext = chain.head();
    if (!empty) {
        auto* copy = vku::find_pnext_mut<VkMemoryAllocateFlagsInfo>(native.pNext,flags->sType);
        copy->flags &= ~VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT;
        copy->deviceMask = 0;
    }
    return VK_SUCCESS;
}
VkResult SingletonGroupBatch::submits(uint32_t count, const VkSubmitInfo* source, const VkSubmitInfo*& native) {
    native = source;
    bool needed = false;
    for (uint32_t i = 0; i < count; ++i)
        needed |= vku::has_pnext(source[i].pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO);
    if (!needed) return VK_SUCCESS;
    try {
        storage_.resize(count); submits_.assign(source,source+count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* group = vku::find_pnext<VkDeviceGroupSubmitInfo>(source[i].pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO);
            if (!group) continue;
            for (uint32_t n = 0; n < group->waitSemaphoreCount; ++n) if (group->pWaitSemaphoreDeviceIndices[n]) return VK_ERROR_FEATURE_NOT_PRESENT;
            for (uint32_t n = 0; n < group->signalSemaphoreCount; ++n) if (group->pSignalSemaphoreDeviceIndices[n]) return VK_ERROR_FEATURE_NOT_PRESENT;
            auto& entry = storage_[i]; auto& info = submits_[i];
            const auto result = entry.chain.copy(info.pNext,InputChainKind::Submit,false,group->sType);
            if (result != VK_SUCCESS) return result;
            info.pNext = entry.chain.head();
            bool filtered = false;
            for (uint32_t n = 0; n < group->commandBufferCount; ++n) {
                const auto mask = group->pCommandBufferDeviceMasks[n];
                if (mask & ~1u) return VK_ERROR_FEATURE_NOT_PRESENT;
                filtered |= mask == 0;
            }
            if (filtered) {
                entry.commands.clear();
                entry.commands.reserve(info.commandBufferCount);
                for (uint32_t n = 0; n < info.commandBufferCount; ++n)
                    if (group->pCommandBufferDeviceMasks[n]) entry.commands.push_back(info.pCommandBuffers[n]);
                info.commandBufferCount = uint32_t(entry.commands.size()); info.pCommandBuffers = entry.commands.data();
            }
        }
        native = submits_.data(); return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult SingletonGroupBatch::sparse(uint32_t count, const VkBindSparseInfo* source, const VkBindSparseInfo*& native) {
    native = source;
    bool needed = false;
    for (uint32_t i = 0; i < count; ++i)
        needed |= vku::has_pnext(source[i].pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_BIND_SPARSE_INFO);
    if (!needed) return VK_SUCCESS;
    try {
        storage_.resize(count); sparse_.assign(source,source+count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* group = vku::find_pnext<VkDeviceGroupBindSparseInfo>(source[i].pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_BIND_SPARSE_INFO);
            if (!group) continue;
            if (group->resourceDeviceIndex || group->memoryDeviceIndex) return VK_ERROR_FEATURE_NOT_PRESENT;
            const auto result = storage_[i].chain.copy(source[i].pNext,InputChainKind::BindSparse,false,group->sType);
            if (result != VK_SUCCESS) return result;
            sparse_[i].pNext = storage_[i].chain.head();
        }
        native = sparse_.data(); return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult SingletonGroupBatch::buffers(uint32_t count, const VkBindBufferMemoryInfo* source, const VkBindBufferMemoryInfo*& native) {
    native = source;
    bool needed = false;
    for (uint32_t i = 0; i < count; ++i)
        needed |= vku::has_pnext(source[i].pNext,VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO);
    if (!needed) return VK_SUCCESS;
    try {
        storage_.resize(count); buffers_.assign(source,source+count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* group = vku::find_pnext<VkBindBufferMemoryDeviceGroupInfo>(source[i].pNext,VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO);
            if (!group) continue;
            if (group->deviceIndexCount > 1 || (group->deviceIndexCount == 1 && group->pDeviceIndices[0])) return VK_ERROR_FEATURE_NOT_PRESENT;
            const auto status = storage_[i].chain.copy(source[i].pNext,InputChainKind::BindBuffer,false,group->sType);
            if (status != VK_SUCCESS) return status;
            buffers_[i].pNext = storage_[i].chain.head();
        }
        native = buffers_.data(); return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult SingletonGroupBatch::images(uint32_t count, const VkBindImageMemoryInfo* source, const VkBindImageMemoryInfo*& native) {
    native = source;
    bool needed = false;
    for (uint32_t i = 0; i < count; ++i)
        needed |= vku::has_pnext(source[i].pNext,VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO);
    if (!needed) return VK_SUCCESS;
    try {
        storage_.resize(count); images_.assign(source,source+count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* group = vku::find_pnext<VkBindImageMemoryDeviceGroupInfo>(source[i].pNext,VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO);
            if (!group) continue;
            if (group->deviceIndexCount > 1 || group->splitInstanceBindRegionCount > 1 ||
                (group->deviceIndexCount == 1 && group->pDeviceIndices[0])) return VK_ERROR_FEATURE_NOT_PRESENT;
            // Valid N=1 split input has one rectangle covering the entire
            // image (VUID-VkBindImageMemoryInfo-pNext-01629). Its memory offsets
            // are exactly those of an ordinary whole-image binding.
            const auto status = storage_[i].chain.copy(source[i].pNext,InputChainKind::BindImage,false,group->sType);
            if (status != VK_SUCCESS) return status;
            images_[i].pNext = storage_[i].chain.head();
        }
        native = images_.data(); return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
}

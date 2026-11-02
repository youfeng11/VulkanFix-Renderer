#pragma once
#include <vulkan/vulkan.h>

namespace vkfix {
// Own only handles from successful native calls. A failure may write an
// arbitrary non-null output, which must never reach a destruction callback.
// Ownership spans metadata registration and can be transferred afterwards.
class CreatedShaderModule {
public:
    CreatedShaderModule(VkDevice device, PFN_vkDestroyShaderModule destroy,
                        const VkAllocationCallbacks* allocator = nullptr)
        : device_(device), destroy_(destroy), allocator_(allocator) {}
    ~CreatedShaderModule() { if (handle_) destroy_(device_,handle_,allocator_); }
    CreatedShaderModule(const CreatedShaderModule&) = delete;
    CreatedShaderModule& operator=(const CreatedShaderModule&) = delete;
    VkResult create(PFN_vkCreateShaderModule create, const VkShaderModuleCreateInfo* info) {
        if (!create || !destroy_ || handle_) return VK_ERROR_INITIALIZATION_FAILED;
        VkShaderModule candidate{};
        const auto result = create(device_,info,allocator_,&candidate);
        if (result != VK_SUCCESS) return result;
        if (!candidate) return VK_ERROR_INITIALIZATION_FAILED;
        handle_ = candidate;
        return VK_SUCCESS;
    }
    VkShaderModule get() const { return handle_; }
    VkShaderModule release() { const auto result = handle_; handle_ = VK_NULL_HANDLE; return result; }
private:
    VkDevice device_;
    PFN_vkDestroyShaderModule destroy_;
    const VkAllocationCallbacks* allocator_;
    VkShaderModule handle_{};
};
}

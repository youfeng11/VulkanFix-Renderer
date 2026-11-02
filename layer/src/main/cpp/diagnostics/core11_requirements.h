#pragma once
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkfix::post {
// Mandatory feature dependencies and selected 1.1 minimum limits, from
// Vulkan's features-requirements and limits tables. This is a necessary gate,
// not a replacement for behavioral tests or the full conformance suite.
struct Core11RequiredState {
    bool graphics = false, compute = false, portability_subset = false, draw_extension = false;
    VkPhysicalDeviceFeatures base{};
    VkPhysicalDeviceMultiviewFeatures multiview{};
    VkPhysicalDeviceMultiviewProperties views{};
    VkPhysicalDeviceSubgroupProperties subgroup{};
    VkPhysicalDeviceMaintenance3Properties maintenance{};
    VkPhysicalDevice16BitStorageFeatures storage{};
    VkPhysicalDeviceShaderDrawParametersFeatures draw{};
};
inline std::vector<std::string> missing_core11_requirements(const Core11RequiredState& state) {
    std::vector<std::string> failures;
    auto require = [&](bool valid, const char* name) { if (!valid) failures.emplace_back(name); };
    if (state.graphics) {
        require(state.portability_subset || state.base.robustBufferAccess, "robustBufferAccess");
        require(state.multiview.multiview, "multiview");
    }
    if (state.graphics || state.multiview.multiview) {
        require(state.views.maxMultiviewViewCount >= 6, "maxMultiviewViewCount >= 6");
        require(state.views.maxMultiviewInstanceIndex >= ((1u << 27) - 1), "maxMultiviewInstanceIndex >= 2^27-1");
    }
    if (state.graphics || state.compute) {
        const auto size = state.subgroup.subgroupSize;
        require(size && !(size & (size - 1)), "subgroupSize is a nonzero power of two");
        require(state.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT, "subgroup BASIC operations");
    }
    if (state.compute) require(state.subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT, "compute subgroup stage");
    require(state.maintenance.maxPerSetDescriptors >= 1024, "maxPerSetDescriptors >= 1024");
    require(state.maintenance.maxMemoryAllocationSize >= (VkDeviceSize(1) << 30), "maxMemoryAllocationSize >= 2^30");
    require(!state.storage.uniformAndStorageBuffer16BitAccess || state.storage.storageBuffer16BitAccess,
            "uniform/storage 16-bit access implies storageBuffer16BitAccess");
    require(!state.draw_extension || state.draw.shaderDrawParameters, "shaderDrawParameters for advertised KHR extension");
    return failures;
}
} // namespace vkfix::post

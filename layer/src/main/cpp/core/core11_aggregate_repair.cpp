#include "core11_aggregate_repair.h"
#include "promoted_features.h"
#include "promoted_properties.h"
#include "native_instance_state.h"
#include "driver_loader.h"

#include <cstdlib>
#include <cstring>

namespace vkfix {
namespace {
template <typename T>
T *find_node(void *head, VkStructureType type) {
    for (auto *node = static_cast<VkBaseOutStructure *>(head); node; node = node->pNext)
        if (node->sType == type) return reinterpret_cast<T *>(node);
    return nullptr;
}

uint32_t native_api(VkPhysicalDevice physical) {
    VkInstance instance = native_instance_for_physical(physical);
    if (instance == VK_NULL_HANDLE) instance = get_last_instance();
    auto get = (PFN_vkGetPhysicalDeviceProperties)get_real_proc(
        instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (!get) return 0;
    VkPhysicalDeviceProperties props{};
    get(physical, &props);
    return props.apiVersion;
}
} // namespace

void repair_vulkan11_features(VkPhysicalDevice physical,
                              VkPhysicalDeviceFeatures2 *features) {
    if (!features || native_api(physical) < VK_API_VERSION_1_1) return;
    auto *v11 = find_node<VkPhysicalDeviceVulkan11Features>(
        features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    if (!v11) return;
    if (getenv("VULKAN_FIX_TEST_CORRUPT_CORE11_AGGREGATE")) {
        v11->multiview = !v11->multiview;
        v11->variablePointers = !v11->variablePointers;
    }
    auto query2 = native_features2_proc(physical);
    if (!query2) return;

    VkPhysicalDevice16BitStorageFeatures storage{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceMultiviewFeatures multiview{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceVariablePointersFeatures variable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
    VkPhysicalDeviceProtectedMemoryFeatures protected_memory{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    VkPhysicalDeviceShaderDrawParametersFeatures draw{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    storage.pNext = &multiview;
    multiview.pNext = &variable;
    variable.pNext = &protected_memory;
    protected_memory.pNext = &ycbcr;
    ycbcr.pNext = &draw;
    VkPhysicalDeviceFeatures2 local{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &storage};
    query2(physical, &local);

    v11->storageBuffer16BitAccess = storage.storageBuffer16BitAccess;
    v11->uniformAndStorageBuffer16BitAccess = storage.uniformAndStorageBuffer16BitAccess;
    v11->storagePushConstant16 = storage.storagePushConstant16;
    v11->storageInputOutput16 = storage.storageInputOutput16;
    v11->multiview = multiview.multiview;
    v11->multiviewGeometryShader = multiview.multiviewGeometryShader;
    v11->multiviewTessellationShader = multiview.multiviewTessellationShader;
    v11->variablePointersStorageBuffer = variable.variablePointersStorageBuffer;
    v11->variablePointers = variable.variablePointers;
    v11->protectedMemory = protected_memory.protectedMemory;
    v11->samplerYcbcrConversion = ycbcr.samplerYcbcrConversion;
    v11->shaderDrawParameters = draw.shaderDrawParameters;
}

void repair_vulkan11_properties(VkPhysicalDevice physical,
                                VkPhysicalDeviceProperties2 *properties) {
    if (!properties || native_api(physical) < VK_API_VERSION_1_1) return;
    auto *v11 = find_node<VkPhysicalDeviceVulkan11Properties>(
        properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES);
    if (!v11) return;
    if (getenv("VULKAN_FIX_TEST_CORRUPT_CORE11_AGGREGATE")) {
        v11->subgroupSize = 1;
        v11->maxMultiviewViewCount = 0;
    }
    auto query2 = native_properties2_proc(physical);
    if (!query2) return;

    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDevicePointClippingProperties clipping{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES};
    VkPhysicalDeviceMultiviewProperties multiview{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
    VkPhysicalDeviceProtectedMemoryProperties protected_memory{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES};
    VkPhysicalDeviceMaintenance3Properties maintenance{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    subgroup.pNext = &clipping;
    clipping.pNext = &multiview;
    multiview.pNext = &protected_memory;
    protected_memory.pNext = &maintenance;
    maintenance.pNext = &id;
    VkPhysicalDeviceProperties2 local{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
    query2(physical, &local);

    memcpy(v11->deviceUUID, id.deviceUUID, VK_UUID_SIZE);
    memcpy(v11->driverUUID, id.driverUUID, VK_UUID_SIZE);
    memcpy(v11->deviceLUID, id.deviceLUID, VK_LUID_SIZE);
    v11->deviceNodeMask = id.deviceNodeMask;
    v11->deviceLUIDValid = id.deviceLUIDValid;
    v11->subgroupSize = subgroup.subgroupSize;
    v11->subgroupSupportedStages = subgroup.supportedStages;
    v11->subgroupSupportedOperations = subgroup.supportedOperations;
    v11->subgroupQuadOperationsInAllStages = subgroup.quadOperationsInAllStages;
    v11->pointClippingBehavior = clipping.pointClippingBehavior;
    v11->maxMultiviewViewCount = multiview.maxMultiviewViewCount;
    v11->maxMultiviewInstanceIndex = multiview.maxMultiviewInstanceIndex;
    v11->protectedNoFault = protected_memory.protectedNoFault;
    v11->maxPerSetDescriptors = maintenance.maxPerSetDescriptors;
    v11->maxMemoryAllocationSize = maintenance.maxMemoryAllocationSize;
}
} // namespace vkfix

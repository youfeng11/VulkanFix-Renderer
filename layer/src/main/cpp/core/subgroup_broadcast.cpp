#include "subgroup_broadcast.h"
#include "driver_loader.h"
#include "native_instance_state.h"
#include "promoted_properties.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <vector>
namespace vkfix {
SubgroupBroadcastSupport subgroup_broadcast_support(VkPhysicalDevice physical) {
    auto instance = native_instance_for_physical(physical);
    if (!instance) instance = get_last_instance();
    // The general SPIR-V downgrade must remain enabled for 1.5 -> 1.3.
    const char *enabled = getenv("VULKAN_FIX_SHADER_REWRITE");
    if (enabled && strcmp(enabled, "1") && strcasecmp(enabled, "true") &&
        strcasecmp(enabled, "on") && strcasecmp(enabled, "yes"))
        return {};
    const char *target = getenv("VULKAN_FIX_SPIRV_MAX_VERSION");
    if (target && strcmp(target, "1.3"))
        return {};
    auto props = (PFN_vkGetPhysicalDeviceProperties)get_real_proc(
        instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (!props)
        return {};
    VkPhysicalDeviceProperties base{};
    props(physical, &base);
    if (base.apiVersion < VK_API_VERSION_1_1 || base.apiVersion >= VK_API_VERSION_1_2)
        return {};
    auto query = native_properties2_proc(physical);
    if (!query)
        return {};
    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties sizes{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)get_real_proc(
        instance, VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    bool variable_size = false;
    if (enumerate)
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            uint32_t count = 0;
            if (enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS)
                break;
            std::vector<VkExtensionProperties> values(count);
            auto result = count ? enumerate(physical, nullptr, &count, values.data()) : VK_SUCCESS;
            if (result == VK_INCOMPLETE)
                continue;
            if (result == VK_SUCCESS)
                for (uint32_t i = 0; i < count; ++i)
                    if (!strcmp(values[i].extensionName,
                                VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME))
                        variable_size = true;
            break;
        }
    if (variable_size)
        subgroup.pNext = &sizes;
    VkPhysicalDeviceProperties2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
    query(physical, &root);
    auto maximum = variable_size ? sizes.maxSubgroupSize : subgroup.subgroupSize;
    if (!maximum || (maximum & (maximum - 1)) || !subgroup.supportedStages)
        return {};
    const char *mode = getenv("VULKAN_FIX_SUBGROUP_BROADCAST_MODE");
    bool shuffle = (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0;
    if (mode && !strcmp(mode, "constant"))
        shuffle = false;
    return {maximum, shuffle};
}
} // namespace vkfix

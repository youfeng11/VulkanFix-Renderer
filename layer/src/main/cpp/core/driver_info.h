#pragma once

#include "vk_common.h"

namespace vkfix {
inline void format_vulkan_version(char *output, size_t size, uint32_t version) {
    if (VK_API_VERSION_PATCH(version))
        snprintf(output, size, "%u.%u.%u", VK_API_VERSION_MAJOR(version),
                 VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
    else
        snprintf(output, size, "%u.%u", VK_API_VERSION_MAJOR(version),
                 VK_API_VERSION_MINOR(version));
}

template <size_t N, size_t M>
inline void set_driver_display(char (&name)[N], char (&info)[M], const char *description) {
    snprintf(name, N, "Vulkan Fix");
    snprintf(info, M, "%s", description);
}

// Apply after promoted-property folding has restored the application's chain,
// so aggregate and KHR driver properties expose the same display information.
inline void set_vulkan_fix_driver_info(VkPhysicalDeviceProperties2 &properties,
                                       uint32_t nativeApi) {
    char nativeVersion[32], reportedVersion[32], description[VK_MAX_DRIVER_INFO_SIZE];
    format_vulkan_version(nativeVersion, sizeof(nativeVersion), nativeApi);
    format_vulkan_version(reportedVersion, sizeof(reportedVersion),
                          properties.properties.apiVersion);
    if (properties.properties.apiVersion > nativeApi)
        snprintf(description, sizeof(description), "%s (%s), Emulated Vulkan %s (Vulkan %s)",
                 PROJECT_VERSION_NAME, PROJECT_VERSION_CODE, reportedVersion, nativeVersion);
    else
        snprintf(description, sizeof(description), "%s (%s), Vulkan %s", PROJECT_VERSION_NAME,
                 PROJECT_VERSION_CODE, nativeVersion);
    for (auto node = static_cast<VkBaseOutStructure *>(properties.pNext); node;
         node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
            auto driver = reinterpret_cast<VkPhysicalDeviceVulkan12Properties *>(node);
            set_driver_display(driver->driverName, driver->driverInfo, description);
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            auto driver = reinterpret_cast<VkPhysicalDeviceDriverProperties *>(node);
            set_driver_display(driver->driverName, driver->driverInfo, description);
        }
    }
}
} // namespace vkfix

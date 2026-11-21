#pragma once
#include <string>
#include <vulkan/vulkan.h>

namespace vkfix {
using DriverPostRow = void (*)(const std::string &name, const char *status, const std::string &detail,
                               const char *evidence);
void report_driver_trust(VkInstance, VkPhysicalDevice, PFN_vkGetInstanceProcAddr, DriverPostRow);
}

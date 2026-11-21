#pragma once
#include <string_view>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkfix {
enum class ProcScope { Global, Instance, Physical, Device };
struct ProcMetadata {
    const char* name;
    ProcScope scope;
    bool core;
    const char* instance_extensions;
    const char* device_extensions;
};
const ProcMetadata* proc_metadata(const char* name);
// Record the application's names, before modules add private native dependencies.
VkResult remember_public_instance(VkInstance, const VkInstanceCreateInfo&) noexcept;
VkResult remember_public_device(VkDevice, VkInstance, const VkDeviceCreateInfo&) noexcept;
void forget_public_instance(VkInstance);
void forget_public_device(VkDevice);
void invalidate_public_device_extensions(VkInstance);
bool public_device_proc_allowed(VkDevice, const ProcMetadata&);
using DeviceExtensionInventory = VkResult (*)(VkInstance, std::vector<std::string>&);
bool public_instance_proc_allowed(VkInstance, const ProcMetadata&, DeviceExtensionInventory);
} // namespace vkfix

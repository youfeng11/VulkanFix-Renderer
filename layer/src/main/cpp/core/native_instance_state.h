#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkfix {
struct NativeInstanceState {
  // target_api is the application's version actually sent to the loader.
  uint32_t target_api = VK_API_VERSION_1_0;
  // The wrapper's application target can exceed the version sent to a 1.0
  // loader. Retain it to enable native extensions backing promoted commands.
  uint32_t public_api = VK_API_VERSION_1_0;
  uint32_t instance_api = VK_API_VERSION_1_0;
  std::set<std::string> enabled_extensions;
  std::set<std::string> enabled_layers;
};
void remember_native_instance(VkInstance instance, uint32_t target_api,
                              uint32_t loader_api,
                              const std::vector<const char *> &extensions,
                              const std::vector<const char *> &layers = {},
                              uint32_t public_api = 0);
void remember_physical_devices(VkInstance instance, uint32_t count,
                               const VkPhysicalDevice *physical_devices);
void forget_native_instance(VkInstance instance);
std::optional<NativeInstanceState>
native_state_for_physical(VkPhysicalDevice physical);
VkInstance native_instance_for_physical(VkPhysicalDevice physical);
} // namespace vkfix

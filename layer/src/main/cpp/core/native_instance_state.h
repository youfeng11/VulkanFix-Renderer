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
  uint32_t instance_api = VK_API_VERSION_1_0;
  std::set<std::string> enabled_extensions;
  std::set<std::string> enabled_layers;
};
void remember_native_instance(VkInstance instance, uint32_t target_api,
                              uint32_t loader_api,
                              const std::vector<const char *> &extensions,
                              const std::vector<const char *> &layers = {});
void remember_physical_devices(VkInstance instance, uint32_t count,
                               const VkPhysicalDevice *physical_devices);
void forget_native_instance(VkInstance instance);
std::optional<NativeInstanceState>
native_state_for_physical(VkPhysicalDevice physical);
VkInstance native_instance_for_physical(VkPhysicalDevice physical);
} // namespace vkfix

#pragma once
#include <set>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkfix {
struct ExtensionEnvironment {
  uint32_t instance_api = VK_API_VERSION_1_0;
  uint32_t device_api = VK_API_VERSION_1_0;
  bool instance_creation = false;
  std::set<std::string> available;
  std::set<std::string> enabled_instance;
};
struct ExtensionDependencyPlan {
  VkResult result = VK_SUCCESS;
  // Original names retain their caller-owned lifetime; additions refer to
  // static registry data. No pointer into a temporary set is returned.
  std::vector<const char *> names;
  std::string missing;
};
ExtensionDependencyPlan
close_extension_dependencies(const std::vector<const char *> &requested,
                             const ExtensionEnvironment &environment);
std::vector<const char *>
promoted_instance_extensions(uint32_t public_api,
                             const ExtensionEnvironment &environment);
std::vector<const char *>
vulkan11_device_extensions(uint32_t public_api,
                           const ExtensionEnvironment &environment);
} // namespace vkfix

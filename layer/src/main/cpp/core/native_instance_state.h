#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkfix {
enum class NativeGroupCreationRoute { Singleton, Core, KHR };
NativeGroupCreationRoute native_group_creation_route(VkInstance);
struct NativeInstanceVersions { uint32_t native_api = VK_API_VERSION_1_0, public_api = VK_API_VERSION_1_0; };
NativeInstanceVersions native_instance_versions(VkInstance);
enum class NativeQuery2Route { Legacy, Core, KHR };
struct NativePhysicalQueryContext {
  VkInstance instance;
  uint32_t target_api;
  NativeQuery2Route query2;
};
// Primitive snapshot: query routing must not copy/allocate extension sets.
std::optional<NativePhysicalQueryContext> native_physical_query_context(VkPhysicalDevice);
uint32_t limit_native_device_api(VkPhysicalDevice, uint32_t physical_api);
struct NativeInstanceState {
  // target_api is the application's version actually sent to the loader.
  uint32_t target_api = VK_API_VERSION_1_0;
  // The wrapper's application target can exceed the version sent to a 1.0
  // loader. Retain it to enable native extensions backing promoted commands.
  uint32_t public_api = VK_API_VERSION_1_0;
  uint32_t instance_api = VK_API_VERSION_1_0;
  std::set<std::string> enabled_extensions;
  std::set<std::string> enabled_layers;
  std::optional<VkAllocationCallbacks> allocation_callbacks;
};
void remember_native_instance(VkInstance instance, uint32_t target_api,
                              uint32_t loader_api,
                              const std::vector<const char *> &extensions,
                              const std::vector<const char *> &layers = {},
                              uint32_t public_api = 0,
                              const VkAllocationCallbacks* allocator = nullptr);
std::optional<VkAllocationCallbacks> native_instance_allocation_callbacks(VkInstance instance);
void remember_physical_devices(VkInstance instance, uint32_t count,
                               const VkPhysicalDevice *physical_devices);
void forget_native_instance(VkInstance instance);
std::optional<NativeInstanceState>
native_state_for_physical(VkPhysicalDevice physical);
VkInstance native_instance_for_physical(VkPhysicalDevice physical);
} // namespace vkfix

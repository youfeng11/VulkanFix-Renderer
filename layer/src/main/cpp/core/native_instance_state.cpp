#include "native_instance_state.h"
#include <algorithm>
#include <map>
#include <mutex>

namespace vkfix {
namespace {
std::mutex state_mutex;
std::map<VkInstance, NativeInstanceState> instances;
std::map<VkPhysicalDevice, VkInstance> physical_instances;
} // namespace
void remember_native_instance(VkInstance instance, uint32_t target_api,
                              uint32_t loader_api,
                              const std::vector<const char *> &extensions,
                              const std::vector<const char *> &layers,
                              uint32_t public_api,
                              const VkAllocationCallbacks* allocator) {
  NativeInstanceState state;
  state.target_api = target_api ? target_api : VK_API_VERSION_1_0;
  state.public_api = public_api ? public_api : state.target_api;
  state.instance_api = std::min(state.target_api, loader_api);
  if (allocator) state.allocation_callbacks = *allocator;
  for (auto name : extensions)
    state.enabled_extensions.insert(name);
  for (auto name : layers)
    state.enabled_layers.insert(name);
  std::lock_guard<std::mutex> lock(state_mutex);
  instances[instance] = std::move(state);
}
std::optional<VkAllocationCallbacks> native_instance_allocation_callbacks(VkInstance instance) {
  std::lock_guard<std::mutex> lock(state_mutex);
  const auto found = instances.find(instance);
  return found == instances.end() ? std::optional<VkAllocationCallbacks>{} : found->second.allocation_callbacks;
}
void remember_physical_devices(VkInstance instance, uint32_t count,
                               const VkPhysicalDevice *physical_devices) {
  if (!physical_devices)
    return;
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!instances.count(instance))
    return;
  for (uint32_t i = 0; i < count; ++i)
    physical_instances[physical_devices[i]] = instance;
}
void forget_native_instance(VkInstance instance) {
  std::lock_guard<std::mutex> lock(state_mutex);
  instances.erase(instance);
  for (auto it = physical_instances.begin(); it != physical_instances.end();)
    if (it->second == instance)
      it = physical_instances.erase(it);
    else
      ++it;
}
std::optional<NativeInstanceState>
native_state_for_physical(VkPhysicalDevice physical) {
  std::lock_guard<std::mutex> lock(state_mutex);
  auto parent = physical_instances.find(physical);
  if (parent == physical_instances.end())
    return {};
  auto found = instances.find(parent->second);
  return found == instances.end() ? std::optional<NativeInstanceState>{}
                                  : found->second;
}
VkInstance native_instance_for_physical(VkPhysicalDevice physical) {
  std::lock_guard<std::mutex> lock(state_mutex);
  auto found = physical_instances.find(physical);
  return found == physical_instances.end() ? VK_NULL_HANDLE : found->second;
}
NativeGroupCreationRoute native_group_creation_route(VkInstance instance) {
  std::lock_guard<std::mutex> lock(state_mutex);
  const auto found = instances.find(instance);
  if (found == instances.end()) return NativeGroupCreationRoute::Singleton;
  if (found->second.instance_api >= VK_API_VERSION_1_1) return NativeGroupCreationRoute::Core;
  for (const auto& extension : found->second.enabled_extensions)
    if (extension == VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME) return NativeGroupCreationRoute::KHR;
  return NativeGroupCreationRoute::Singleton;
}
NativeInstanceVersions native_instance_versions(VkInstance instance) {
  std::lock_guard<std::mutex> lock(state_mutex);
  const auto found = instances.find(instance);
  if (found == instances.end()) return {};
  return {found->second.instance_api,found->second.public_api};
}
std::optional<NativePhysicalQueryContext> native_physical_query_context(VkPhysicalDevice physical) {
  std::lock_guard<std::mutex> lock(state_mutex);
  const auto parent = physical_instances.find(physical);
  if (parent == physical_instances.end()) return {};
  const auto found = instances.find(parent->second);
  if (found == instances.end()) return {};
  auto route = NativeQuery2Route::Legacy;
  if (found->second.instance_api >= VK_API_VERSION_1_1) route = NativeQuery2Route::Core;
  else for (const auto& extension : found->second.enabled_extensions)
    if (extension == VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) {
      route = NativeQuery2Route::KHR;
      break;
    }
  return NativePhysicalQueryContext{parent->second,found->second.target_api,route};
}
uint32_t limit_native_device_api(VkPhysicalDevice physical,uint32_t physical_api) {
  const auto context = native_physical_query_context(physical);
  return context ? std::min(physical_api,context->target_api) : physical_api;
}
} // namespace vkfix

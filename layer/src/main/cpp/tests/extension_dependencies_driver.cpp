// Strict native boundary for the real shared-library entry-point test. No GPU
// commands are implemented or submitted by this provider.
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#define EXPORT extern "C" __attribute__((visibility("default")))
namespace {
struct Instance {
  bool live = false;
  uint32_t target = 0;
  std::set<std::string> enabled;
  std::set<std::string> layers;
};
std::array<Instance, 8> instances;
bool incomplete_once = true, missing_storage = false;
uint32_t mismatches = 0, device_calls = 0, last_target = 0;
uint32_t api() {
  const char *mode = getenv("VKFIX_TEST_NATIVE_API");
  return mode && !strcmp(mode, "1.1") ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
}
int slot(VkInstance instance) { return int((uintptr_t)instance - 0x1000); }
int slot(VkPhysicalDevice physical) {
  return int((uintptr_t)physical - 0x2000);
}
bool valid(int index) {
  return index >= 0 && size_t(index) < instances.size() &&
         instances[index].live;
}
std::vector<const char *> instance_names() {
  return {"VK_KHR_get_physical_device_properties2",
          "VK_KHR_device_group_creation", "VK_KHR_external_memory_capabilities",
          "VK_KHR_external_semaphore_capabilities",
          "VK_KHR_external_fence_capabilities"};
}
VkResult enumerate(const std::vector<const char *> &names, uint32_t *count,
                   VkExtensionProperties *out) {
  if (!out) {
    *count = uint32_t(names.size());
    return VK_SUCCESS;
  }
  uint32_t written = std::min(*count, uint32_t(names.size()));
  for (uint32_t i = 0; i < written; ++i) {
    out[i] = {};
    strcpy(out[i].extensionName, names[i]);
    out[i].specVersion = 1;
  }
  *count = written;
  return written < names.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL instance_extensions(const char *layer,
                                                   uint32_t *count,
                                                   VkExtensionProperties *out) {
  if (layer) {
    if (strcmp(layer, "VK_LAYER_TEST_dependency")) return VK_ERROR_LAYER_NOT_PRESENT;
    *count = 0;
    return VK_SUCCESS;
  }
  auto names = instance_names();
  if (incomplete_once) {
    if (!out) {
      *count = uint32_t(names.size() - 1);
      return VK_SUCCESS;
    }
    incomplete_once = false;
  }
  return enumerate(names, count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL version(uint32_t *out) {
  *out = api();
  return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *ci,
                                               const VkAllocationCallbacks *,
                                               VkInstance *out) {
  last_target = ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion
                                     : VK_API_VERSION_1_0;
  if (!last_target)
    last_target = VK_API_VERSION_1_0;
  if (api() == VK_API_VERSION_1_0 && last_target > api())
    return VK_ERROR_INCOMPATIBLE_DRIVER;
  for (size_t i = 0; i < instances.size(); ++i)
    if (!instances[i].live) {
      auto &instance = instances[i];
      instance = {};
      instance.live = true;
      instance.target = last_target;
      for (uint32_t n = 0; n < ci->enabledExtensionCount; ++n)
        instance.enabled.insert(ci->ppEnabledExtensionNames[n]);
      for (uint32_t n = 0; n < ci->enabledLayerCount; ++n)
        instance.layers.insert(ci->ppEnabledLayerNames[n]);
      *out = (VkInstance)(0x1000 + i);
      return VK_SUCCESS;
    }
  return VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance instance,
                                            const VkAllocationCallbacks *) {
  if (valid(slot(instance)))
    instances[slot(instance)].live = false;
}
VKAPI_ATTR VkResult VKAPI_CALL physical_devices(VkInstance instance,
                                                uint32_t *count,
                                                VkPhysicalDevice *out) {
  if (!valid(slot(instance)))
    return VK_ERROR_INITIALIZATION_FAILED;
  if (!out) {
    *count = 1;
    return VK_SUCCESS;
  }
  if (!*count)
    return VK_INCOMPLETE;
  *out = (VkPhysicalDevice)(uintptr_t(0x2000) + slot(instance));
  *count = 1;
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice,
                                      VkPhysicalDeviceProperties *out) {
  *out = {};
  out->apiVersion = api();
  strcpy(out->deviceName, "Strict extension dependency mock");
}
VKAPI_ATTR void VKAPI_CALL features(VkPhysicalDevice,
                                    VkPhysicalDeviceFeatures *out) {
  *out = {};
}
VKAPI_ATTR void VKAPI_CALL memory(VkPhysicalDevice,
                                  VkPhysicalDeviceMemoryProperties *out) {
  *out = {};
}
VKAPI_ATTR VkResult VKAPI_CALL device_extensions(VkPhysicalDevice, const char *layer,
                                                 uint32_t *count,
                                                 VkExtensionProperties *out) {
  if (layer) {
    if (strcmp(layer, "VK_LAYER_TEST_dependency")) return VK_ERROR_LAYER_NOT_PRESENT;
    return enumerate({"VK_TEST_layer_resource"}, count, out);
  }
  std::vector<const char *> names{"VK_KHR_8bit_storage"};
  if (!missing_storage)
    names.push_back("VK_KHR_storage_buffer_storage_class");
  return enumerate(names, count, out);
}
template <unsigned N>
VKAPI_ATTR void VKAPI_CALL features2(VkPhysicalDevice physical,
                                     VkPhysicalDeviceFeatures2 *out) {
  if (slot(physical) != N)
    ++mismatches;
  out->features = {};
  for (auto node = (VkBaseOutStructure *)out->pNext; node; node = node->pNext)
    if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES)
      ((VkPhysicalDevice8BitStorageFeatures *)node)->storageBuffer8BitAccess =
          VK_TRUE;
}
template <unsigned N>
VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical,
                                             const VkDeviceCreateInfo *ci,
                                             const VkAllocationCallbacks *,
                                             VkDevice *out) {
  ++device_calls;
  if (slot(physical) != N)
    ++mismatches;
  if (!valid(N))
    return VK_ERROR_INITIALIZATION_FAILED;
  std::set<std::string> names;
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
    names.insert(ci->ppEnabledExtensionNames[i]);
  if (names.count("VK_TEST_layer_resource") && !instances[N].layers.count("VK_LAYER_TEST_dependency"))
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  if (names.count("VK_KHR_8bit_storage") && api() == VK_API_VERSION_1_0 &&
      (!names.count("VK_KHR_storage_buffer_storage_class") ||
       !instances[N].enabled.count("VK_KHR_get_physical_device_properties2")))
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  *out = (VkDevice)(uintptr_t(0x4000) + N);
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice,
                                          const VkAllocationCallbacks *) {}
VKAPI_ATTR void VKAPI_CALL get_queue(VkDevice device, uint32_t, uint32_t,
                                     VkQueue *out) {
  *out = (VkQueue)((uintptr_t)device + 0x1000);
}
VKAPI_ATTR void VKAPI_CALL families(VkPhysicalDevice, uint32_t *count,
                                    VkQueueFamilyProperties *out) {
  if (!out) {
    *count = 1;
    return;
  }
  if (*count) {
    *out = {};
    out->queueCount = 1;
    out->queueFlags = VK_QUEUE_GRAPHICS_BIT;
    *count = 1;
  }
}
template <unsigned N> PFN_vkVoidFunction instance_proc(const char *name) {
  if (!strcmp(name, "vkCreateDevice"))
    return (PFN_vkVoidFunction)create_device<N>;
  if ((!strcmp(name, "vkGetPhysicalDeviceFeatures2") &&
       api() >= VK_API_VERSION_1_1 &&
       instances[N].target >= VK_API_VERSION_1_1) ||
      (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") &&
       instances[N].enabled.count("VK_KHR_get_physical_device_properties2")))
    return (PFN_vkVoidFunction)features2<N>;
  return nullptr;
}
} // namespace
EXPORT void mock_missing_storage(uint32_t missing) {
  missing_storage = missing;
}
EXPORT uint32_t mock_device_calls() { return device_calls; }
EXPORT uint32_t mock_mismatches() { return mismatches; }
EXPORT uint32_t mock_target_api() { return last_target; }
EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice, const char *name) {
  if (!strcmp(name, "vkDestroyDevice"))
    return (PFN_vkVoidFunction)destroy_device;
  if (!strcmp(name, "vkGetDeviceQueue"))
    return (PFN_vkVoidFunction)get_queue;
  return nullptr;
}
EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance,
                                                const char *name) {
  if (!strcmp(name, "vkGetInstanceProcAddr"))
    return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
  if (!strcmp(name, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
  if (!strcmp(name, "vkCreateInstance"))
    return (PFN_vkVoidFunction)create_instance;
  if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
    return (PFN_vkVoidFunction)instance_extensions;
  if (!strcmp(name, "vkEnumerateInstanceVersion"))
    return api() >= VK_API_VERSION_1_1 ? (PFN_vkVoidFunction)version : nullptr;
  if (!valid(slot(instance)))
    return nullptr;
  if (!strcmp(name, "vkEnumeratePhysicalDevices"))
    return (PFN_vkVoidFunction)physical_devices;
  if (!strcmp(name, "vkDestroyInstance"))
    return (PFN_vkVoidFunction)destroy_instance;
  if (!strcmp(name, "vkGetPhysicalDeviceProperties"))
    return (PFN_vkVoidFunction)properties;
  if (!strcmp(name, "vkGetPhysicalDeviceFeatures"))
    return (PFN_vkVoidFunction)features;
  if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties"))
    return (PFN_vkVoidFunction)memory;
  if (!strcmp(name, "vkEnumerateDeviceExtensionProperties"))
    return (PFN_vkVoidFunction)device_extensions;
  if (!strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties"))
    return (PFN_vkVoidFunction)families;
  switch (slot(instance)) {
  case 0:
    return instance_proc<0>(name);
  case 1:
    return instance_proc<1>(name);
  case 2:
    return instance_proc<2>(name);
  case 3:
    return instance_proc<3>(name);
  case 4:
    return instance_proc<4>(name);
  case 5:
    return instance_proc<5>(name);
  case 6:
    return instance_proc<6>(name);
  case 7:
    return instance_proc<7>(name);
  }
  return nullptr;
}

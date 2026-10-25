#include <algorithm>
#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

namespace {
PFN_vkGetInstanceProcAddr native_gipa{}, layer_gipa{};
PFN_vkGetDeviceProcAddr native_gdpa{}, layer_gdpa{};
std::once_flag once;
std::mutex mutex, log_mutex;
std::map<VkPhysicalDevice, VkInstance> physical_instances;
std::map<VkInstance, VkDebugUtilsMessengerEXT> messengers;
std::vector<VkExtensionProperties> native_extensions;
bool ready = false;
uint64_t messages = 0;
std::map<std::string, uint64_t> message_counts;
FILE *log_file = nullptr;
std::string log_path;
void open_log() {
  if (log_file)
    return;
  const char *configured = getenv("VULKAN_FIX_VALIDATION_LOG");
  const char *home = getenv("HOME");
  log_path = configured && *configured ? configured
             : home && *home ? std::string(home) + "/vulkanfix-validation.log"
                             : "./vulkanfix-validation.log";
  log_file = fopen(log_path.c_str(), "w");
  if (!log_file)
    __android_log_print(ANDROID_LOG_ERROR, "VulkanFixValidation",
                        "Cannot open validation log %s", log_path.c_str());
}
VKAPI_ATTR VkBool32 VKAPI_CALL
message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
  if (!data)
    return VK_FALSE;
  std::lock_guard<std::mutex> lock(log_mutex);
  ++messages;
  const char *id = data->pMessageIdName ? data->pMessageIdName : "unknown";
  const char *text = data->pMessage ? data->pMessage : "";
  const char *block = getenv("VULKAN_FIX_VALIDATION_BLOCK_INVALID");
  const VkBool32 blocked = block && !strcmp(block, "1") ? VK_TRUE : VK_FALSE;
  std::string key(id);
  if (key.size() > 192)
    key.resize(192);
  if (message_counts.size() >= 512 && !message_counts.count(key))
    key = "additional-message-types";
  const uint64_t count = ++message_counts[key];
  // Retain first reports and exponential samples rather than filling storage
  // with the same VUID on every draw. Never change the validator's decision.
  if (count > 8 && (count & (count - 1)) != 0)
    return blocked;
  __android_log_print(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                          ? ANDROID_LOG_ERROR
                          : ANDROID_LOG_WARN,
                      "VulkanFixValidation", "%.192s #%llu: %.6000s", id,
                      (unsigned long long)count, text);
  open_log();
  if (log_file && ftell(log_file) > 4 * 1024 * 1024) {
    fclose(log_file);
    log_file = nullptr;
    const std::string previous = log_path + ".previous";
    rename(log_path.c_str(), previous.c_str());
    log_file = fopen(log_path.c_str(), "w");
  }
  if (log_file) {
    fprintf(log_file,
            "message=%llu repeated=%llu blocked=%u %.192s: %.65536s\n",
            (unsigned long long)messages, (unsigned long long)count, blocked,
            id, text);
    fflush(log_file);
  }
  return blocked;
}
VkDebugUtilsMessengerCreateInfoEXT debug_info() {
  VkDebugUtilsMessengerCreateInfoEXT info{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  info.pfnUserCallback = message;
  return info;
}
bool layer_extension(const char *name) {
  return !strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
         !strcmp(name, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) ||
         !strcmp(name, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
}
VKAPI_ATTR VkResult VKAPI_CALL
bottom_instance(const VkInstanceCreateInfo *info,
                const VkAllocationCallbacks *allocator, VkInstance *out) {
  auto ci = *info;
  std::vector<const char *> extensions, layers;
  for (uint32_t i = 0; i < ci.enabledExtensionCount; ++i) {
    const char *name = ci.ppEnabledExtensionNames[i];
    bool supported = std::any_of(
        native_extensions.begin(), native_extensions.end(),
        [&](const auto &e) { return !strcmp(e.extensionName, name); });
    if (supported || !layer_extension(name))
      extensions.push_back(name);
  }
  for (uint32_t i = 0; i < ci.enabledLayerCount; ++i)
    if (strcmp(ci.ppEnabledLayerNames[i], "VK_LAYER_KHRONOS_validation"))
      layers.push_back(ci.ppEnabledLayerNames[i]);
  ci.enabledExtensionCount = uint32_t(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();
  ci.enabledLayerCount = uint32_t(layers.size());
  ci.ppEnabledLayerNames = layers.data();
  return ((PFN_vkCreateInstance)native_gipa(nullptr, "vkCreateInstance"))(
      &ci, allocator, out);
}
VKAPI_ATTR VkResult VKAPI_CALL enumerate_physical(VkInstance instance,
                                                  uint32_t *count,
                                                  VkPhysicalDevice *out) {
  auto result = ((PFN_vkEnumeratePhysicalDevices)native_gipa(
      instance, "vkEnumeratePhysicalDevices"))(instance, count, out);
  if (out && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
    std::lock_guard<std::mutex> lock(mutex);
    for (uint32_t i = 0; i < *count; ++i)
      physical_instances[out[i]] = instance;
  }
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL
enumerate_groups(VkInstance instance, uint32_t *count,
                 VkPhysicalDeviceGroupProperties *out) {
  auto fn = (PFN_vkEnumeratePhysicalDeviceGroups)native_gipa(
      instance, "vkEnumeratePhysicalDeviceGroups");
  if (!fn)
    fn = (PFN_vkEnumeratePhysicalDeviceGroups)native_gipa(
        instance, "vkEnumeratePhysicalDeviceGroupsKHR");
  if (!fn)
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  auto result = fn(instance, count, out);
  if (out && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
    std::lock_guard<std::mutex> lock(mutex);
    for (uint32_t i = 0; i < *count; ++i)
      for (uint32_t n = 0; n < out[i].physicalDeviceCount; ++n)
        physical_instances[out[i].physicalDevices[n]] = instance;
  }
  return result;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL next_gipa(VkInstance instance,
                                                   const char *name) {
  if (!strcmp(name, "vkCreateInstance"))
    return (PFN_vkVoidFunction)bottom_instance;
  if (!strcmp(name, "vkEnumeratePhysicalDevices"))
    return (PFN_vkVoidFunction)enumerate_physical;
  if (!strcmp(name, "vkEnumeratePhysicalDeviceGroups") ||
      !strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR"))
    return native_gipa(instance, name) ? (PFN_vkVoidFunction)enumerate_groups
                                       : nullptr;
  return native_gipa(instance, name);
}
void initialize() {
  std::call_once(once, [] {
    const char *custom_native =
        getenv("VULKAN_FIX_VALIDATION_NATIVE_DRIVER_PATH");
    auto native =
        dlopen(custom_native && *custom_native ? custom_native : "libvulkan.so",
               RTLD_NOW | RTLD_LOCAL);
    if (!native)
      return;
    native_gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(native, "vkGetInstanceProcAddr");
    native_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(native, "vkGetDeviceProcAddr");
    if (!native_gipa || !native_gdpa)
      return;
    Dl_info own{};
    if (!dladdr((void *)initialize, &own) || !own.dli_fname)
      return;
    std::string path = own.dli_fname;
    auto slash = path.find_last_of('/');
    path = slash == path.npos ? "./" : path.substr(0, slash + 1);
    path += "libVkLayer_khronos_validation.so";
    auto layer = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!layer) {
      __android_log_print(ANDROID_LOG_ERROR, "VulkanFixValidation",
                          "VVL load failed: %s", dlerror());
      return;
    }
    layer_gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(layer, "vkGetInstanceProcAddr");
    layer_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(layer, "vkGetDeviceProcAddr");
    auto negotiate = (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(
        layer, "vkNegotiateLoaderLayerInterfaceVersion");
    VkNegotiateLayerInterface version{LAYER_NEGOTIATE_INTERFACE_STRUCT, nullptr,
                                      2};
    if (negotiate && negotiate(&version) != VK_SUCCESS)
      return;
    if (version.pfnGetInstanceProcAddr)
      layer_gipa = version.pfnGetInstanceProcAddr;
    if (version.pfnGetDeviceProcAddr)
      layer_gdpa = version.pfnGetDeviceProcAddr;
    auto enumerate = (PFN_vkEnumerateInstanceExtensionProperties)native_gipa(
        nullptr, "vkEnumerateInstanceExtensionProperties");
    uint32_t count = 0;
    if (enumerate(nullptr, &count, nullptr) != VK_SUCCESS)
      return;
    native_extensions.resize(count);
    if (enumerate(nullptr, &count, native_extensions.data()) != VK_SUCCESS)
      return;
    native_extensions.resize(count);
    ready = layer_gipa && layer_gdpa;
    if (ready) {
      std::lock_guard<std::mutex> lock(log_mutex);
      open_log();
      if (log_file) {
        fprintf(log_file,
                "VulkanFix validation driver ready: layer link interface=%u\n",
                version.loaderLayerInterfaceVersion);
        fflush(log_file);
      }
    }
    __android_log_print(ANDROID_LOG_INFO, "VulkanFixValidation",
                        "VVL diagnostic bridge ready=%d", ready);
  });
}
VKAPI_ATTR VkResult VKAPI_CALL
create_instance(const VkInstanceCreateInfo *info,
                const VkAllocationCallbacks *allocator, VkInstance *out) {
  initialize();
  if (!ready)
    return VK_ERROR_INITIALIZATION_FAILED;
  if (!info || !out)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto ci = *info;
  std::vector<const char *> extensions;
  for (uint32_t i = 0; i < ci.enabledExtensionCount; ++i)
    extensions.push_back(ci.ppEnabledExtensionNames[i]);
  if (std::none_of(extensions.begin(), extensions.end(), [](auto e) {
        return !strcmp(e, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
      }))
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  if (std::none_of(extensions.begin(), extensions.end(), [](auto e) {
        return !strcmp(e, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
      }))
    extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
  ci.enabledExtensionCount = uint32_t(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();
  VkLayerInstanceLink link{};
  link.pfnNextGetInstanceProcAddr = next_gipa;
  VkLayerInstanceCreateInfo chain{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                  ci.pNext, VK_LAYER_LINK_INFO};
  chain.u.pLayerInfo = &link;
  // Android VVL reads system properties instead of desktop environment
  // settings. Set this diagnostic option through the supported API without
  // changing any global device properties. Multiple settings structs are
  // explicitly allowed in the instance pNext chain.
  const char *sync = getenv("VK_VALIDATION_VALIDATE_SYNC");
  const VkBool32 enable_sync = sync && !strcmp(sync, "1") ? VK_TRUE : VK_FALSE;
  const VkLayerSettingEXT sync_setting{
      "VK_LAYER_KHRONOS_validation", "validate_sync",
      VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enable_sync};
  VkLayerSettingsCreateInfoEXT settings{
      VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT, &chain, 1,
      &sync_setting};
  auto debug = debug_info();
  debug.pNext = &settings;
  ci.pNext = &debug;
  auto result = ((PFN_vkCreateInstance)layer_gipa(nullptr, "vkCreateInstance"))(
      &ci, allocator, out);
  if (result == VK_SUCCESS) {
    auto create = (PFN_vkCreateDebugUtilsMessengerEXT)layer_gipa(
        *out, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerEXT messenger{};
    debug.pNext = nullptr;
    if (create && create(*out, &debug, nullptr, &messenger) == VK_SUCCESS) {
      std::lock_guard<std::mutex> lock(mutex);
      messengers[*out] = messenger;
    }
  }
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL
create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
              const VkAllocationCallbacks *allocator, VkDevice *out) {
  if (!info || !out)
    return VK_ERROR_INITIALIZATION_FAILED;
  VkInstance instance{};
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = physical_instances.find(physical);
    if (it != physical_instances.end())
      instance = it->second;
  }
  if (!instance)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto ci = *info;
  VkLayerDeviceLink link{};
  link.pfnNextGetInstanceProcAddr = next_gipa;
  link.pfnNextGetDeviceProcAddr = native_gdpa;
  VkLayerDeviceCreateInfo chain{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                ci.pNext, VK_LAYER_LINK_INFO};
  chain.u.pLayerInfo = &link;
  ci.pNext = &chain;
  return ((PFN_vkCreateDevice)layer_gipa(instance, "vkCreateDevice"))(
      physical, &ci, allocator, out);
}
VKAPI_ATTR void VKAPI_CALL
destroy_instance(VkInstance instance, const VkAllocationCallbacks *allocator) {
  if (!instance)
    return;
  VkDebugUtilsMessengerEXT messenger{};
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = messengers.find(instance);
    if (it != messengers.end()) {
      messenger = it->second;
      messengers.erase(it);
    }
  }
  auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)layer_gipa(
      instance, "vkDestroyDebugUtilsMessengerEXT");
  if (messenger && destroy)
    destroy(instance, messenger, nullptr);
  ((PFN_vkDestroyInstance)layer_gipa(instance, "vkDestroyInstance"))(instance,
                                                                     allocator);
  std::lock_guard<std::mutex> lock(mutex);
  for (auto it = physical_instances.begin(); it != physical_instances.end();)
    if (it->second == instance)
      it = physical_instances.erase(it);
    else
      ++it;
}
VKAPI_ATTR VkResult VKAPI_CALL enumerate_extensions(
    const char *layer, uint32_t *count, VkExtensionProperties *out) {
  if (layer) {
    auto fn = (PFN_vkEnumerateInstanceExtensionProperties)native_gipa(
        nullptr, "vkEnumerateInstanceExtensionProperties");
    return fn(layer, count, out);
  }
  if (!count)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto extensions = native_extensions;
  for (auto name : {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                    VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,
                    VK_EXT_LAYER_SETTINGS_EXTENSION_NAME}) {
    if (std::none_of(extensions.begin(), extensions.end(), [&](const auto &e) {
          return !strcmp(name, e.extensionName);
        })) {
      VkExtensionProperties ext{};
      snprintf(ext.extensionName, sizeof(ext.extensionName), "%s", name);
      ext.specVersion =
          !strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
              ? VK_EXT_DEBUG_UTILS_SPEC_VERSION
          : !strcmp(name, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)
              ? VK_EXT_VALIDATION_FEATURES_SPEC_VERSION
              : VK_EXT_LAYER_SETTINGS_SPEC_VERSION;
      extensions.push_back(ext);
    }
  }
  if (!out) {
    *count = uint32_t(extensions.size());
    return VK_SUCCESS;
  }
  const uint32_t size = std::min(*count, uint32_t(extensions.size()));
  std::copy_n(extensions.data(), size, out);
  *count = size;
  return size < extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
} // namespace
extern "C" __attribute__((visibility("default"))) PFN_vkVoidFunction
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  initialize();
  if (!ready || !name)
    return nullptr;
  if (!strcmp(name, "vkCreateInstance"))
    return (PFN_vkVoidFunction)create_instance;
  if (!strcmp(name, "vkCreateDevice"))
    return (PFN_vkVoidFunction)create_device;
  if (!strcmp(name, "vkDestroyInstance"))
    return (PFN_vkVoidFunction)destroy_instance;
  if (!strcmp(name, "vkGetInstanceProcAddr"))
    return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
  if (!strcmp(name, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
  if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
    return (PFN_vkVoidFunction)enumerate_extensions;
  if (!instance)
    return native_gipa(instance, name);
  return layer_gipa(instance, name);
}
extern "C" __attribute__((visibility("default"))) PFN_vkVoidFunction
vkGetDeviceProcAddr(VkDevice device, const char *name) {
  initialize();
  return ready && device && name ? layer_gdpa(device, name) : nullptr;
}

// Read-only self-test/diagnostic evidence; never changes validation behavior.
extern "C" __attribute__((visibility("default"))) uint64_t
vulkanfixValidationMessageCount(const char *prefix) {
  initialize();
  if (!ready || !prefix)
    return 0;
  std::lock_guard<std::mutex> lock(log_mutex);
  uint64_t total = 0;
  for (const auto &item : message_counts)
    if (item.first.rfind(prefix, 0) == 0)
      total += item.second;
  return total;
}

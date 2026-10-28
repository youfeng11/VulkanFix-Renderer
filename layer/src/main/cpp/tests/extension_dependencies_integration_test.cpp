#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#define REQUIRE(x)                                                             \
  do {                                                                         \
    if (!(x)) {                                                                \
      printf("FAIL line %d: %s\n", __LINE__, #x);                              \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsetenv("VULKAN_FIX_VALIDATION_DRIVER");
  setenv("VULKAN_FIX_DRIVER_PATH", "./libextension_dependencies_driver.so", 1);
  setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
  setenv("VULKAN_FIX_LOG_PATH", "./extension-integration-layer.log", 1);
  auto driver =
      dlopen("./libextension_dependencies_driver.so", RTLD_NOW | RTLD_LOCAL);
  REQUIRE(driver);
  auto target = (uint32_t(*)())dlsym(driver, "mock_target_api");
  auto calls = (uint32_t(*)())dlsym(driver, "mock_device_calls");
  auto mismatches = (uint32_t(*)())dlsym(driver, "mock_mismatches");
  auto missing_storage =
      (void (*)(uint32_t))dlsym(driver, "mock_missing_storage");
  REQUIRE(target && calls && mismatches && missing_storage);
  auto layer = dlopen("./libvulkanfix.so", RTLD_NOW | RTLD_LOCAL);
  REQUIRE(layer);
  auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(layer, "vkGetInstanceProcAddr");
  auto create_instance = (PFN_vkCreateInstance)dlsym(layer, "vkCreateInstance");
  auto create_device = (PFN_vkCreateDevice)dlsym(layer, "vkCreateDevice");
  auto destroy_device = (PFN_vkDestroyDevice)dlsym(layer, "vkDestroyDevice");
  auto destroy_instance =
      (PFN_vkDestroyInstance)dlsym(layer, "vkDestroyInstance");
  REQUIRE(gipa && create_instance && create_device && destroy_device &&
          destroy_instance);
  const bool legacy = !getenv("VKFIX_TEST_NATIVE_API") ||
                      strcmp(getenv("VKFIX_TEST_NATIVE_API"), "1.1");
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app;
  VkInstance a{}, b{};
  REQUIRE(create_instance(&instance_info, nullptr, &a) == VK_SUCCESS);
  REQUIRE(target() == (legacy ? VK_API_VERSION_1_0 : VK_API_VERSION_1_2));
  REQUIRE(app.apiVersion == VK_API_VERSION_1_2 &&
          instance_info.enabledExtensionCount == 0);
  printf("PASS: native %s loader receives the correct application target and "
         "retries INCOMPLETE enumeration\n",
         legacy ? "1.0" : "1.1");
  VkPhysicalDevice pa{};
  uint32_t count = 1;
  VkPhysicalDeviceGroupProperties group{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES};
  auto groups = (PFN_vkEnumeratePhysicalDeviceGroups)gipa(
      a, "vkEnumeratePhysicalDeviceGroups");
  REQUIRE(groups && groups(a, &count, &group) == VK_SUCCESS && count == 1);
  pa = group.physicalDevices[0];
  app.apiVersion = VK_API_VERSION_1_0;
  REQUIRE(create_instance(&instance_info, nullptr, &b) == VK_SUCCESS);
  VkPhysicalDevice pb{};
  auto enumerate =
      (PFN_vkEnumeratePhysicalDevices)gipa(b, "vkEnumeratePhysicalDevices");
  count = 1;
  REQUIRE(enumerate && enumerate(b, &count, &pb) == VK_SUCCESS);
  float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkPhysicalDeviceVulkan12Features features{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  features.storageBuffer8BitAccess = VK_TRUE;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 &features};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue;
  VkDevice device{};
  REQUIRE(create_device(pa, &device_info, nullptr, &device) == VK_SUCCESS &&
          device);
  REQUIRE(calls() == 1 && mismatches() == 0);
  REQUIRE(device_info.pNext == &features && features.storageBuffer8BitAccess &&
          features.pNext == nullptr && device_info.enabledExtensionCount == 0);
  destroy_device(device, nullptr);
  printf("PASS: actual vkCreateDevice routes aggregate 8-bit storage plus "
         "native dependencies to the correct older instance\n");
  const char *extension = "VK_KHR_8bit_storage";
  device_info.pNext = nullptr;
  device_info.enabledExtensionCount = 1;
  device_info.ppEnabledExtensionNames = &extension;
  device = (VkDevice)(uintptr_t)1;
  REQUIRE(create_device(pb, &device_info, nullptr, &device) ==
              VK_ERROR_EXTENSION_NOT_PRESENT &&
          !device);
  REQUIRE(calls() == 1 && mismatches() == 0);
  printf("PASS: a different instance without prerequisites fails before any "
         "native device creation\n");
  if (legacy) {
    missing_storage(1);
    device_info.pNext = &features;
    device_info.enabledExtensionCount = 0;
    device_info.ppEnabledExtensionNames = nullptr;
    REQUIRE(create_device(pa, &device_info, nullptr, &device) ==
                VK_ERROR_EXTENSION_NOT_PRESENT &&
            !device);
    REQUIRE(calls() == 1);
    missing_storage(0);
    printf("PASS: missing storage-class dependency aborts the actual native "
           "transaction\n");
  }
  destroy_instance(a, nullptr);
  destroy_instance(b, nullptr);
  VkInstance reused{};
  REQUIRE(create_instance(&instance_info, nullptr, &reused) == VK_SUCCESS &&
          reused == a);
  count = 1;
  REQUIRE(enumerate(reused, &count, &pa) == VK_SUCCESS);
  device_info.pNext = nullptr;
  device_info.enabledExtensionCount = 1;
  device_info.ppEnabledExtensionNames = &extension;
  REQUIRE(create_device(pa, &device_info, nullptr, &device) ==
              VK_ERROR_EXTENSION_NOT_PRESENT &&
          !device);
  REQUIRE(calls() == 1 && mismatches() == 0);
  destroy_instance(reused, nullptr);
  printf("PASS: reused native handles do not retain the destroyed instance's "
         "private extension enablement\n");
  app.apiVersion = VK_API_VERSION_1_2;
  const char *instance_layer = "VK_LAYER_TEST_dependency";
  instance_info.enabledLayerCount = 1;
  instance_info.ppEnabledLayerNames = &instance_layer;
  VkInstance layered{};
  REQUIRE(create_instance(&instance_info, nullptr, &layered) == VK_SUCCESS);
  count = 1;
  REQUIRE(enumerate(layered, &count, &pa) == VK_SUCCESS);
  const char *layer_extension = "VK_TEST_layer_resource";
  device_info.ppEnabledExtensionNames = &layer_extension;
  REQUIRE(create_device(pa, &device_info, nullptr, &device) == VK_SUCCESS && device);
  REQUIRE(calls() == 2 && mismatches() == 0);
  destroy_device(device, nullptr);
  destroy_instance(layered, nullptr);
  printf("PASS: instance-enabled layers contribute native device extensions without device-layer fields\n");
  instance_info.enabledLayerCount = 0;
  instance_info.ppEnabledLayerNames = nullptr;
  REQUIRE(create_instance(&instance_info, nullptr, &layered) == VK_SUCCESS);
  count = 1;
  REQUIRE(enumerate(layered, &count, &pa) == VK_SUCCESS);
  REQUIRE(create_device(pa, &device_info, nullptr, &device) == VK_ERROR_EXTENSION_NOT_PRESENT && !device);
  REQUIRE(calls() == 2 && mismatches() == 0);
  destroy_instance(layered, nullptr);
  printf("PASS: a recreated instance cannot inherit the old instance's layer-provided device extensions\n");
  return 0;
}

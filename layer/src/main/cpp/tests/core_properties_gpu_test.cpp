#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <set>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

static unsigned failures = 0;
static void check(bool ok, const char *name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance{};
    check(vkCreateInstance(&ci, nullptr, &instance) == VK_SUCCESS, "create test instance");
    if (!instance)
        return 1;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    check(count && vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS,
          "enumerate physical device");
    if (devices.empty())
        return 1;
    auto physical = devices.front();
    void *library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    auto raw_query =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
    if (!raw_query)
        raw_query =
            (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2KHR");
    check(raw_query != nullptr, "native properties2 is available");
    if (!raw_query)
        return 1;
    auto base = (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties native_base{};
    base(physical, &native_base);
    const auto native_api = native_base.apiVersion;
    printf("Native driver: %s / Vulkan %u.%u\n", native_base.deviceName,
           VK_API_VERSION_MAJOR(native_api), VK_API_VERSION_MINOR(native_api));
    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)gipa(
        instance, "vkEnumerateDeviceExtensionProperties");
    enumerate(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> values(count);
    enumerate(physical, nullptr, &count, values.data());
    std::set<std::string> extensions, instance_extensions;
    for (auto &e : values)
        extensions.insert(e.extensionName);
    auto enumerate_instance = (PFN_vkEnumerateInstanceExtensionProperties)gipa(
        VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    enumerate_instance(nullptr, &count, nullptr);
    values.resize(count);
    enumerate_instance(nullptr, &count, values.data());
    for (auto &e : values)
        instance_extensions.insert(e.extensionName);

    VkPhysicalDeviceVulkan11Properties actual11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties actual12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    actual11.pNext = &actual12;
    VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &actual11};
    vkGetPhysicalDeviceProperties2(physical, &query);
    check(query.pNext == &actual11 && actual11.pNext == &actual12 && !actual12.pNext,
          "aggregate query restores exact application links");
#include "core_properties_checks.inc"
    check(!strcmp(actual12.driverName, "Vulkan Fix") &&
              strstr(actual12.driverInfo, PROJECT_VERSION_NAME " (" PROJECT_VERSION_CODE ")") &&
              !strchr(actual12.driverInfo, '|'),
          "driver display uses the original short Vulkan Fix build description");
    check((strstr(actual12.driverInfo, "Emulated Vulkan") != nullptr) ==
              (query.properties.apiVersion > native_api),
          "driver display distinguishes promoted Vulkan from the native API version");
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 individual{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(physical, &individual);
    check(!strcmp(driver.driverName, actual12.driverName) &&
              !strcmp(driver.driverInfo, actual12.driverInfo) && driver.driverID == actual12.driverID &&
              !memcmp(&driver.conformanceVersion, &actual12.conformanceVersion,
                      sizeof(driver.conformanceVersion)),
          "aggregate and individual driver queries agree on display and native identity");
    if (native_api < VK_API_VERSION_1_2 &&
        !extensions.count(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
        check(actual12.maxTimelineSemaphoreValueDifference > 0,
              "timeline emulator property is folded into the aggregate");
    printf("Reported driver: %s / %s / conformance %u.%u.%u.%u\n", actual12.driverName,
           actual12.driverInfo, actual12.conformanceVersion.major,
           actual12.conformanceVersion.minor, actual12.conformanceVersion.subminor,
           actual12.conformanceVersion.patch);
    printf("Subgroup: size=%u stages=0x%x operations=0x%x; integer color samples=0x%x\n",
           actual11.subgroupSize, actual11.subgroupSupportedStages,
           actual11.subgroupSupportedOperations, actual12.framebufferIntegerColorSampleCounts);
    auto image_query = (PFN_vkGetPhysicalDeviceImageFormatProperties)gipa(
        instance, "vkGetPhysicalDeviceImageFormatProperties");
    check(actual12.framebufferIntegerColorSampleCounts & VK_SAMPLE_COUNT_1_BIT,
          "integer color attachments support single sampling");
    const VkFormat required[] = {VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
                                 VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
    for (auto format : required) {
        VkImageFormatProperties props{};
        auto result = image_query(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &props);
        check(result == VK_SUCCESS &&
                  !(actual12.framebufferIntegerColorSampleCounts & ~props.sampleCounts),
              "every reported integer sample count is supported by a required integer attachment "
              "format");
    }
    vkDestroyInstance(instance, nullptr);
    dlclose(library);
    printf("Core property GPU failures: %u\n", failures);
    return failures ? 1 : 0;
}

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vulkan/vulkan.h>

namespace {
unsigned failures = 0;
void check(bool ok, const char* message) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
template<class T> T load(PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                        const char* name, const char* suffix) {
    auto proc = (T)gipa(instance, (std::string(name) + suffix).c_str());
    check(proc != nullptr, name);
    return proc;
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    unsetenv("VULKAN_FIX_VALIDATION_DRIVER");
    unsetenv("VULKAN_FIX_RENDERDOC_DRIVER");
    setenv("VULKAN_FIX_DRIVER_PATH", "./libcore11_query_driver.so", 1);
    setenv("VULKAN_FIX_LOG_PATH", "/dev/null", 1);
    auto driver = dlopen("./libcore11_query_driver.so", RTLD_NOW | RTLD_LOCAL);
    auto layer = dlopen("./libvulkanfix.so", RTLD_NOW | RTLD_LOCAL);
    check(driver && layer, "load actual renderer with the strict query provider");
    if (!driver || !layer) { printf("dlerror: %s\n", dlerror()); return 1; }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(layer, "vkGetInstanceProcAddr");
    auto mismatches = (unsigned(*)())dlsym(driver, "core11_query_mismatches");
    if (!gipa || !mismatches) return 1;
    auto create = (PFN_vkCreateInstance)gipa({}, "vkCreateInstance");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance a{}, b{};
    check(create(&info, nullptr, &a) == VK_SUCCESS, "create primary instance for Core 1.1 queries");
    auto enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(a, "vkEnumeratePhysicalDevices");
    VkPhysicalDevice physical{};
    uint32_t count = 1;
    check(enumerate(a, &count, &physical) == VK_SUCCESS && count == 1,
          "remember the primary physical device's owning instance");
    app.apiVersion = VK_API_VERSION_1_0;
    check(create(&info, nullptr, &b) == VK_SUCCESS && a != b,
          "a later instance changes the global last-instance handle");

    for (const char* suffix : {"", "KHR"}) {
        auto families = load<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(gipa, a,
            "vkGetPhysicalDeviceQueueFamilyProperties2", suffix);
        auto sparse = load<PFN_vkGetPhysicalDeviceSparseImageFormatProperties2>(gipa, a,
            "vkGetPhysicalDeviceSparseImageFormatProperties2", suffix);
        if (!families || !sparse) return 1;
        count = 0;
        families(physical, &count, nullptr);
        check(count == 2, "queue families: count query returns both payloads");
        VkQueueFamilyCheckpointPropertiesNV sentinel{VK_STRUCTURE_TYPE_QUEUE_FAMILY_CHECKPOINT_PROPERTIES_NV};
        std::array<VkQueueFamilyProperties2, 3> output{};
        for (auto& o : output) {
            o.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
            o.pNext = &sentinel;
            o.queueFamilyProperties.queueCount = 777;
        }
        count = 0;
        families(physical, &count, output.data());
        check(count == 0 && output[0].queueFamilyProperties.queueCount == 777,
              "queue families: zero capacity returns no payload and performs no out-of-bounds access");
        count = 1;
        families(physical, &count, output.data());
        check(count == 1 && output[0].queueFamilyProperties.queueCount == 17 &&
                  output[0].pNext == &sentinel && output[1].queueFamilyProperties.queueCount == 777,
              "queue families: truncated output preserves headers, owner and unwritten entries");
        count = 3;
        families(physical, &count, output.data());
        check(count == 2 && output[1].queueFamilyProperties.queueCount == 18 &&
                  output[1].pNext == &sentinel && output[2].queueFamilyProperties.queueCount == 777,
              "queue families: full output preserves pNext and unused capacity");
        VkPhysicalDeviceSparseImageFormatInfo2 sparse_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2};
        sparse_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        sparse_info.type = VK_IMAGE_TYPE_2D;
        sparse_info.samples = VK_SAMPLE_COUNT_1_BIT;
        sparse_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        count = 0;
        sparse(physical, &sparse_info, &count, nullptr);
        check(count == 2, "sparse image formats: legacy inventory is not lost");
        std::array<VkSparseImageFormatProperties2, 3> formats{};
        for (auto& o : formats) {
            o.sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2;
            o.properties.imageGranularity.width = 777;
        }
        count = 0;
        sparse(physical, &sparse_info, &count, formats.data());
        check(!count && formats[0].properties.imageGranularity.width == 777,
              "sparse image formats: zero capacity does not become a native count query");
        count = 1;
        sparse(physical, &sparse_info, &count, formats.data());
        check(count == 1 && formats[0].properties.imageGranularity.width == 33 &&
                  formats[1].properties.imageGranularity.width == 777,
              "sparse image formats: truncated output retains the native payload");
        count = 3;
        sparse(physical, &sparse_info, &count, formats.data());
        check(count == 2 && formats[1].properties.imageGranularity.width == 34 &&
                  formats[2].properties.imageGranularity.width == 777,
              "sparse image formats: full output cannot overwrite unused entries");

        auto format = load<PFN_vkGetPhysicalDeviceFormatProperties2>(gipa, a, "vkGetPhysicalDeviceFormatProperties2", suffix);
        auto image = load<PFN_vkGetPhysicalDeviceImageFormatProperties2>(gipa, a, "vkGetPhysicalDeviceImageFormatProperties2", suffix);
        auto memory = load<PFN_vkGetPhysicalDeviceMemoryProperties2>(gipa, a, "vkGetPhysicalDeviceMemoryProperties2", suffix);
        if (!format || !image || !memory) return 1;
        VkFormatProperties2 fp{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        format(physical, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        check(fp.formatProperties.bufferFeatures == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,
              "format properties: native representation survives Core/KHR/legacy dispatch");
        VkPhysicalDeviceImageFormatInfo2 ip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        ip.format = VK_FORMAT_R8G8B8A8_UNORM;
        ip.type = VK_IMAGE_TYPE_2D;
        ip.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImageFormatProperties2 image_out{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        check(image(physical, &ip, &image_out) == VK_SUCCESS && image_out.imageFormatProperties.maxExtent.width == 129,
              "image properties: native output uses the enumerating instance");
        ip.format = VK_FORMAT_UNDEFINED;
        check(image(physical, &ip, &image_out) == VK_ERROR_FORMAT_NOT_SUPPORTED,
              "image properties: native format errors are propagated");
        VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        memory(physical, &mp);
        check(mp.memoryProperties.memoryHeapCount == 1 && mp.memoryProperties.memoryHeaps[0].size == 4096,
              "memory properties: native heap data is preserved");
        setenv("VKFIX_CORE11_QUERY_BAD", "1", 1);
        ip.format = VK_FORMAT_R8G8B8A8_UNORM;
        VkImageFormatProperties2 bad_image{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        check(image(physical, &ip, &bad_image) == VK_SUCCESS &&
                  bad_image.imageFormatProperties.maxExtent.width == 129,
              "image properties: a wrong 1.1 query is repaired from legacy");
        std::array<VkQueueFamilyProperties2, 2> bad_families{};
        for (auto &family : bad_families)
            family.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        uint32_t bad_family_count = 2;
        families(physical, &bad_family_count, bad_families.data());
        check(bad_family_count == 2 &&
                  bad_families[0].queueFamilyProperties.queueCount == 17 &&
                  bad_families[1].queueFamilyProperties.queueCount == 18,
              "queue families: a wrong 1.1 query is repaired from legacy");
        std::array<VkSparseImageFormatProperties2, 2> bad_sparse{};
        for (auto &properties : bad_sparse)
            properties.sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2;
        uint32_t bad_sparse_count = 2;
        sparse(physical, &sparse_info, &bad_sparse_count, bad_sparse.data());
        check(bad_sparse_count == 2 && bad_sparse[0].properties.imageGranularity.width == 33 &&
                  bad_sparse[1].properties.imageGranularity.width == 34,
              "sparse image formats: a wrong 1.1 query is repaired from legacy");
        VkFormatProperties2 bad_format{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        format(physical, VK_FORMAT_R8G8B8A8_UNORM, &bad_format);
        check(bad_format.formatProperties.bufferFeatures == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,
              "format properties: a wrong 1.1 query is repaired from legacy");
        VkPhysicalDeviceMemoryProperties2 bad_memory{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        memory(physical, &bad_memory);
        check(bad_memory.memoryProperties.memoryHeapCount == 1 &&
                  bad_memory.memoryProperties.memoryHeaps[0].size == 4096,
              "memory properties: a wrong 1.1 query is repaired from legacy");
        unsetenv("VKFIX_CORE11_QUERY_BAD");

        auto buffer = load<PFN_vkGetPhysicalDeviceExternalBufferProperties>(gipa, a, "vkGetPhysicalDeviceExternalBufferProperties", suffix);
        auto fence = load<PFN_vkGetPhysicalDeviceExternalFenceProperties>(gipa, a, "vkGetPhysicalDeviceExternalFenceProperties", suffix);
        auto semaphore = load<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(gipa, a, "vkGetPhysicalDeviceExternalSemaphoreProperties", suffix);
        if (!buffer || !fence || !semaphore) return 1;
        VkPhysicalDeviceExternalBufferInfo bi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalBufferProperties bo{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        bo.externalMemoryProperties.externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
        buffer(physical, &bi, &bo);
        VkPhysicalDeviceExternalFenceInfo fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
        fi.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalFenceProperties fo{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
        fo.externalFenceFeatures = VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT;
        fence(physical, &fi, &fo);
        VkPhysicalDeviceExternalSemaphoreInfo si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        si.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalSemaphoreProperties so{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        so.externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        semaphore(physical, &si, &so);
        check(!bo.externalMemoryProperties.externalMemoryFeatures && !fo.externalFenceFeatures && !so.externalSemaphoreFeatures,
              "external capabilities: unsupported handles are initialized deterministically");
    }
    check(mismatches() == 0, "all native query dispatches use the physical device's owning instance");
    auto destroy = (PFN_vkDestroyInstance)gipa(a, "vkDestroyInstance");
    destroy(b, nullptr); destroy(a, nullptr);
    printf("Core 1.1 shared-library query failures: %u\n", failures);
    return failures ? 1 : 0;
}

// Strict instance-bound query provider for the shared-library Core 1.1 tests.
// All data is synthetic; this test library never submits GPU work.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vulkan/vulkan.h>

#define EXPORT extern "C" __attribute__((visibility("default")))
namespace {
unsigned next_instance = 0, mismatches = 0;
int route() {
    const char* value = getenv("VKFIX_CORE11_QUERY_ROUTE");
    return value && !strcmp(value, "core") ? 1 : value && !strcmp(value, "khr") ? 2 : 0;
}
bool bad_queries() {
    const char* value = getenv("VKFIX_CORE11_QUERY_BAD");
    return value && !strcmp(value, "1");
}
uint32_t api() { return route() == 1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0; }
VkResult VKAPI_CALL version(uint32_t* out) { *out = api(); return VK_SUCCESS; }
VkResult VKAPI_CALL create(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* out) {
    *out = (VkInstance)(uintptr_t(0x1000) + ++next_instance);
    return VK_SUCCESS;
}
void VKAPI_CALL destroy(VkInstance, const VkAllocationCallbacks*) {}
VkResult VKAPI_CALL extensions(const char*, uint32_t* count, VkExtensionProperties* out) {
    const char* names[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME};
    if (!out) { *count = 4; return VK_SUCCESS; }
    auto written = std::min(*count, 4u);
    for (uint32_t i = 0; i < written; ++i) {
        out[i] = {};
        strcpy(out[i].extensionName, names[i]);
        out[i].specVersion = 1;
    }
    *count = written;
    return written < 4 ? VK_INCOMPLETE : VK_SUCCESS;
}
VkResult VKAPI_CALL devices(VkInstance instance, uint32_t* count, VkPhysicalDevice* out) {
    if (!out) { *count = 1; return VK_SUCCESS; }
    if (!*count) return VK_INCOMPLETE;
    *out = (VkPhysicalDevice)(uintptr_t(instance) + 0x1000);
    *count = 1;
    return VK_SUCCESS;
}
template<unsigned N> void verify(VkPhysicalDevice physical) {
    mismatches += uintptr_t(physical) != 0x2000 + N;
}
template<unsigned N> void VKAPI_CALL properties(VkPhysicalDevice p, VkPhysicalDeviceProperties* out) {
    verify<N>(p); *out = {}; out->apiVersion = api(); out->deviceID = N;
}
template<unsigned N> void VKAPI_CALL format(VkPhysicalDevice p, VkFormat, VkFormatProperties* out) {
    verify<N>(p); *out = {}; out->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}
template<unsigned N> VkResult VKAPI_CALL image(VkPhysicalDevice p, VkFormat format, VkImageType,
    VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties* out) {
    verify<N>(p);
    if (format == VK_FORMAT_UNDEFINED) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out = {}; out->maxExtent = {128 + N, 64, 1}; out->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    return VK_SUCCESS;
}
template<unsigned N> void VKAPI_CALL families(VkPhysicalDevice p, uint32_t* count, VkQueueFamilyProperties* out) {
    verify<N>(p);
    if (!out) { *count = 2; return; }
    auto written = std::min(*count, 2u);
    for (uint32_t i = 0; i < written; ++i) {
        out[i] = {}; out[i].queueCount = 16 + N + i; out[i].queueFlags = VK_QUEUE_COMPUTE_BIT;
    }
    *count = written;
}
template<unsigned N> void VKAPI_CALL memory(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties* out) {
    verify<N>(p); *out = {}; out->memoryHeapCount = 1; out->memoryHeaps[0].size = 4096 * N;
}
template<unsigned N> void VKAPI_CALL sparse(VkPhysicalDevice p, VkFormat, VkImageType,
    VkSampleCountFlagBits, VkImageUsageFlags, VkImageTiling, uint32_t* count, VkSparseImageFormatProperties* out) {
    verify<N>(p);
    if (!out) { *count = 2; return; }
    auto written = std::min(*count, 2u);
    for (uint32_t i = 0; i < written; ++i) {
        out[i] = {}; out[i].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        out[i].imageGranularity = {32 + N + i, 32, 1};
    }
    *count = written;
}
template<unsigned N> void VKAPI_CALL format2(VkPhysicalDevice p, VkFormat f, VkFormatProperties2* out) {
    format<N>(p, f, &out->formatProperties);
    if (bad_queries()) out->formatProperties.bufferFeatures = 0;
}
template<unsigned N> VkResult VKAPI_CALL image2(VkPhysicalDevice p,
    const VkPhysicalDeviceImageFormatInfo2* info, VkImageFormatProperties2* out) {
    VkResult result = image<N>(p, info->format, info->type, info->tiling, info->usage, info->flags,
                               &out->imageFormatProperties);
    if (result == VK_SUCCESS && bad_queries()) out->imageFormatProperties.maxExtent.width = 1;
    return result;
}
template<unsigned N> void VKAPI_CALL families2(VkPhysicalDevice p, uint32_t* count, VkQueueFamilyProperties2* out) {
    if (!out) { families<N>(p, count, nullptr); return; }
    auto written = std::min(*count, 2u);
    verify<N>(p);
    for (uint32_t i = 0; i < written; ++i) {
        out[i].queueFamilyProperties = {};
        out[i].queueFamilyProperties.queueCount = (bad_queries() && i == 0) ? 0 : 16 + N + i;
        out[i].queueFamilyProperties.queueFlags = VK_QUEUE_COMPUTE_BIT;
    }
    *count = written;
}
template<unsigned N> void VKAPI_CALL memory2(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties2* out) {
    memory<N>(p, &out->memoryProperties);
    if (bad_queries()) out->memoryProperties.memoryHeaps[0].size = 1;
}
template<unsigned N> void VKAPI_CALL sparse2(VkPhysicalDevice p,
    const VkPhysicalDeviceSparseImageFormatInfo2*, uint32_t* count, VkSparseImageFormatProperties2* out) {
    verify<N>(p);
    if (!out) { *count = 2; return; }
    auto written = std::min(*count, 2u);
    for (uint32_t i = 0; i < written; ++i) {
        out[i].properties = {}; out[i].properties.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        out[i].properties.imageGranularity =
            {bad_queries() && i == 0 ? 1u : 32u + N + i, 32, 1};
    }
    *count = written;
}
template<unsigned N> void VKAPI_CALL external_buffer(VkPhysicalDevice p,
    const VkPhysicalDeviceExternalBufferInfo*, VkExternalBufferProperties* out) {
    verify<N>(p); out->externalMemoryProperties = {};
}
template<unsigned N> void VKAPI_CALL external_fence(VkPhysicalDevice p,
    const VkPhysicalDeviceExternalFenceInfo*, VkExternalFenceProperties* out) {
    verify<N>(p); out->compatibleHandleTypes = out->exportFromImportedHandleTypes = 0; out->externalFenceFeatures = 0;
}
template<unsigned N> void VKAPI_CALL external_semaphore(VkPhysicalDevice p,
    const VkPhysicalDeviceExternalSemaphoreInfo*, VkExternalSemaphoreProperties* out) {
    verify<N>(p); out->compatibleHandleTypes = out->exportFromImportedHandleTypes = 0; out->externalSemaphoreFeatures = 0;
}
template<unsigned N> PFN_vkVoidFunction proc(const char* name) {
#define LEGACY(command, function) if (!strcmp(name, #command)) return (PFN_vkVoidFunction)function<N>
    LEGACY(vkGetPhysicalDeviceProperties, properties);
    LEGACY(vkGetPhysicalDeviceFormatProperties, format);
    LEGACY(vkGetPhysicalDeviceImageFormatProperties, image);
    LEGACY(vkGetPhysicalDeviceQueueFamilyProperties, families);
    LEGACY(vkGetPhysicalDeviceMemoryProperties, memory);
    LEGACY(vkGetPhysicalDeviceSparseImageFormatProperties, sparse);
#undef LEGACY
    if (!route()) return nullptr;
#define QUERY(command, function) if (!strcmp(name, route() == 1 ? #command : #command "KHR")) return (PFN_vkVoidFunction)function<N>
    QUERY(vkGetPhysicalDeviceFormatProperties2, format2);
    QUERY(vkGetPhysicalDeviceImageFormatProperties2, image2);
    QUERY(vkGetPhysicalDeviceQueueFamilyProperties2, families2);
    QUERY(vkGetPhysicalDeviceMemoryProperties2, memory2);
    QUERY(vkGetPhysicalDeviceSparseImageFormatProperties2, sparse2);
    QUERY(vkGetPhysicalDeviceExternalBufferProperties, external_buffer);
    QUERY(vkGetPhysicalDeviceExternalFenceProperties, external_fence);
    QUERY(vkGetPhysicalDeviceExternalSemaphoreProperties, external_semaphore);
#undef QUERY
    return nullptr;
}
} // namespace
EXPORT unsigned core11_query_mismatches() { return mismatches; }
EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name) {
#define GLOBAL(command, function) if (!strcmp(name, #command)) return (PFN_vkVoidFunction)function
    GLOBAL(vkGetInstanceProcAddr, vkGetInstanceProcAddr);
    GLOBAL(vkCreateInstance, create);
    GLOBAL(vkEnumerateInstanceExtensionProperties, extensions);
    GLOBAL(vkEnumerateInstanceVersion, version);
    GLOBAL(vkEnumeratePhysicalDevices, devices);
    GLOBAL(vkDestroyInstance, destroy);
#undef GLOBAL
    if (instance == (VkInstance)(uintptr_t)0x1001) return proc<1>(name);
    if (instance == (VkInstance)(uintptr_t)0x1002) return proc<2>(name);
    return nullptr;
}

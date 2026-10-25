#include "core/driver_loader.h"
#include "core/promoted_properties.h"
#include "core/vk_pnext.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

static uint32_t driver_api = VK_API_VERSION_1_1;
static bool allow_properties2 = true;
static bool native_extension = true;
static std::atomic<unsigned> invalid_nodes{0};
static unsigned failures = 0;
static void check(bool condition, const char *name) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition)
        ++failures;
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties *p) {
    *p = {};
    p->apiVersion = driver_api;
    p->vendorID = 0x13b5;
}
static const char *ext_names[] = {VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
                                  VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
                                  VK_KHR_MAINTENANCE3_EXTENSION_NAME};
static VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice, const char *, uint32_t *count,
                                                 VkExtensionProperties *out) {
    unsigned size = native_extension ? std::size(ext_names) : 0;
    if (!out) {
        *count = size;
        return VK_SUCCESS;
    }
    unsigned n = std::min(*count, size);
    for (unsigned i = 0; i < n; ++i) {
        out[i] = {};
        strcpy(out[i].extensionName, ext_names[i]);
    }
    *count = n;
    return n == size ? VK_SUCCESS : VK_INCOMPLETE;
}
static VKAPI_ATTR void VKAPI_CALL properties2(VkPhysicalDevice physical,
                                              VkPhysicalDeviceProperties2 *root) {
    properties(physical, &root->properties);
    for (auto n = (VkBaseOutStructure *)root->pNext; n; n = n->pNext) {
        switch (n->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
            if (driver_api < VK_API_VERSION_1_1) {
                ++invalid_nodes;
                break;
            }
            auto p = (VkPhysicalDeviceIDProperties *)n;
            for (unsigned i = 0; i < VK_UUID_SIZE; ++i) {
                p->deviceUUID[i] = 0x80 + i;
                p->driverUUID[i] = 0xc0 + i;
            }
            memset(p->deviceLUID, 0x43, VK_LUID_SIZE);
            p->deviceLUIDValid = VK_TRUE;
            p->deviceNodeMask = 8;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
            if (driver_api < VK_API_VERSION_1_1) {
                ++invalid_nodes;
                break;
            }
            auto p = (VkPhysicalDeviceSubgroupProperties *)n;
            p->subgroupSize = 16;
            p->supportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
            p->supportedOperations =
                VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES: {
            auto p = (VkPhysicalDeviceMaintenance3Properties *)n;
            p->maxPerSetDescriptors = 12345;
            p->maxMemoryAllocationSize = 0x123456789ULL;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
            auto p = (VkPhysicalDeviceDriverProperties *)n;
            p->driverID = VK_DRIVER_ID_ARM_PROPRIETARY;
            strcpy(p->driverName, "ARM mock driver");
            strcpy(p->driverInfo, "build 98765");
            p->conformanceVersion = {1, 1, 7, 2};
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES: {
            auto p = (VkPhysicalDeviceFloatControlsProperties *)n;
            p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY;
            p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
            p->shaderDenormPreserveFloat32 = VK_TRUE;
            p->shaderRoundingModeRTZFloat16 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES:
            ++invalid_nodes;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
            if (driver_api < VK_API_VERSION_1_2)
                ++invalid_nodes;
            else
                ((VkPhysicalDeviceVulkan11Properties *)n)->subgroupSize = 64;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
            if (driver_api < VK_API_VERSION_1_2)
                ++invalid_nodes;
            else
                ((VkPhysicalDeviceVulkan12Properties *)n)->framebufferIntegerColorSampleCounts =
                    VK_SAMPLE_COUNT_8_BIT;
            break;
        default:
            break;
        }
    }
}
static VKAPI_ATTR void VKAPI_CALL format_properties(VkPhysicalDevice, VkFormat format,
                                                    VkFormatProperties *p) {
    *p = {};
    if (format == VK_FORMAT_R8_UINT || format == VK_FORMAT_R32_UINT)
        p->optimalTilingFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_properties(VkPhysicalDevice, VkFormat format,
                                                       VkImageType, VkImageTiling,
                                                       VkImageUsageFlags, VkImageCreateFlags,
                                                       VkImageFormatProperties *p) {
    *p = {};
    p->sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    if (format == VK_FORMAT_R8_UINT)
        p->sampleCounts |= VK_SAMPLE_COUNT_2_BIT;
    return VK_SUCCESS;
}
extern "C" {
VkInstance get_last_instance() {
    return VK_NULL_HANDLE;
}
PFN_vkGetInstanceProcAddr get_real_instance_proc_addr() {
    return nullptr;
}
void *get_real_proc(VkInstance, VkDevice, const char *name) {
    if (!strcmp(name, "vkGetPhysicalDeviceProperties"))
        return (void *)properties;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties"))
        return (void *)extensions;
    if (allow_properties2 &&
        ((!strcmp(name, "vkGetPhysicalDeviceProperties2") && driver_api >= VK_API_VERSION_1_1) ||
         !strcmp(name, "vkGetPhysicalDeviceProperties2KHR")))
        return (void *)properties2;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties"))
        return (void *)format_properties;
    if (!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties"))
        return (void *)image_properties;
    return nullptr;
}
}
static const auto physical = (VkPhysicalDevice)(uintptr_t)0x123;
int main() {
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDevicePushDescriptorProperties unknown{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES};
    p11.pNext = &unknown;
    unknown.pNext = &p12;
    VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    query.pNext = &p11;
    p12.shaderDenormPreserveFloat64 = 0xdeadbeef;
    {
        PromotedPropertyQuery bridge(physical, &query);
        bridge.query_native();
        auto timeline = vku::find_pnext_mut<VkPhysicalDeviceTimelineSemaphoreProperties>(
            query.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES);
        timeline->maxTimelineSemaphoreValueDifference = 0x987654321ULL;
    }
    check(invalid_nodes == 0,
          "old driver sees only supported child properties, no aggregate structures");
    check(query.pNext == &p11 && p11.pNext == &unknown && unknown.pNext == &p12 && !p12.pNext,
          "exact output chain order including unknown nodes is restored");
    check(p11.deviceUUID[0] == 0x80 && p11.deviceUUID[15] == 0x8f && p11.driverUUID[15] == 0xcf &&
              p11.deviceLUID[7] == 0x43 && p11.deviceLUIDValid && p11.deviceNodeMask == 8,
          "UUID and LUID arrays retain the real driver bytes");
    check(p11.subgroupSize == 16 && p11.subgroupSupportedStages == VK_SHADER_STAGE_COMPUTE_BIT &&
              p11.subgroupSupportedOperations ==
                  (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT),
          "renamed subgroup fields preserve native values and stages");
    check(p11.maxPerSetDescriptors == 12345 && p11.maxMemoryAllocationSize == 0x123456789ULL,
          "descriptor and allocation limits retain full native width");
    check(p12.driverID == VK_DRIVER_ID_ARM_PROPRIETARY &&
              !strcmp(p12.driverName, "ARM mock driver") &&
              !strcmp(p12.driverInfo, "build 98765") && p12.conformanceVersion.patch == 2,
          "driver identity, strings, and conformance version are never fabricated");
    check(p12.shaderDenormPreserveFloat32 && p12.shaderRoundingModeRTZFloat16 &&
              !p12.shaderDenormPreserveFloat64 &&
              p12.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE,
          "float controls preserve independent supported bits and clear stale output");
    check(!p12.supportedDepthResolveModes && !p12.filterMinmaxSingleComponentFormats &&
              !p12.maxUpdateAfterBindDescriptorsInAllPools,
          "unavailable optional property families remain unsupported");
    check(p12.maxTimelineSemaphoreValueDifference == 0x987654321ULL,
          "emulator post-hook properties are folded after the native query");
    check(p12.framebufferIntegerColorSampleCounts ==
              (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT),
          "integer sample counts intersect every supported integer attachment format");

    VkPhysicalDeviceDriverProperties child{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    query.pNext = &child;
    {
        PromotedPropertyQuery bridge(physical, &query);
        bridge.query_native();
    }
    check(!strcmp(child.driverName, p12.driverName) && child.driverID == p12.driverID,
          "individual and aggregate property queries agree");

    driver_api = VK_API_VERSION_1_0;
    query.pNext = &p11;
    p11.pNext = &p12;
    {
        PromotedPropertyQuery bridge(physical, &query);
        bridge.query_native();
    }
    check(invalid_nodes == 0 && !p11.subgroupSize && !p11.deviceUUID[0] &&
              p11.maxPerSetDescriptors == 12345 && p12.driverID == VK_DRIVER_ID_ARM_PROPRIETARY,
          "1.0 KHR query uses only advertised extension structures");

    native_extension = false;
    {
        PromotedPropertyQuery bridge(physical, &query);
        bridge.query_native();
    }
    check(!p12.driverID && !p12.driverName[0] && !p11.maxMemoryAllocationSize &&
              !p11.subgroupSupportedStages,
          "missing 1.0 extensions do not invent UUIDs, driver identity, or limits");
    allow_properties2 = false;
    {
        PromotedPropertyQuery bridge(physical, &query);
        bridge.query_native();
    }
    check(query.properties.vendorID == 0x13b5 && query.pNext == &p11 && p11.pNext == &p12,
          "legacy properties fallback retains base output and restores links");
    allow_properties2 = true;

    driver_api = VK_API_VERSION_1_2;
    {
        PromotedPropertyQuery bridge(physical, &query);
        bridge.query_native();
    }
    check(p11.subgroupSize == 64 &&
              p12.framebufferIntegerColorSampleCounts == VK_SAMPLE_COUNT_8_BIT &&
              invalid_nodes == 0,
          "native 1.2 aggregate values pass through without reduction");

    driver_api = VK_API_VERSION_1_1;
    native_extension = true;
    std::atomic<unsigned> thread_failures{0};
    auto run = [&] {
        for (unsigned i = 0; i < 100; ++i) {
            VkPhysicalDeviceVulkan11Properties p{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
            VkPhysicalDeviceProperties2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p};
            {
                PromotedPropertyQuery outer(physical, &q);
                outer.query_native();
                VkPhysicalDeviceProperties2 nested{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                PromotedPropertyQuery inner(physical, &nested);
                inner.query_native();
            }
            if (p.subgroupSize != 16 || q.pNext != &p || p.pNext)
                ++thread_failures;
        }
    };
    std::thread a(run), b(run);
    a.join();
    b.join();
    check(thread_failures == 0, "nested and concurrent property queries have independent storage");
    printf("Property promotion failures: %u\n", failures);
    return failures ? 1 : 0;
}

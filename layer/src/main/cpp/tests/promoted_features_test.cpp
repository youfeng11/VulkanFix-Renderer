#include "core/driver_loader.h"
#include "core/promoted_features.h"
#include "core/promoted_properties.h"
#include "core/native_instance_state.h"
#include "core/vk_pnext.h"
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <cstdlib>

static uint32_t driver_api = VK_API_VERSION_1_1;
static uint32_t subgroup_size = 16;
static bool strict_queries = false;
static bool hide_multiview = false;
static uint32_t strict_target = VK_API_VERSION_1_0;
static unsigned core_features_calls = 0, khr_features_calls = 0;
static unsigned core_properties_calls = 0, khr_properties_calls = 0, forbidden_nodes = 0;
static VKAPI_ATTR void VKAPI_CALL properties2(VkPhysicalDevice, VkPhysicalDeviceProperties2* root) {
    for (auto n = (VkBaseOutStructure*)root->pNext; n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
            auto p = (VkPhysicalDeviceSubgroupProperties*)n;
            p->subgroupSize = subgroup_size;
            p->supportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
            p->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;
        }
        else if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES)
            ((VkPhysicalDeviceMultiviewProperties*)n)->maxMultiviewViewCount = 6;
}
static int failures = 0;
static void check(bool ok, const char *name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
static const char *supported_extensions[] = {
    VK_KHR_8BIT_STORAGE_EXTENSION_NAME,        VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME,
    VK_KHR_MULTIVIEW_EXTENSION_NAME,
};
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties *p) {
    *p = {};
    p->apiVersion = driver_api;
}
static VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice, const char *, uint32_t *count,
                                                 VkExtensionProperties *values) {
    const char* names[sizeof(supported_extensions) / sizeof(supported_extensions[0])];
    uint32_t size = 0;
    for (auto name : supported_extensions)
        if (!hide_multiview || strcmp(name, VK_KHR_MULTIVIEW_EXTENSION_NAME)) names[size++] = name;
    if (!values) {
        *count = size;
        return VK_SUCCESS;
    }
    const uint32_t written = std::min(*count, size);
    for (uint32_t i = 0; i < written; ++i) {
        values[i] = {};
        strcpy(values[i].extensionName, names[i]);
        values[i].specVersion = 1;
    }
    *count = written;
    return written == size ? VK_SUCCESS : VK_INCOMPLETE;
}
static VKAPI_ATTR void VKAPI_CALL features2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *root) {
    root->features = {};
    for (auto n = (VkBaseOutStructure *)root->pNext; n; n = n->pNext)
        switch (n->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
            if (driver_api >= VK_API_VERSION_1_1)
                ((VkPhysicalDevice16BitStorageFeatures *)n)->storageBuffer16BitAccess = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
            if (driver_api >= VK_API_VERSION_1_1)
                ((VkPhysicalDeviceProtectedMemoryFeatures*)n)->protectedMemory = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            if (driver_api >= VK_API_VERSION_1_2)
                ((VkPhysicalDeviceVulkan11Features*)n)->protectedMemory = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
            ((VkPhysicalDeviceMultiviewFeatures *)n)->multiview = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
            ((VkPhysicalDeviceVariablePointersFeatures *)n)->variablePointersStorageBuffer =
                VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
            if (driver_api >= VK_API_VERSION_1_1)
                ((VkPhysicalDeviceShaderDrawParametersFeatures *)n)->shaderDrawParameters = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
            ((VkPhysicalDevice8BitStorageFeatures *)n)->storageBuffer8BitAccess = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
            ((VkPhysicalDeviceShaderFloat16Int8Features *)n)->shaderInt8 = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
            auto f = (VkPhysicalDeviceDescriptorIndexingFeatures *)n;
            f->runtimeDescriptorArray = VK_TRUE;
            f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            break;
        }
        default:
            break;
        }
}
static void inspect_feature_chain(VkPhysicalDeviceFeatures2* root) {
    for (auto n = (VkBaseOutStructure*)root->pNext; n; n = n->pNext) {
        if (strict_target < VK_API_VERSION_1_2 && (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
            n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) ++forbidden_nodes;
        if (strict_target < VK_API_VERSION_1_1 && n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES)
            ++forbidden_nodes;
    }
}
static VKAPI_ATTR void VKAPI_CALL strict_features_core(VkPhysicalDevice p,VkPhysicalDeviceFeatures2* root) {
    ++core_features_calls; inspect_feature_chain(root); features2(p,root);
}
static VKAPI_ATTR void VKAPI_CALL strict_features_khr(VkPhysicalDevice p,VkPhysicalDeviceFeatures2* root) {
    ++khr_features_calls; inspect_feature_chain(root); features2(p,root);
}
static void inspect_property_chain(VkPhysicalDeviceProperties2* root) {
    for (auto n = (VkBaseOutStructure*)root->pNext; n; n = n->pNext) {
        if (strict_target < VK_API_VERSION_1_2 && (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES ||
            n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES)) ++forbidden_nodes;
        if (strict_target < VK_API_VERSION_1_1 && (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES ||
            n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES ||
            n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES)) ++forbidden_nodes;
    }
}
static VKAPI_ATTR void VKAPI_CALL strict_properties_core(VkPhysicalDevice p,VkPhysicalDeviceProperties2* root) {
    ++core_properties_calls; inspect_property_chain(root); properties(p,&root->properties); properties2(p,root);
}
static VKAPI_ATTR void VKAPI_CALL strict_properties_khr(VkPhysicalDevice p,VkPhysicalDeviceProperties2* root) {
    ++khr_properties_calls; inspect_property_chain(root); properties(p,&root->properties); properties2(p,root);
}
static VKAPI_ATTR void VKAPI_CALL legacy_features(VkPhysicalDevice,VkPhysicalDeviceFeatures* out) {
    *out = {}; out->robustBufferAccess = VK_TRUE;
}
extern "C" {
void init_real_vulkan() {}
void register_vulkan_ptr() {}
VkInstance get_last_instance() {
    return VK_NULL_HANDLE;
}
void set_last_instance(VkInstance) {}
PFN_vkGetInstanceProcAddr get_real_instance_proc_addr() {
    return nullptr;
}
PFN_vkGetDeviceProcAddr get_real_device_proc_addr() {
    return nullptr;
}
void *get_real_proc(VkInstance, VkDevice, const char *name) {
    if (strict_queries) {
        if (!strcmp(name,"vkGetPhysicalDeviceFeatures2")) return (void*)strict_features_core;
        if (!strcmp(name,"vkGetPhysicalDeviceFeatures2KHR")) return (void*)strict_features_khr;
        if (!strcmp(name,"vkGetPhysicalDeviceProperties2")) return (void*)strict_properties_core;
        if (!strcmp(name,"vkGetPhysicalDeviceProperties2KHR")) return (void*)strict_properties_khr;
        if (!strcmp(name,"vkGetPhysicalDeviceFeatures")) return (void*)legacy_features;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceProperties"))
        return (void *)properties;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties"))
        return (void *)extensions;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2") && driver_api >= VK_API_VERSION_1_1)
        return (void*)properties2;
    if ((!strcmp(name, "vkGetPhysicalDeviceFeatures2") && driver_api >= VK_API_VERSION_1_1) ||
        !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
        return (void *)features2;
    return nullptr;
}
}
static const VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)0x11;
static bool has_extension(const std::vector<const char *> &extensions, const char *name) {
    return std::count_if(extensions.begin(), extensions.end(),
                         [&](const char *e) { return !strcmp(e, name); }) == 1;
}
int main() {
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f11.pNext = &f12;
    query.pNext = &f11;
    {
        PromotedFeatureQuery promotion(physical, &query);
        check(!vku::has_pnext(query.pNext, f11.sType) && !vku::has_pnext(query.pNext, f12.sType),
              "old driver receives individual feature structures, never 1.2 aggregates");
        features2(physical, &query);
        auto timeline = vku::find_pnext<VkPhysicalDeviceTimelineSemaphoreFeatures>(
            query.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        auto reset = vku::find_pnext<VkPhysicalDeviceHostQueryResetFeatures>(
            query.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
        timeline->timelineSemaphore = VK_TRUE;
        reset->hostQueryReset = VK_TRUE;
    }
    check(query.pNext == &f11 && f11.pNext == &f12 && f12.pNext == nullptr,
          "query restores the exact caller chain order");
    check(f11.storageBuffer16BitAccess && !f11.uniformAndStorageBuffer16BitAccess &&
              f11.multiview && !f11.multiviewGeometryShader && f11.variablePointersStorageBuffer &&
              !f11.variablePointers,
          "all independent 1.1 bits retain the driver's values");
    check(f12.shaderInt8 && !f12.shaderFloat16 && f12.storageBuffer8BitAccess &&
              !f12.uniformAndStorageBuffer8BitAccess,
          "extension presence never enables unsupported scalar/storage feature bits");
    check(f12.runtimeDescriptorArray && f12.shaderSampledImageArrayNonUniformIndexing &&
              !f12.descriptorBindingPartiallyBound && !f12.descriptorIndexing,
          "partial indexing support does not pretend to meet the 1.2 minimum");
    check(f12.timelineSemaphore && f12.hostQueryReset,
          "module emulation results are folded into aggregates after post-hooks");
    check(f12.subgroupBroadcastDynamicId,
          "native 1.1 subgroup properties enable the dynamic broadcast lowering");

    VkPhysicalDeviceVulkan12Features broadcast_request{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    broadcast_request.subgroupBroadcastDynamicId = VK_TRUE;
    VkDeviceCreateInfo broadcast_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &broadcast_request};
    std::vector<const char*> broadcast_extensions;
    {
        PromotedDeviceFeatures promotion(physical, broadcast_info, broadcast_extensions);
        check(promotion.result() == VK_SUCCESS && !broadcast_info.pNext && broadcast_extensions.empty(),
              "dynamic broadcast enablement is consumed without sending a false native feature bit");
    }
    subgroup_size = 0;
    broadcast_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &broadcast_request};
    {
        PromotedDeviceFeatures promotion(physical, broadcast_info, broadcast_extensions);
        check(promotion.result() == VK_ERROR_FEATURE_NOT_PRESENT,
              "missing native subgroup properties reject dynamic broadcast enablement");
    }
    subgroup_size = 16;

    f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f11.pNext = &f12;
    f11.multiview = VK_TRUE;
    f12.shaderInt8 = VK_TRUE;
    f12.storageBuffer8BitAccess = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    VkDeviceCreateInfo original{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    original.pNext = &f11;
    const auto saved11 = f11;
    const auto saved12 = f12;
    std::vector<const char *> enabled{VK_KHR_8BIT_STORAGE_EXTENSION_NAME};
    auto created = original;
    {
        PromotedDeviceFeatures promotion(physical, created, enabled);
        check(promotion.result() == VK_SUCCESS,
              "independent int8-only and indexing-subset requests are supported");
        auto numeric = vku::find_pnext<VkPhysicalDeviceShaderFloat16Int8Features>(
            created.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
        auto indexing = vku::find_pnext<VkPhysicalDeviceDescriptorIndexingFeatures>(
            created.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
        check(numeric && numeric->shaderInt8 && !numeric->shaderFloat16 && indexing &&
                  indexing->runtimeDescriptorArray && !indexing->descriptorBindingPartiallyBound,
              "device promotion enables exactly the requested native bits");
        check(has_extension(enabled, VK_KHR_8BIT_STORAGE_EXTENSION_NAME) &&
                  has_extension(enabled, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) &&
                  has_extension(enabled, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME),
              "required native extension names are added once");
        check(!memcmp(&f11, &saved11, sizeof(f11)) && !memcmp(&f12, &saved12, sizeof(f12)) &&
                  original.pNext == &f11,
              "application input bytes and links remain unchanged while device creation is active");
        auto nested_info = original;
        std::vector<const char *> nested_extensions;
        PromotedDeviceFeatures nested(physical, nested_info, nested_extensions);
        check(nested.result() == VK_SUCCESS && nested_info.pNext != created.pNext,
              "nested device creations own different promotion nodes");
    }
    check(!memcmp(&f11, &saved11, sizeof(f11)) && !memcmp(&f12, &saved12, sizeof(f12)),
          "input remains unchanged after nested creation cleanup");
    std::atomic<unsigned> concurrent_ok{0};
    auto concurrent = [&] {
        auto info = original;
        std::vector<const char *> extensions;
        PromotedDeviceFeatures promotion(physical, info, extensions);
        if (promotion.result() == VK_SUCCESS && info.pNext != original.pNext)
            ++concurrent_ok;
    };
    std::thread a(concurrent), b(concurrent);
    a.join();
    b.join();
    check(concurrent_ok == 2 && !memcmp(&f11, &saved11, sizeof(f11)),
          "concurrent promotion never writes shared caller memory");
    f12.shaderFloat16 = VK_TRUE;
    created = original;
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical, created, enabled);
        check(promotion.result() == VK_ERROR_FEATURE_NOT_PRESENT,
              "unsupported enabled bit is rejected even when its extension exists");
    }
    f12.shaderFloat16 = VK_FALSE;
    driver_api = VK_API_VERSION_1_2;
    created = original;
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical, created, enabled);
        check(promotion.result() == VK_SUCCESS && vku::has_pnext(created.pNext, f11.sType) &&
                  vku::has_pnext(created.pNext, f12.sType) && created.pNext != original.pNext,
              "native 1.2 keeps aggregate semantics but still owns its input copies");
    }
    driver_api = VK_API_VERSION_1_0;
    VkPhysicalDeviceVulkan11Features legacy{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    legacy.multiview = VK_TRUE;
    VkDeviceCreateInfo legacy_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    legacy_info.pNext = &legacy;
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical, legacy_info, enabled);
        check(promotion.result() == VK_SUCCESS &&
                  has_extension(enabled, VK_KHR_MULTIVIEW_EXTENSION_NAME),
              "1.0 driver receives the extension needed by a promoted 1.1 feature");
    }
    VkPhysicalDeviceFeatures2 legacy_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    legacy_query.pNext = &legacy;
    {
        PromotedFeatureQuery promotion(physical, &legacy_query);
        features2(physical, &legacy_query);
    }
    check(legacy.multiview && !legacy.storageBuffer16BitAccess && !legacy.shaderDrawParameters,
          "1.0/KHR query does not infer unsupported core capabilities");
    VkPhysicalDeviceFeatures2 legacy_base{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    legacy_base.features.robustBufferAccess = VK_TRUE;
    legacy_base.pNext = &legacy;
    const auto saved_base = legacy_base;
    legacy_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    legacy_info.pNext = &legacy_base;
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical, legacy_info, enabled);
        check(promotion.result() == VK_SUCCESS && legacy_info.pEnabledFeatures &&
                  legacy_info.pEnabledFeatures->robustBufferAccess &&
                  !vku::has_pnext(legacy_info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2),
              "1.0 device creation translates Features2 base bits to legacy pEnabledFeatures");
        check(!memcmp(&legacy_base, &saved_base, sizeof(legacy_base)),
              "legacy Features2 translation never mutates the application's input");
    }
    driver_api = VK_API_VERSION_1_2;
    strict_queries = true;
    const auto query_instance = (VkInstance)(uintptr_t)0x99;
    auto context = [&](uint32_t target,uint32_t loader,std::vector<const char*> names) {
        strict_target = target;
        core_features_calls = khr_features_calls = core_properties_calls = khr_properties_calls = forbidden_nodes = 0;
        vkfix::remember_native_instance(query_instance,target,loader,names,{},VK_API_VERSION_1_2);
        vkfix::remember_physical_devices(query_instance,1,&physical);
    };
    context(VK_API_VERSION_1_0,VK_API_VERSION_1_3,{});
    check(!native_features2_proc(physical) && !native_properties2_proc(physical),
          "known legacy instance ignores both nonnull unenabled query spellings");
    VkPhysicalDeviceVulkan11Features guarded{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceHostQueryResetFeatures tail{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
    guarded.pNext = &tail;
    VkPhysicalDeviceFeatures2 guarded_root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&guarded};
    {
        PromotedFeatureQuery promotion(physical,&guarded_root);
        promotion.query_native();
    }
    check(guarded_root.features.robustBufferAccess && !guarded.multiview && !guarded.protectedMemory &&
              !core_features_calls && !khr_features_calls && guarded_root.pNext == &guarded && guarded.pNext == &tail,
          "legacy feature queries preserve base bits and headers without manufacturing newer physical-device features");
    context(VK_API_VERSION_1_0,VK_API_VERSION_1_3,{VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME});
    guarded = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES}; guarded.pNext = &tail;
    guarded_root = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&guarded};
    {
        PromotedFeatureQuery promotion(physical,&guarded_root);
        promotion.query_native();
    }
    check(!core_features_calls && khr_features_calls && !forbidden_nodes && guarded.multiview && !guarded.protectedMemory &&
              guarded_root.pNext == &guarded && guarded.pNext == &tail && !tail.pNext,
          "native 1.0 KHR query filters unavailable core-only nodes, retains available multiview and restores the complete chain");
    VkPhysicalDeviceVulkan11Properties guarded_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceDriverProperties property_tail{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    guarded_properties.pNext = &property_tail;
    VkPhysicalDeviceProperties2 properties_root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&guarded_properties};
    {
        PromotedPropertyQuery promotion(physical,&properties_root);
        check(promotion.native_api_version() == VK_API_VERSION_1_0,
              "native property transport ceiling follows application target rather than newer physical version");
        promotion.query_native();
    }
    check(!core_properties_calls && khr_properties_calls && !forbidden_nodes && guarded_properties.maxMultiviewViewCount == 6 &&
              properties_root.pNext == &guarded_properties && guarded_properties.pNext == &property_tail && !property_tail.pNext,
          "KHR property query preserves supported payloads and headers without forwarding newer aggregates or core-only nodes");
    VkPhysicalDeviceProtectedMemoryFeatures protected_request{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES};
    protected_request.protectedMemory = VK_TRUE;
    VkDeviceCreateInfo protected_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&protected_request};
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical,protected_info,enabled);
        check(promotion.result() == VK_ERROR_FEATURE_NOT_PRESENT && protected_request.protectedMemory && !protected_request.pNext,
              "native 1.0 rejects protected-memory enablement even when the physical GPU advertises a newer API");
    }
    VkPhysicalDeviceVulkan11Features multiview_request{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    multiview_request.multiview = VK_TRUE;
    VkDeviceCreateInfo multiview_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&multiview_request};
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical,multiview_info,enabled);
        check(promotion.result() == VK_SUCCESS && has_extension(enabled,VK_KHR_MULTIVIEW_EXTENSION_NAME) &&
                  vku::has_pnext(multiview_info.pNext,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) && !multiview_request.pNext,
              "available native KHR multiview remains enableable through owned individual features on a 1.0 target");
    }
    const auto* previousSoftwareMode = std::getenv("VULKAN_FIX_SOFTWARE_MULTIVIEW");
    const bool hadSoftwareMode = previousSoftwareMode != nullptr;
    const std::string savedSoftwareMode = previousSoftwareMode ? previousSoftwareMode : "";
    hide_multiview = true;
    context(VK_API_VERSION_1_0, VK_API_VERSION_1_3, {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME});
    setenv("VULKAN_FIX_SOFTWARE_MULTIVIEW", "0", 1);
    multiview_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &multiview_request};
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical, multiview_info, enabled);
        check(promotion.result() == VK_ERROR_FEATURE_NOT_PRESENT,
              "explicit opt-out rejects multiview when no native backend exists");
    }
    unsetenv("VULKAN_FIX_SOFTWARE_MULTIVIEW");
    multiview_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &multiview_request};
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical, multiview_info, enabled);
        const auto* owned = vku::find_pnext<VkPhysicalDeviceMultiviewFeatures>(multiview_info.pNext,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES);
        check(promotion.result() == VK_SUCCESS && owned && owned->multiview &&
                  !has_extension(enabled, VK_KHR_MULTIVIEW_EXTENSION_NAME) && multiview_request.multiview && !multiview_request.pNext,
              "default software multiview owns the request without enabling an absent native extension or modifying input");
    }
    multiview_request.multiviewGeometryShader = VK_TRUE;
    multiview_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &multiview_request};
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical, multiview_info, enabled);
        check(promotion.result() == VK_ERROR_FEATURE_NOT_PRESENT && multiview_request.multiviewGeometryShader,
              "basic software admission does not invent the optional multiview geometry-shader feature");
    }
    multiview_request.multiviewGeometryShader = VK_FALSE;
    hide_multiview = false;
    multiview_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &multiview_request};
    enabled.clear();
    {
        PromotedDeviceFeatures promotion(physical, multiview_info, enabled);
        check(promotion.result() == VK_SUCCESS && has_extension(enabled, VK_KHR_MULTIVIEW_EXTENSION_NAME),
              "software admission preserves a supported native KHR multiview backend");
    }
    if (hadSoftwareMode) setenv("VULKAN_FIX_SOFTWARE_MULTIVIEW", savedSoftwareMode.c_str(), 1);
    else unsetenv("VULKAN_FIX_SOFTWARE_MULTIVIEW");
    context(VK_API_VERSION_1_2,VK_API_VERSION_1_1,{});
    guarded = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    guarded_root = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&guarded};
    {
        PromotedFeatureQuery promotion(physical,&guarded_root);
        promotion.query_native();
    }
    check(core_features_calls && !khr_features_calls && guarded.protectedMemory && !forbidden_nodes &&
              vkfix::limit_native_device_api(physical,driver_api) == VK_API_VERSION_1_2,
          "device API1.2 functionality remains available through a Core1.1 instance when the application actually targets1.2");
    vkfix::forget_native_instance(query_instance);
    strict_queries = false;
    printf("Promotion failures: %d\n", failures);
    return failures ? 1 : 0;
}

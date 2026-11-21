#include "core/feature_request_validation.h"
#include "core/vk_pnext.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
bool fail_allocation = false, throw_in_query = false, owned_chain = true;
unsigned failures = 0, queries = 0;
const void* original_node = nullptr;
void check(bool value, const char* message) { printf("%s: %s\n", value ? "PASS" : "FAIL", message); failures += !value; }
VKAPI_ATTR void VKAPI_CALL query(VkPhysicalDevice, VkPhysicalDeviceFeatures2* out) {
    ++queries;
    if (throw_in_query) throw std::bad_alloc();
    out->features.robustBufferAccess = VK_TRUE;
    for (auto node = static_cast<VkBaseOutStructure*>(out->pNext); node; node = node->pNext) {
        owned_chain &= node != original_node && node->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO;
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT)
            reinterpret_cast<VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(node)->transformFeedback = VK_TRUE;
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT)
            reinterpret_cast<VkPhysicalDeviceProvokingVertexFeaturesEXT*>(node)->provokingVertexLast = VK_TRUE;
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
            reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(node)->timelineSemaphore = VK_TRUE;
    }
}
}
void* operator new(size_t size) {
    if (fail_allocation) throw std::bad_alloc();
    if (auto p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
int main() {
    using vkfix::validate_device_features;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDeviceGroupDeviceCreateInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO};
    info.pNext = &group;
    fail_allocation = true;
    auto empty = validate_device_features({}, info, nullptr);
    fail_allocation = false;
    check(empty == VK_SUCCESS && !queries, "feature-free device creation requires neither allocation nor a feature query");
    VkPhysicalDeviceFeatures base{}; base.robustBufferAccess = VK_TRUE; info.pEnabledFeatures = &base;
    check(validate_device_features({}, info, query) == VK_SUCCESS, "supported base feature remains enableable");
    base.geometryShader = VK_TRUE;
    check(validate_device_features({}, info, query) == VK_ERROR_FEATURE_NOT_PRESENT && base.geometryShader,
          "unsupported base feature is rejected without modifying the request");
    base.geometryShader = VK_FALSE;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT transform{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    transform.transformFeedback = VK_TRUE; group.pNext = &transform; original_node = &transform;
    check(validate_device_features({}, info, query) == VK_SUCCESS && owned_chain && group.pNext == &transform && transform.transformFeedback,
          "publicly supported extension feature survives validation and only owned feature nodes reach the query");
    transform.geometryStreams = VK_TRUE;
    check(validate_device_features({}, info, query) == VK_ERROR_FEATURE_NOT_PRESENT && transform.geometryStreams,
          "every extension feature bit is checked, including unsupported trailing fields");
    VkPhysicalDeviceProvokingVertexFeaturesEXT provoking{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    provoking.provokingVertexLast = VK_TRUE; provoking.transformFeedbackPreservesProvokingVertex = VK_TRUE;
    info.pNext = &provoking;
    check(validate_device_features({}, info, query) == VK_ERROR_FEATURE_NOT_PRESENT,
          "mixed supported and unsupported provoking-vertex flags cannot be silently stripped");
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.features.robustBufferAccess = VK_TRUE;
    info.pEnabledFeatures = nullptr; info.pNext = &features2;
    check(validate_device_features({}, info, query) == VK_SUCCESS && features2.features.robustBufferAccess,
          "Features2 base enablement uses the same public support contract");
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR};
    timeline.timelineSemaphore = VK_TRUE; features2.pNext = &timeline;
    check(validate_device_features({}, info, query) == VK_SUCCESS && features2.pNext == &timeline,
          "promoted KHR alias request can use an advertised emulated feature");
    VkPhysicalDeviceVulkan11Features aggregate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    aggregate.protectedMemory = VK_TRUE; info.pNext = &aggregate;
    check(validate_device_features({}, info, query) == VK_ERROR_FEATURE_NOT_PRESENT,
          "aggregate feature requests receive individual-bit validation too");
    info.pNext = &transform;
    const unsigned before = queries;
    fail_allocation = true;
    auto oom = validate_device_features({}, info, query);
    fail_allocation = false;
    check(oom == VK_ERROR_OUT_OF_HOST_MEMORY && queries == before && transform.geometryStreams && !transform.pNext,
          "preparation OOM returns before querying and preserves caller memory");
    throw_in_query = true;
    auto query_oom = validate_device_features({}, info, query);
    throw_in_query = false;
    check(query_oom == VK_ERROR_OUT_OF_HOST_MEMORY && transform.geometryStreams && !transform.pNext,
          "query OOM releases private nodes and leaves original links intact");
    transform.pNext = &transform;
    check(validate_device_features({}, info, query) == VK_ERROR_INITIALIZATION_FAILED && transform.pNext == &transform,
          "cyclic requests are rejected before cloning or querying");
    transform.pNext = nullptr;
    printf("Feature request validation failures: %u\n", failures);
    return failures ? 1 : 0;
}

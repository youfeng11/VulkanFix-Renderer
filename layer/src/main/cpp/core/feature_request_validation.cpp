#include "feature_request_validation.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <vector>

namespace vkfix {
namespace {
using FeatureNode = std::unique_ptr<void, void (*)(void*)>;
template<class T, VkStructureType Type> FeatureNode make_feature() {
    auto result = new T{};
    result->sType = Type;
    return FeatureNode(result, [](void* p) { delete static_cast<T*>(p); });
}
struct FeatureDescriptor {
    VkStructureType type;
    const size_t* fields;
    size_t count;
    FeatureNode (*make)();
};
#include "feature_request_metadata.inc"
VkBool32 flag(const void* value, size_t offset) {
    VkBool32 result;
    std::memcpy(&result, static_cast<const char*>(value) + offset, sizeof(result));
    return result;
}
bool any_requested(const void* value, const size_t* fields, size_t count) {
    if (!value) return false;
    for (size_t i = 0; i < count; ++i) if (flag(value, fields[i])) return true;
    return false;
}
bool supported(const void* request, const void* available, const size_t* fields, size_t count) {
    if (!request) return true;
    for (size_t i = 0; i < count; ++i)
        if (flag(request, fields[i]) && !flag(available, fields[i])) return false;
    return true;
}
struct Request {
    const void* original;
    const FeatureDescriptor* descriptor;
    FeatureNode output;
};
}
VkResult validate_device_features(VkPhysicalDevice physical, const VkDeviceCreateInfo& info,
                                 PFN_vkGetPhysicalDeviceFeatures2 query) {
    try {
        auto slow = static_cast<const VkBaseInStructure*>(info.pNext), fast = slow;
        while (fast && fast->pNext) {
            slow = slow->pNext; fast = fast->pNext->pNext;
            if (slow == fast) return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkPhysicalDeviceFeatures* features2_base = nullptr;
        std::vector<Request> requests;
        for (auto node = static_cast<const VkBaseInStructure*>(info.pNext); node; node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                features2_base = &reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->features;
                continue;
            }
            auto descriptor = std::find_if(std::begin(feature_descriptors), std::end(feature_descriptors),
                [&](const FeatureDescriptor& value) { return node->sType == value.type; });
            if (descriptor != std::end(feature_descriptors) && any_requested(node, descriptor->fields, descriptor->count))
                requests.push_back({node, descriptor, descriptor->make()});
        }
        if (requests.empty() && !any_requested(info.pEnabledFeatures, base_fields, std::size(base_fields)) &&
            !any_requested(features2_base, base_fields, std::size(base_fields))) return VK_SUCCESS;
        if (!query) return VK_ERROR_INITIALIZATION_FAILED;
        VkPhysicalDeviceFeatures2 output{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkBaseOutStructure* previous = nullptr;
        for (auto& request : requests) {
            auto node = static_cast<VkBaseOutStructure*>(request.output.get());
            if (previous) previous->pNext = node;
            else output.pNext = node;
            previous = node;
        }
        query(physical, &output);
        if (!supported(info.pEnabledFeatures, &output.features, base_fields, std::size(base_fields)) ||
            !supported(features2_base, &output.features, base_fields, std::size(base_fields))) return VK_ERROR_FEATURE_NOT_PRESENT;
        for (const auto& request : requests)
            if (!supported(request.original, request.output.get(), request.descriptor->fields, request.descriptor->count))
                return VK_ERROR_FEATURE_NOT_PRESENT;
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
}

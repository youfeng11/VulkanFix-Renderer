#include "device_group_routes.h"
#include "driver_loader.h"
#include "legacy_query_array.h"
#include "native_instance_state.h"
#include "singleton_device_group.h"
#include "vk_pnext.h"
#include <algorithm>
#include <new>

namespace vkfix {
namespace {
template<class T> T proc(VkInstance instance,const char* name) { return (T)get_real_proc(instance,VK_NULL_HANDLE,name); }
VkResult native_group_member(VkInstance instance,VkPhysicalDevice physical,bool& supported) {
    const auto versions = native_instance_versions(instance);
    return native_device_group_support(physical,proc<PFN_vkGetPhysicalDeviceProperties>(instance,"vkGetPhysicalDeviceProperties"),
        proc<PFN_vkEnumerateDeviceExtensionProperties>(instance,"vkEnumerateDeviceExtensionProperties"),supported,
        versions.public_api >= VK_API_VERSION_1_1 ? versions.native_api : UINT32_MAX);
}
}
VkResult enumerate_layer_device_groups(VkInstance instance,uint32_t* count,VkPhysicalDeviceGroupProperties* output) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!instance) instance = get_last_instance();
    try {
        const auto route = native_group_creation_route(instance);
        PFN_vkEnumeratePhysicalDeviceGroups enumerate = nullptr;
        if (route == NativeGroupCreationRoute::Core) enumerate = proc<PFN_vkEnumeratePhysicalDeviceGroups>(instance,"vkEnumeratePhysicalDeviceGroups");
        else if (route == NativeGroupCreationRoute::KHR) enumerate = proc<PFN_vkEnumeratePhysicalDeviceGroups>(instance,"vkEnumeratePhysicalDeviceGroupsKHR");
        if (!enumerate) return enumerate_singleton_device_groups(instance,count,output,
            proc<PFN_vkEnumeratePhysicalDevices>(instance,"vkEnumeratePhysicalDevices"));
        std::vector<VkPhysicalDeviceGroupProperties> native;
        for (;;) {
            uint32_t size = 0; auto result = enumerate(instance,&size,nullptr);
            if (result != VK_SUCCESS) return result;
            native.assign(size,VkPhysicalDeviceGroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES});
            if (!size) break;
            result = enumerate(instance,&size,native.data());
            if (result == VK_INCOMPLETE) continue;
            if (result != VK_SUCCESS) return result;
            native.resize(size); break;
        }
        std::vector<VkPhysicalDeviceGroupProperties> published;
        const bool public_core = native_instance_versions(instance).public_api >= VK_API_VERSION_1_1;
        for (const auto& group : native) {
            if (!group.physicalDeviceCount || group.physicalDeviceCount > VK_MAX_DEVICE_GROUP_SIZE) return VK_ERROR_INITIALIZATION_FAILED;
            bool complete = true;
            for (uint32_t i = 0; public_core && i < group.physicalDeviceCount; ++i) {
                bool supported = false; const auto status = native_group_member(instance,group.physicalDevices[i],supported);
                if (status != VK_SUCCESS) return status;
                complete &= supported;
            }
            if (complete) published.push_back(group);
            else for (uint32_t i = 0; i < group.physicalDeviceCount; ++i) {
                VkPhysicalDeviceGroupProperties single{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES};
                single.physicalDeviceCount = 1; single.physicalDevices[0] = group.physicalDevices[i];
                published.push_back(single);
            }
        }
        if (!output) { *count = uint32_t(published.size()); return VK_SUCCESS; }
        const auto written = std::min<size_t>(*count,published.size());
        for (size_t i = 0; i < written; ++i) {
            output[i].physicalDeviceCount = published[i].physicalDeviceCount;
            std::copy(std::begin(published[i].physicalDevices),std::end(published[i].physicalDevices),std::begin(output[i].physicalDevices));
            output[i].subsetAllocation = published[i].subsetAllocation;
        }
        *count = uint32_t(written);
        return written == published.size() ? VK_SUCCESS : VK_INCOMPLETE;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult prepare_device_group_creation(VkInstance instance,VkPhysicalDevice physical,const VkDeviceCreateInfo& application,
    VkDeviceCreateInfo& native,bool native_group_operations,VkDeviceGroupDeviceCreateInfo& storage) {
    const auto* requested = vku::find_pnext<VkDeviceGroupDeviceCreateInfo>(application.pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO);
    if (!requested) return VK_SUCCESS;
    const auto route = native_group_creation_route(instance);
    if (requested->physicalDeviceCount > 1) {
        bool requires_operations = native_instance_versions(instance).public_api >= VK_API_VERSION_1_1;
        for (uint32_t i = 0; i < application.enabledExtensionCount; ++i)
            requires_operations |= std::strcmp(application.ppEnabledExtensionNames[i],VK_KHR_DEVICE_GROUP_EXTENSION_NAME) == 0;
        if (route == NativeGroupCreationRoute::Singleton || (requires_operations && !native_group_operations)) return VK_ERROR_FEATURE_NOT_PRESENT;
        for (uint32_t i = 0; requires_operations && i < requested->physicalDeviceCount; ++i) {
            bool supported = false; const auto status = native_group_member(instance,requested->pPhysicalDevices[i],supported);
            if (status != VK_SUCCESS) return status;
            if (!supported) return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    } else if (requested->physicalDeviceCount == 1 && requested->pPhysicalDevices[0] != physical)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    // Module hooks may already have unlinked their owned copy. Always rebuild
    // this one node from the immutable application input, preserving ordering
    // of its member array while leaving all other feature nodes intact.
    vku::unlink_pnext(native.pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO);
    if (route != NativeGroupCreationRoute::Singleton) {
        storage = *requested; storage.pNext = native.pNext; native.pNext = &storage;
    }
    return VK_SUCCESS;
}
}

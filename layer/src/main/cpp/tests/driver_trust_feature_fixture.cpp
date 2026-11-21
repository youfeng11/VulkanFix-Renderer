// Test-only provider: exercise an installed renderer with its advertised
// vertex-store feature enabled, using the same legal GPU probe as raw Vulkan.
// No application process/configuration or production worker is changed.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

namespace {
PFN_vkGetInstanceProcAddr backend() {
    static auto result=[] {
        const char *path=std::getenv("VKFIX_TRUST_FEATURE_BACKEND");
        auto library=path ? dlopen(path,RTLD_NOW|RTLD_LOCAL) : nullptr;
        return library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr")) : nullptr;
    }();
    return result;
}
PFN_vkCreateDevice create{};
PFN_vkGetPhysicalDeviceFeatures query{};
VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,
                                            const VkAllocationCallbacks *allocator,VkDevice *out) {
    if (!info || !create || !query) return VK_ERROR_INITIALIZATION_FAILED;
    for (auto next=static_cast<const VkBaseInStructure*>(info->pNext);next;next=next->pNext)
        if (next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDeviceFeatures supported{}; query(physical,&supported);
    VkPhysicalDeviceFeatures enabled=info->pEnabledFeatures ? *info->pEnabledFeatures : VkPhysicalDeviceFeatures{};
    enabled.vertexPipelineStoresAndAtomics=supported.vertexPipelineStoresAndAtomics;
    auto modified=*info; modified.pEnabledFeatures=&enabled;
    std::printf("VKFIX_TEST_FEATURE:vertexPipelineStoresAndAtomics=%u\n",enabled.vertexPipelineStoresAndAtomics);
    return create(physical,&modified,allocator,out);
}
}
extern "C" __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    auto gipa=backend();
    if (!gipa || !name) return nullptr;
    if (!std::strcmp(name,"vkCreateDevice")) {
        create=reinterpret_cast<PFN_vkCreateDevice>(gipa(instance,name));
        query=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(gipa(instance,"vkGetPhysicalDeviceFeatures"));
        return reinterpret_cast<PFN_vkVoidFunction>(create_device);
    }
    return gipa(instance,name);
}

#include "core/device_group_routes.h"
#include "core/native_instance_state.h"
#include "core/vk_pnext.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

static std::atomic<bool> fail_new{false};
void* operator new(size_t size) { if (fail_new.exchange(false)) throw std::bad_alloc(); if (auto p = std::malloc(size ? size : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,size_t) noexcept { std::free(p); }
using namespace vkfix;
namespace {
template<class T> T handle(uint64_t value) { return (T)(uintptr_t)value; }
const auto legacy = handle<VkInstance>(0x100), khr = handle<VkInstance>(0x200), core = handle<VkInstance>(0x300), creation_only = handle<VkInstance>(0x201);
const VkPhysicalDevice physical[] = {handle<VkPhysicalDevice>(0x400),handle<VkPhysicalDevice>(0x401),handle<VkPhysicalDevice>(0x402)};
unsigned failures = 0, core_calls = 0, khr_calls = 0, invalid_calls = 0;
uint32_t physical_api = VK_API_VERSION_1_0;
bool supported[] = {true,true,false}; bool incomplete_once = false;
VkResult group_status = VK_SUCCESS, extension_status = VK_SUCCESS;
void check(bool okay,const char* label) { printf("%s: %s\n",okay ? "PASS" : "FAIL",label); failures += !okay; }
VKAPI_ATTR VkResult VKAPI_CALL enumerate_devices(VkInstance,uint32_t* count,VkPhysicalDevice* out) {
    if (!out) { *count = 3; return VK_SUCCESS; }
    auto written = std::min(*count,3u); for (uint32_t i = 0; i < written; ++i) out[i] = physical[i]; *count = written;
    return written == 3 ? VK_SUCCESS : VK_INCOMPLETE;
}
VkResult groups(uint32_t* count,VkPhysicalDeviceGroupProperties* out) {
    if (group_status != VK_SUCCESS) return group_status;
    if (!out) { *count = 2; return VK_SUCCESS; }
    if (incomplete_once) { incomplete_once = false; *count = 0; return VK_INCOMPLETE; }
    const auto written = std::min(*count,2u);
    for (uint32_t i = 0; i < written; ++i) {
        check(out[i].sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES && !out[i].pNext,"native snapshot uses valid private group headers");
        out[i].physicalDeviceCount = i ? 1 : 2; out[i].physicalDevices[0] = physical[i ? 2 : 0];
        if (!i) out[i].physicalDevices[1] = physical[1]; out[i].subsetAllocation = !i;
    }
    *count = written; return written == 2 ? VK_SUCCESS : VK_INCOMPLETE;
}
VKAPI_ATTR VkResult VKAPI_CALL core_groups(VkInstance instance,uint32_t* count,VkPhysicalDeviceGroupProperties* out) {
    ++core_calls; if (instance != core) { ++invalid_calls; return VK_ERROR_INITIALIZATION_FAILED; } return groups(count,out);
}
VKAPI_ATTR VkResult VKAPI_CALL khr_groups(VkInstance instance,uint32_t* count,VkPhysicalDeviceGroupProperties* out) {
    ++khr_calls; if (instance != khr && instance != creation_only) { ++invalid_calls; return VK_ERROR_INITIALIZATION_FAILED; } return groups(count,out);
}
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice,VkPhysicalDeviceProperties* out) { *out = {}; out->apiVersion = physical_api; }
VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice device,const char*,uint32_t* count,VkExtensionProperties* out) {
    if (extension_status != VK_SUCCESS) return extension_status;
    bool group = false; for (uint32_t i = 0; i < 3; ++i) if (device == physical[i]) group = supported[i];
    if (!out) { *count = group ? 1 : 0; return VK_SUCCESS; }
    if (group && *count) { out[0] = {}; std::strcpy(out[0].extensionName,VK_KHR_DEVICE_GROUP_EXTENSION_NAME); *count = 1; }
    else *count = 0; return VK_SUCCESS;
}
}
extern "C" VkInstance get_last_instance() { return legacy; }
extern "C" void* get_real_proc(VkInstance,VkDevice,const char* name) {
#define PROC(n,f) if (!std::strcmp(name,n)) return (void*)f
    PROC("vkEnumeratePhysicalDevices",enumerate_devices); PROC("vkEnumeratePhysicalDeviceGroups",core_groups);
    PROC("vkEnumeratePhysicalDeviceGroupsKHR",khr_groups); PROC("vkGetPhysicalDeviceProperties",properties);
    PROC("vkEnumerateDeviceExtensionProperties",extensions);
#undef PROC
    return nullptr;
}
int main() {
    remember_native_instance(legacy,VK_API_VERSION_1_0,VK_API_VERSION_1_0,{}, {},VK_API_VERSION_1_1);
    remember_native_instance(khr,VK_API_VERSION_1_0,VK_API_VERSION_1_0,{VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME},{},VK_API_VERSION_1_1);
    remember_native_instance(core,VK_API_VERSION_1_1,VK_API_VERSION_1_2,{});
    remember_native_instance(creation_only,VK_API_VERSION_1_0,VK_API_VERSION_1_0,{VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME});
    uint32_t count = 0;
    check(enumerate_layer_device_groups(legacy,&count,nullptr) == VK_SUCCESS && count == 3 && !core_calls && !khr_calls,
        "unenabled native core/KHR pointers are ignored on the singleton instance route");
    check(enumerate_layer_device_groups(khr,&count,nullptr) == VK_SUCCESS && count == 2 && !core_calls && khr_calls,
        "KHR creation extension selects only the KHR enumeration entry");
    VkPhysicalDeviceGroupProperties output[4]{}; VkBaseOutStructure sentinel{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    for (auto& g : output) { g.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES; g.pNext = &sentinel; g.physicalDeviceCount = 99; }
    count = 4;
    check(enumerate_layer_device_groups(khr,&count,output) == VK_SUCCESS && count == 2 && output[0].physicalDeviceCount == 2 &&
        output[0].physicalDevices[0] == physical[0] && output[0].physicalDevices[1] == physical[1] && output[0].subsetAllocation &&
        output[1].physicalDeviceCount == 1 && output[0].pNext == &sentinel && output[2].physicalDeviceCount == 99,
        "native groups retain member ordering, subset support and caller-owned output headers");
    supported[1] = false; count = 0;
    check(enumerate_layer_device_groups(khr,&count,nullptr) == VK_SUCCESS && count == 3,"a group lacking native public operations is published as its singleton members");
    count = 1; output[1].physicalDeviceCount = 99;
    check(enumerate_layer_device_groups(khr,&count,output) == VK_INCOMPLETE && count == 1 && output[0].physicalDeviceCount == 1 &&
        output[0].physicalDevices[0] == physical[0] && output[1].physicalDeviceCount == 99,
        "flattened group enumeration honors capacity and VK_INCOMPLETE without overwriting the next output");
    count = 0; output[0].physicalDeviceCount = 99;
    check(enumerate_layer_device_groups(khr,&count,output) == VK_INCOMPLETE && !count && output[0].physicalDeviceCount == 99,
        "zero-capacity group output remains distinct from a count-only query");
    group_status = VK_ERROR_OUT_OF_DEVICE_MEMORY; count = 4;
    check(enumerate_layer_device_groups(khr,&count,output) == group_status && count == 4 && output[0].physicalDeviceCount == 99,
        "native enumeration failure never publishes a partial group snapshot"); group_status = VK_SUCCESS;
    extension_status = VK_ERROR_OUT_OF_HOST_MEMORY;
    check(enumerate_layer_device_groups(khr,&count,output) == extension_status && output[0].physicalDeviceCount == 99,
        "native member capability-query failure does not invent fallback groups"); extension_status = VK_SUCCESS;
    fail_new = true;
    check(enumerate_layer_device_groups(khr,&count,output) == VK_ERROR_OUT_OF_HOST_MEMORY && !fail_new && output[0].physicalDeviceCount == 99,
        "group snapshot OOM is returned without changing caller payloads");
    incomplete_once = true; count = 4;
    check(enumerate_layer_device_groups(khr,&count,output) == VK_SUCCESS && count == 3 && !incomplete_once,
        "a changed native count is retried before publishing the final topology");
    physical_api = VK_API_VERSION_1_1; count = 0;
    check(enumerate_layer_device_groups(core,&count,nullptr) == VK_SUCCESS && count == 2 && core_calls && !invalid_calls,
        "native Core creation uses its core function and preserves supported multi-device groups");
    supported[0] = supported[1] = supported[2] = false; count = 0;
    check(enumerate_layer_device_groups(khr,&count,nullptr) == VK_SUCCESS && count == 3,
        "public Core1.1 through a native1.0 instance cannot infer enabled device operations from physical apiVersion alone");
    const VkPhysicalDevice reversed[] = {physical[1],physical[0]};
    VkDeviceGroupDeviceCreateInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO}; group.physicalDeviceCount = 2; group.pPhysicalDevices = reversed;
    VkPhysicalDeviceMultiviewFeatures source_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES}; source_feature.pNext = &group;
    VkDeviceCreateInfo application{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; application.pNext = &source_feature;
    VkPhysicalDeviceMultiviewFeatures owned_feature = source_feature; owned_feature.pNext = nullptr; // Model a module stripping its own clone.
    VkDeviceCreateInfo native = application; native.pNext = &owned_feature; VkDeviceGroupDeviceCreateInfo storage{};
    check(prepare_device_group_creation(core,physical[0],application,native,true,storage) == VK_SUCCESS && native.pNext == &storage &&
        storage.physicalDeviceCount == 2 && storage.pPhysicalDevices == reversed && storage.pNext == &owned_feature && !owned_feature.pNext,
        "enabled native creation restores the entire ordered group after module hooks stripped their private node");
    check(application.pNext == &source_feature && source_feature.pNext == &group && group.pPhysicalDevices == reversed,
        "group creation preparation never mutates the application's feature links or member array");
    native = application; native.pNext = &owned_feature;
    check(prepare_device_group_creation(legacy,physical[0],application,native,false,storage) == VK_ERROR_FEATURE_NOT_PRESENT && native.pNext == &owned_feature,
        "a multi-device request cannot silently become a singleton on an unavailable creation route");
    check(prepare_device_group_creation(khr,physical[0],application,native,false,storage) == VK_ERROR_FEATURE_NOT_PRESENT,
        "public Core groups cannot be created without the native operations promised by their topology");
    check(prepare_device_group_creation(creation_only,physical[0],application,native,false,storage) == VK_SUCCESS && storage.physicalDeviceCount == 2,
        "legacy KHR creation-only usage remains valid without enabling the independent device-group operations extension");
    physical_api = VK_API_VERSION_1_0; count = 0;
    check(enumerate_layer_device_groups(creation_only,&count,nullptr) == VK_SUCCESS && count == 2 &&
        prepare_device_group_creation(creation_only,physical[0],application,native,false,storage) == VK_SUCCESS,
        "native1.0 KHR creation-only groups do not require the separate device operations capability");
    group.physicalDeviceCount = 1; group.pPhysicalDevices = &physical[0]; owned_feature.pNext = nullptr; native.pNext = &owned_feature;
    fail_new = true;
    check(prepare_device_group_creation(legacy,physical[0],application,native,false,storage) == VK_SUCCESS && native.pNext == &owned_feature && fail_new,
        "singleton fallback removes only its native group node without allocating"); fail_new = false;
    group.physicalDeviceCount = 0; group.pPhysicalDevices = reinterpret_cast<const VkPhysicalDevice*>(uintptr_t(1));
    check(prepare_device_group_creation(legacy,physical[0],application,native,false,storage) == VK_SUCCESS,
        "zero physicalDeviceCount uses the primary device and ignores its unused pointer");
    forget_native_instance(core);
    check(native_group_creation_route(core) == NativeGroupCreationRoute::Singleton,"retired instance routes cannot be reused by stale handles");
    forget_native_instance(legacy); forget_native_instance(khr); forget_native_instance(creation_only);
    printf("Device group route failures: %u\n",failures); return failures ? 1 : 0;
}

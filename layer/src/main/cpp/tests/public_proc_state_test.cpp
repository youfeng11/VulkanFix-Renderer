#include "core/public_proc_state.h"
#include <cstdio>
#include <cstdlib>
#include <new>

namespace { bool fail_allocation = false; unsigned failures = 0;
unsigned inventories = 0;
void check(bool ok, const char* text) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }
}
void* operator new(std::size_t size) {
    if (fail_allocation) throw std::bad_alloc();
    if (auto p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main() {
    using namespace vkfix;
    auto a = (VkInstance)(uintptr_t)1, b = (VkInstance)(uintptr_t)2;
    auto da = (VkDevice)(uintptr_t)3, db = (VkDevice)(uintptr_t)4;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const char* debug = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    instance.enabledExtensionCount = 1; instance.ppEnabledExtensionNames = &debug;
    check(remember_public_instance(a, instance) == VK_SUCCESS, "remember enabled instance extension");
    instance.enabledExtensionCount = 0;
    check(remember_public_instance(b, instance) == VK_SUCCESS, "remember independent extension-free instance");
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    check(remember_public_device(da, a, device) == VK_SUCCESS, "remember extension-free device");
    const char* shader = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
    device.enabledExtensionCount = 1; device.ppEnabledExtensionNames = &shader;
    check(remember_public_device(db, b, device) == VK_SUCCESS, "remember alternate owner of shared extension command");
    auto allowed = [&](VkDevice d, const char* name) { return public_device_proc_allowed(d, *proc_metadata(name)); };
    check(allowed(da, "vkCmdDispatchBase") && !allowed(da, "vkCmdDispatchBaseKHR"), "core promotion does not enable its extension spelling");
    check(allowed(da, "vkCmdBeginDebugUtilsLabelEXT") && !allowed(db, "vkCmdBeginDebugUtilsLabelEXT"), "device commands inherit only their own instance enablement");
    check(allowed(db, "vkCmdBindVertexBuffers2EXT") && !allowed(da, "vkCmdBindVertexBuffers2EXT"), "any explicitly enabled extension owner authorizes a shared command");
    check(!allowed(da, "vkGetPhysicalDeviceProperties") && !allowed(da, "vkCreateInstance") && !allowed(da, "vkCreateDevice"), "device query excludes physical, instance and global commands");
    check(public_instance_proc_allowed({}, *proc_metadata("vkCreateInstance"), nullptr) &&
          public_instance_proc_allowed({}, *proc_metadata("vkGetInstanceProcAddr"), nullptr) &&
          !public_instance_proc_allowed({}, *proc_metadata("vkDestroyDevice"), nullptr), "null instance exposes only global commands and GIPA");
    check(!public_instance_proc_allowed(b, *proc_metadata("vkGetPhysicalDeviceProperties2KHR"), nullptr), "private native promotion dependency cannot enable public KHR query");
    auto available = [](VkInstance i, std::vector<std::string>& out) {
        ++inventories;
        if (i == (VkInstance)(uintptr_t)2) out.emplace_back(VK_KHR_DEVICE_GROUP_EXTENSION_NAME);
        return VK_SUCCESS;
    };
    check(public_instance_proc_allowed(b, *proc_metadata("vkCmdDispatchBaseKHR"), available) &&
          !public_instance_proc_allowed(a, *proc_metadata("vkCmdDispatchBaseKHR"), available), "GIPA checks availability across its own physical devices, independently of device enablement");
    fail_allocation = true;
    const auto cached = public_instance_proc_allowed(b, *proc_metadata("vkCmdSetDeviceMaskKHR"), available);
    fail_allocation = false;
    check(cached && inventories == 2, "GIPA reuses the instance's extension inventory without allocating or re-enumerating");
    invalidate_public_device_extensions(b);
    check(public_instance_proc_allowed(b, *proc_metadata("vkCmdDispatchBaseKHR"), available) && inventories == 3,
          "physical-device enumeration invalidates cached extension availability");
    fail_allocation = true;
    auto status = remember_public_device(db, a, device);
    bool prior = allowed(db, "vkCmdBindVertexBuffers2EXT") && !allowed(db, "vkCmdBeginDebugUtilsLabelEXT");
    fail_allocation = false;
    check(status == VK_ERROR_OUT_OF_HOST_MEMORY && prior, "OOM leaves existing public context intact and device queries allocate nothing");
    forget_public_device(db);
    check(!allowed(db, "vkCmdBindVertexBuffers2EXT"), "destroy removes old device extension ownership");
    device.enabledExtensionCount = 0;
    check(remember_public_device(db, b, device) == VK_SUCCESS && !allowed(db, "vkCmdBindVertexBuffers2EXT"), "reused handle cannot inherit destroyed device extensions");
    forget_public_device(da); forget_public_device(db); forget_public_instance(a); forget_public_instance(b);
    std::printf("Public proc state failures: %u\n", failures);
    return failures ? 1 : 0;
}

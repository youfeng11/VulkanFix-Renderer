#include "public_proc_state.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>

namespace vkfix {
namespace {
constexpr ProcMetadata commands[] = {
#include "proc_metadata.inc"
};
using Extensions = std::set<std::string, std::less<>>;
std::mutex mutex;
struct Instance { Extensions enabled; std::optional<Extensions> available_device_extensions; uint64_t revision = 0; };
std::map<VkInstance, Instance> instances;
struct Device { VkInstance instance; Extensions extensions; };
std::map<VkDevice, Device> devices;
Extensions copy_extensions(uint32_t count, const char* const* names) {
    Extensions result;
    for (uint32_t i = 0; i < count; ++i) result.emplace(names[i]);
    return result;
}
template<class Predicate> bool any_owner(const char* owners, Predicate predicate) {
    std::string_view remaining(owners);
    while (!remaining.empty()) {
        auto end = remaining.find('|');
        auto owner = remaining.substr(0, end);
        if (predicate(owner)) return true;
        if (end == std::string_view::npos) break;
        remaining.remove_prefix(end + 1);
    }
    return false;
}
bool enabled(const Extensions& extensions, const char* owners) {
    return any_owner(owners, [&](std::string_view owner) { return extensions.find(owner) != extensions.end(); });
}
} // namespace
const ProcMetadata* proc_metadata(const char* name) {
    if (!name) return nullptr;
    auto found = std::lower_bound(std::begin(commands), std::end(commands), name,
        [](const ProcMetadata& command, const char* key) { return std::strcmp(command.name, key) < 0; });
    return found != std::end(commands) && !std::strcmp(found->name, name) ? found : nullptr;
}
VkResult remember_public_instance(VkInstance instance, const VkInstanceCreateInfo& info) noexcept {
    try {
        auto extensions = copy_extensions(info.enabledExtensionCount, info.ppEnabledExtensionNames);
        std::lock_guard<std::mutex> lock(mutex);
        instances.insert_or_assign(instance, Instance{std::move(extensions), {}});
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult remember_public_device(VkDevice device, VkInstance instance, const VkDeviceCreateInfo& info) noexcept {
    try {
        Device value{instance, copy_extensions(info.enabledExtensionCount, info.ppEnabledExtensionNames)};
        std::lock_guard<std::mutex> lock(mutex);
        devices.insert_or_assign(device, std::move(value));
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
void forget_public_instance(VkInstance instance) {
    std::lock_guard<std::mutex> lock(mutex);
    instances.erase(instance);
}
void forget_public_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(mutex);
    devices.erase(device);
}
void invalidate_public_device_extensions(VkInstance instance) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = instances.find(instance);
    if (found != instances.end()) {
        ++found->second.revision;
        found->second.available_device_extensions.reset();
    }
}
bool public_device_proc_allowed(VkDevice device, const ProcMetadata& command) {
    if (!device || command.scope != ProcScope::Device) return false;
    if (command.core) return true;
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = devices.find(device);
    if (found == devices.end()) return false;
    if (enabled(found->second.extensions, command.device_extensions)) return true;
    const auto instance = instances.find(found->second.instance);
    return instance != instances.end() && enabled(instance->second.enabled, command.instance_extensions);
}
bool public_instance_proc_allowed(VkInstance instance, const ProcMetadata& command,
                                  DeviceExtensionInventory inventory) {
    if (!instance)
        return command.scope == ProcScope::Global || !std::strcmp(command.name, "vkGetInstanceProcAddr");
    if (command.core) return true;
    uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = instances.find(instance);
        if (found == instances.end()) return false;
        if (enabled(found->second.enabled, command.instance_extensions)) return true;
        if (!*command.device_extensions) return false;
        if (found->second.available_device_extensions)
            return enabled(*found->second.available_device_extensions, command.device_extensions);
        revision = found->second.revision;
    }
    // Enumerating native devices/extensions can re-enter public registration.
    // Never call the provider while holding the state lock.
    if (!inventory) return false;
    try {
        std::vector<std::string> values;
        if (inventory(instance, values) != VK_SUCCESS) return false;
        Extensions available(values.begin(), values.end());
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = instances.find(instance);
        if (found == instances.end()) return false;
        if (found->second.revision != revision) return enabled(available, command.device_extensions);
        found->second.available_device_extensions = std::move(available);
        return enabled(*found->second.available_device_extensions, command.device_extensions);
    } catch (const std::bad_alloc&) { return false; }
}
} // namespace vkfix

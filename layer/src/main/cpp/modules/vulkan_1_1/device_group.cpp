#include "device_group.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include "legacy_query_array.h"
#include <algorithm>

REGISTER_LAYER_MODULE(DeviceGroupModule);

DeviceGroupModule::DeviceGroupModule()
    : ExtensionModuleBase(VK_KHR_DEVICE_GROUP_EXTENSION_NAME, VK_KHR_DEVICE_GROUP_SPEC_VERSION, VK_API_VERSION_1_1) {
    LOGI("DeviceGroupModule initialized");
}

void DeviceGroupModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    ExtensionModuleBase::on_enumerate_device_extensions(physicalDevice, extensions);

    // device_group_creation is an instance extension. Multiview is an
    // independent native feature; singleton groups do not emulate its shaders
    // or layered render passes and must not manufacture that capability.

}

void DeviceGroupModule::on_pre_create_device_custom(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    if (pCreateInfo)
        vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO);
    // Preserve native multiview's extension and feature enablement even when
    // device_group itself is emulated; the two capabilities are independent.

}

// The promotion bridge queries real limits; no synthetic multiview or allocation limits.
void DeviceGroupModule::on_pre_get_properties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*, void*& data) { data = nullptr; }
void DeviceGroupModule::on_post_get_properties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*, void*) {}


bool DeviceGroupModule::on_cmd_set_device_mask(
    VkCommandBuffer commandBuffer,
    uint32_t deviceMask
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    PFN_vkCmdSetDeviceMask real_fn =
        (PFN_vkCmdSetDeviceMask) get_real_proc(get_last_instance(), device, "vkCmdSetDeviceMask");
    if (!real_fn) {
        real_fn = (PFN_vkCmdSetDeviceMask) get_real_proc(get_last_instance(), device, "vkCmdSetDeviceMaskKHR");
    }
    if (real_fn) {
        real_fn(commandBuffer, deviceMask);
    }
    // Singleton fallback: the only valid mask is 1, already implicit in 1.0.
    return true;
}

bool DeviceGroupModule::on_get_device_group_peer_memory_features(
    VkDevice device,
    uint32_t heapIndex,
    uint32_t localDeviceIndex,
    uint32_t remoteDeviceIndex,
    VkPeerMemoryFeatureFlags* pPeerMemoryFeatures
) {
    if (is_device_native(device)) return false;

    PFN_vkGetDeviceGroupPeerMemoryFeatures real_fn =
        (PFN_vkGetDeviceGroupPeerMemoryFeatures) get_real_proc(get_last_instance(), device, "vkGetDeviceGroupPeerMemoryFeatures");
    if (!real_fn) {
        real_fn = (PFN_vkGetDeviceGroupPeerMemoryFeatures) get_real_proc(get_last_instance(), device, "vkGetDeviceGroupPeerMemoryFeaturesKHR");
    }
    if (real_fn) {
        real_fn(device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
        return true;
    }

    if (pPeerMemoryFeatures) {
        *pPeerMemoryFeatures = 0;
    }
    return true;
}

bool DeviceGroupModule::on_cmd_dispatch_base(
    VkCommandBuffer commandBuffer,
    uint32_t baseGroupX,
    uint32_t baseGroupY,
    uint32_t baseGroupZ,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    PFN_vkCmdDispatchBase real_fn =
        (PFN_vkCmdDispatchBase) get_real_proc(get_last_instance(), device, "vkCmdDispatchBase");
    if (!real_fn) {
        real_fn = (PFN_vkCmdDispatchBase) get_real_proc(get_last_instance(), device, "vkCmdDispatchBaseKHR");
    }
    if (real_fn) {
        real_fn(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
        return true;
    }

    if (baseGroupX == 0 && baseGroupY == 0 && baseGroupZ == 0) {
        PFN_vkCmdDispatch real_disp =
            (PFN_vkCmdDispatch) get_real_proc(get_last_instance(), device, "vkCmdDispatch");
        if (real_disp) {
            real_disp(commandBuffer, groupCountX, groupCountY, groupCountZ);
        }
        return true;
    }
    return false;
}

bool DeviceGroupModule::on_enumerate_physical_device_groups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties,
    VkResult& outResult
) {
    VkInstance inst = (instance != VK_NULL_HANDLE) ? instance : get_last_instance();
    PFN_vkEnumeratePhysicalDeviceGroups real_fn =
        (PFN_vkEnumeratePhysicalDeviceGroups) get_real_proc(inst, VK_NULL_HANDLE, "vkEnumeratePhysicalDeviceGroups");
    if (!real_fn) {
        real_fn = (PFN_vkEnumeratePhysicalDeviceGroups) get_real_proc(inst, VK_NULL_HANDLE, "vkEnumeratePhysicalDeviceGroupsKHR");
    }
    if (real_fn) {
        outResult = real_fn(inst, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
        return true;
    }

    PFN_vkEnumeratePhysicalDevices real_epd =
        (PFN_vkEnumeratePhysicalDevices) get_real_proc(inst, VK_NULL_HANDLE, "vkEnumeratePhysicalDevices");
    if (!real_epd) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    outResult = vkfix::enumerate_singleton_device_groups(
        inst, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties, real_epd);
    return true;
}

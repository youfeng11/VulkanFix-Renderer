#include "device_group.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
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

    if (!is_phys_device_native(physicalDevice)) {
        if (!vku::has_extension(extensions, VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = VK_KHR_DEVICE_GROUP_CREATION_SPEC_VERSION;
            extensions.push_back(prop);
        }
        if (!vku::has_extension(extensions, VK_KHR_MULTIVIEW_EXTENSION_NAME)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = VK_KHR_MULTIVIEW_SPEC_VERSION;
            extensions.push_back(prop);
        }
    }
}

void DeviceGroupModule::on_pre_create_device_custom(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    for (auto it = enabledExtensions.begin(); it != enabledExtensions.end();) {
        if (strcmp(*it, VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME) == 0 ||
            strcmp(*it, VK_KHR_MULTIVIEW_EXTENSION_NAME) == 0) {
            it = enabledExtensions.erase(it);
        } else {
            ++it;
        }
    }

    if (pCreateInfo) {
        vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO);
        vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES);
    }
}

void DeviceGroupModule::on_pre_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pProperties) return;

    pUserData = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES);
}

void DeviceGroupModule::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties || !pUserData) return;

    auto* p = reinterpret_cast<VkPhysicalDeviceMultiviewProperties*>(pUserData);
    p->maxMultiviewViewCount = 6;
    p->maxMultiviewInstanceIndex = 134217727;
    vku::relink_pnext(pProperties->pNext, pUserData);
}

void DeviceGroupModule::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    pUserData = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES);
}

void DeviceGroupModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures || !pUserData) return;

    auto* f = reinterpret_cast<VkPhysicalDeviceMultiviewFeatures*>(pUserData);
    f->multiview = VK_TRUE;
    f->multiviewGeometryShader = VK_FALSE;
    f->multiviewTessellationShader = VK_FALSE;
    vku::relink_pnext(pFeatures->pNext, pUserData);
}

void DeviceGroupModule::on_cmd_set_device_mask(
    VkCommandBuffer commandBuffer,
    uint32_t deviceMask
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return;

    PFN_vkCmdSetDeviceMask real_fn =
        (PFN_vkCmdSetDeviceMask) get_real_proc(get_last_instance(), device, "vkCmdSetDeviceMask");
    if (!real_fn) {
        real_fn = (PFN_vkCmdSetDeviceMask) get_real_proc(get_last_instance(), device, "vkCmdSetDeviceMaskKHR");
    }
    if (real_fn) {
        real_fn(commandBuffer, deviceMask);
    }
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

    uint32_t physCount = 0;
    real_epd(inst, &physCount, nullptr);
    if (!pPhysicalDeviceGroupProperties) {
        *pPhysicalDeviceGroupCount = physCount;
        outResult = VK_SUCCESS;
        return true;
    }

    uint32_t toFill = std::min(*pPhysicalDeviceGroupCount, physCount);
    std::vector<VkPhysicalDevice> phys(physCount);
    real_epd(inst, &physCount, phys.data());

    for (uint32_t i = 0; i < toFill; ++i) {
        auto& group = pPhysicalDeviceGroupProperties[i];
        group.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
        group.pNext = nullptr;
        group.physicalDeviceCount = 1;
        group.physicalDevices[0] = phys[i];
        for (uint32_t j = 1; j < VK_MAX_DEVICE_GROUP_SIZE; ++j) {
            group.physicalDevices[j] = VK_NULL_HANDLE;
        }
        group.subsetAllocation = VK_FALSE;
    }
    *pPhysicalDeviceGroupCount = toFill;
    outResult = (toFill < physCount) ? VK_INCOMPLETE : VK_SUCCESS;
    return true;
}

#include "maintenance.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"

REGISTER_LAYER_MODULE(MaintenanceModule);

MaintenanceModule::MaintenanceModule()
    : ExtensionModuleBase(VK_KHR_MAINTENANCE1_EXTENSION_NAME, VK_KHR_MAINTENANCE1_SPEC_VERSION, VK_API_VERSION_1_1) {
    LOGI("MaintenanceModule initialized");
}

void MaintenanceModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    ExtensionModuleBase::on_enumerate_device_extensions(physicalDevice, extensions);

    if (!is_phys_device_native(physicalDevice)) {
        if (!vku::has_extension(extensions, VK_KHR_MAINTENANCE2_EXTENSION_NAME)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, VK_KHR_MAINTENANCE2_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = VK_KHR_MAINTENANCE2_SPEC_VERSION;
            extensions.push_back(prop);
        }
        if (!vku::has_extension(extensions, VK_KHR_MAINTENANCE3_EXTENSION_NAME)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, VK_KHR_MAINTENANCE3_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = VK_KHR_MAINTENANCE3_SPEC_VERSION;
            extensions.push_back(prop);
        }
    }
}

void MaintenanceModule::on_pre_create_device_custom(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    for (auto it = enabledExtensions.begin(); it != enabledExtensions.end();) {
        if (strcmp(*it, VK_KHR_MAINTENANCE2_EXTENSION_NAME) == 0 ||
            strcmp(*it, VK_KHR_MAINTENANCE3_EXTENSION_NAME) == 0) {
            it = enabledExtensions.erase(it);
        } else {
            ++it;
        }
    }
}

void MaintenanceModule::on_pre_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pProperties) return;

    pUserData = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES);
}

void MaintenanceModule::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties || !pUserData) return;

    auto* p = reinterpret_cast<VkPhysicalDeviceMaintenance3Properties*>(pUserData);
    p->maxPerSetDescriptors = 1024;
    p->maxMemoryAllocationSize = 0x80000000ULL;
    vku::relink_pnext(pProperties->pNext, pUserData);
}

bool MaintenanceModule::on_get_descriptor_set_layout_support(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    VkDescriptorSetLayoutSupport* pSupport
) {
    if (is_device_native(device)) return false;

    PFN_vkGetDescriptorSetLayoutSupport real_fn =
        (PFN_vkGetDescriptorSetLayoutSupport) get_real_proc(get_last_instance(), device, "vkGetDescriptorSetLayoutSupport");
    if (!real_fn) {
        real_fn = (PFN_vkGetDescriptorSetLayoutSupport) get_real_proc(get_last_instance(), device, "vkGetDescriptorSetLayoutSupportKHR");
    }
    if (real_fn) {
        real_fn(device, pCreateInfo, pSupport);
        return true;
    }

    if (pSupport) {
        pSupport->supported = VK_TRUE;
        auto* varSupport = vku::find_pnext_mut<VkDescriptorSetVariableDescriptorCountLayoutSupport>(
            pSupport->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT);
        if (varSupport) {
            varSupport->maxVariableDescriptorCount = 1024;
        }
    }
    return true;
}

void MaintenanceModule::on_trim_command_pool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolTrimFlags flags
) {
    if (is_device_native(device)) return;

    PFN_vkTrimCommandPool real_fn =
        (PFN_vkTrimCommandPool) get_real_proc(get_last_instance(), device, "vkTrimCommandPool");
    if (!real_fn) {
        real_fn = (PFN_vkTrimCommandPool) get_real_proc(get_last_instance(), device, "vkTrimCommandPoolKHR");
    }
    if (real_fn) {
        real_fn(device, commandPool, flags);
    }
}

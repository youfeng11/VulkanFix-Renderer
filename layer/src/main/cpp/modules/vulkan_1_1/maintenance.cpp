#include "maintenance.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include "native_instance_state.h"

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

void MaintenanceModule::on_pre_create_device(VkPhysicalDevice physical, VkDeviceCreateInfo* info,
    VkPhysicalDeviceFeatures* features, std::vector<const char*>& extensions, void*& data) {
    data = nullptr;
    if (!is_phys_device_native(physical)) vku::strip_extension(extensions, VK_KHR_MAINTENANCE1_EXTENSION_NAME);
    // maintenance1 capability says nothing about independent maintenance2/3.
    on_pre_create_device_custom(physical, info, features, extensions, data);
}

void MaintenanceModule::on_pre_create_device_custom(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    bool requested = false;
    for (auto extension : enabledExtensions)
        requested |= !strcmp(extension, VK_KHR_MAINTENANCE2_EXTENSION_NAME) || !strcmp(extension, VK_KHR_MAINTENANCE3_EXTENSION_NAME);
    if (!requested) return;
    auto instance = vkfix::native_instance_for_physical(physicalDevice);
    if (!instance) instance = get_last_instance();
    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)get_real_proc(instance, VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> native;
    if (enumerate) {
        VkResult result = VK_INCOMPLETE;
        for (unsigned retry = 0; retry < 8 && result == VK_INCOMPLETE; ++retry) {
            uint32_t count = 0;
            result = enumerate(physicalDevice, nullptr, &count, nullptr);
            if (result != VK_SUCCESS) return;
            native.resize(count);
            result = count ? enumerate(physicalDevice, nullptr, &count, native.data()) : VK_SUCCESS;
            if (result == VK_SUCCESS) native.resize(count);
        }
        // Preserve requests so the central native enumeration reports errors.
        if (result != VK_SUCCESS) return;
    }
    for (auto it = enabledExtensions.begin(); it != enabledExtensions.end();) {
        if ((!strcmp(*it, VK_KHR_MAINTENANCE2_EXTENSION_NAME) ||
             !strcmp(*it, VK_KHR_MAINTENANCE3_EXTENSION_NAME)) && !vku::has_extension(native, *it)) {
            it = enabledExtensions.erase(it);
        } else {
            ++it;
        }
    }
}

// The promotion bridge queries real limits; no synthetic multiview or allocation limits.
void MaintenanceModule::on_pre_get_properties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*, void*& data) { data = nullptr; }
void MaintenanceModule::on_post_get_properties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*, void*) {}

bool MaintenanceModule::on_get_descriptor_set_layout_support(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    VkDescriptorSetLayoutSupport* pSupport
) {
    // The manager owns authorized Core/KHR selection and the creation fallback.
    return false;
}

bool MaintenanceModule::on_trim_command_pool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolTrimFlags flags
) {
    // The manager performs the enabled native operation or a permitted no-op.
    return false;
}

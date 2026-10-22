#include "buffer_device_address.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include <string.h>

REGISTER_LAYER_MODULE(BufferDeviceAddressModule);

BufferDeviceAddressModule::BufferDeviceAddressModule()
    : ExtensionModuleBase(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_SPEC_VERSION, VK_API_VERSION_1_2) {
    LOGI("BufferDeviceAddressModule initialized");
}

void BufferDeviceAddressModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    ExtensionModuleBase::on_enumerate_device_extensions(physicalDevice, extensions);

    if (!is_phys_device_native(physicalDevice)) {
        if (!vku::has_extension(extensions, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = VK_EXT_BUFFER_DEVICE_ADDRESS_SPEC_VERSION;
            extensions.push_back(prop);
        }
    }
}

bool BufferDeviceAddressModule::on_get_buffer_device_address(
    VkDevice device,
    const VkBufferDeviceAddressInfo* pInfo,
    VkDeviceAddress& outAddress
) {
    if (is_device_native(device)) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.GetBufferDeviceAddress) {
        outAddress = dt.GetBufferDeviceAddress(device, pInfo);
        return true;
    }

    PFN_vkGetBufferDeviceAddressKHR real_fn =
        (PFN_vkGetBufferDeviceAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferDeviceAddressKHR");
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferDeviceAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferDeviceAddress");
    }
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferDeviceAddressKHR) get_real_proc(get_last_instance(), device, "vkGetBufferDeviceAddressEXT");
    }
    if (real_fn) {
        outAddress = real_fn(device, pInfo);
        return true;
    }

    outAddress = pInfo ? (VkDeviceAddress)(uintptr_t)pInfo->buffer : 0;
    return true;
}

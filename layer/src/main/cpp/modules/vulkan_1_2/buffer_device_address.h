#ifndef BUFFER_DEVICE_ADDRESS_H
#define BUFFER_DEVICE_ADDRESS_H

#include "extension_module_base.h"

class BufferDeviceAddressModule : public ExtensionModuleBase {
public:
    BufferDeviceAddressModule();
    ~BufferDeviceAddressModule() override = default;

    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    bool on_get_buffer_device_address(
        VkDevice device,
        const VkBufferDeviceAddressInfo* pInfo,
        VkDeviceAddress& outAddress) override;
};

#endif // BUFFER_DEVICE_ADDRESS_H

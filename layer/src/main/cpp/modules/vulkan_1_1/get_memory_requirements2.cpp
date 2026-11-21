#include "get_memory_requirements2.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"

REGISTER_LAYER_MODULE(GetMemoryRequirements2Module);

GetMemoryRequirements2Module::GetMemoryRequirements2Module()
    : ExtensionModuleBase(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_SPEC_VERSION, VK_API_VERSION_1_1) {
    LOGI("GetMemoryRequirements2Module initialized");
}

void GetMemoryRequirements2Module::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    ExtensionModuleBase::on_enumerate_device_extensions(physicalDevice, extensions);

    if (!is_phys_device_native(physicalDevice)) {
        if (!vku::has_extension(extensions, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = VK_KHR_DEDICATED_ALLOCATION_SPEC_VERSION;
            extensions.push_back(prop);
        }
    }
}

// Native command authorization and legacy output initialization belong to the
// central dispatcher, independently of the module's physical capability cache.
bool GetMemoryRequirements2Module::on_get_buffer_memory_requirements2(
    VkDevice, const VkBufferMemoryRequirementsInfo2*, VkMemoryRequirements2*) { return false; }
bool GetMemoryRequirements2Module::on_get_image_memory_requirements2(
    VkDevice, const VkImageMemoryRequirementsInfo2*, VkMemoryRequirements2*) { return false; }
bool GetMemoryRequirements2Module::on_get_image_sparse_memory_requirements2(
    VkDevice, const VkImageSparseMemoryRequirementsInfo2*, uint32_t*, VkSparseImageMemoryRequirements2*) { return false; }

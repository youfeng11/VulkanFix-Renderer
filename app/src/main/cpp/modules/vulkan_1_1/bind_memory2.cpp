#include "bind_memory2.h"
#include "layer_manager.h"
#include "driver_loader.h"

REGISTER_LAYER_MODULE(BindMemory2Module);

BindMemory2Module::BindMemory2Module()
    : ExtensionModuleBase(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_SPEC_VERSION, VK_API_VERSION_1_1) {
    LOGI("BindMemory2Module initialized");
}

bool BindMemory2Module::on_bind_buffer_memory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindBufferMemoryInfo* pBindInfos,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    PFN_vkBindBufferMemory2 real_fn =
        (PFN_vkBindBufferMemory2) get_real_proc(get_last_instance(), device, "vkBindBufferMemory2");
    if (!real_fn) {
        real_fn = (PFN_vkBindBufferMemory2) get_real_proc(get_last_instance(), device, "vkBindBufferMemory2KHR");
    }
    if (real_fn) {
        outResult = real_fn(device, bindInfoCount, pBindInfos);
        return true;
    }

    if (bindInfoCount == 0 || !pBindInfos) {
        outResult = VK_SUCCESS;
        return true;
    }

    PFN_vkBindBufferMemory real_bind =
        (PFN_vkBindBufferMemory) get_real_proc(get_last_instance(), device, "vkBindBufferMemory");
    if (!real_bind) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    outResult = VK_SUCCESS;
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        VkResult r = real_bind(device, pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) {
            outResult = r;
            return true;
        }
    }
    return true;
}

bool BindMemory2Module::on_bind_image_memory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindImageMemoryInfo* pBindInfos,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    PFN_vkBindImageMemory2 real_fn =
        (PFN_vkBindImageMemory2) get_real_proc(get_last_instance(), device, "vkBindImageMemory2");
    if (!real_fn) {
        real_fn = (PFN_vkBindImageMemory2) get_real_proc(get_last_instance(), device, "vkBindImageMemory2KHR");
    }
    if (real_fn) {
        outResult = real_fn(device, bindInfoCount, pBindInfos);
        return true;
    }

    if (bindInfoCount == 0 || !pBindInfos) {
        outResult = VK_SUCCESS;
        return true;
    }

    PFN_vkBindImageMemory real_bind =
        (PFN_vkBindImageMemory) get_real_proc(get_last_instance(), device, "vkBindImageMemory");
    if (!real_bind) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    outResult = VK_SUCCESS;
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        VkResult r = real_bind(device, pBindInfos[i].image, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) {
            outResult = r;
            return true;
        }
    }
    return true;
}

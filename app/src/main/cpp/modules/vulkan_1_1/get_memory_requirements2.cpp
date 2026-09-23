#include "get_memory_requirements2.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include <vector>

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

bool GetMemoryRequirements2Module::on_get_buffer_memory_requirements2(
    VkDevice device,
    const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    if (is_device_native(device)) return false;

    PFN_vkGetBufferMemoryRequirements2 real_fn =
        (PFN_vkGetBufferMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements2");
    if (!real_fn) {
        real_fn = (PFN_vkGetBufferMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements2KHR");
    }
    if (real_fn) {
        real_fn(device, pInfo, pMemoryRequirements);
        return true;
    }

    if (!pInfo || !pMemoryRequirements) return true;

    PFN_vkGetBufferMemoryRequirements real_gmr =
        (PFN_vkGetBufferMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements");
    if (real_gmr) {
        real_gmr(device, pInfo->buffer, &pMemoryRequirements->memoryRequirements);
    }

    auto* dedicated = vku::find_pnext_mut<VkMemoryDedicatedRequirements>(
        pMemoryRequirements->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
    if (dedicated) {
        dedicated->prefersDedicatedAllocation = VK_FALSE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
    }
    return true;
}

bool GetMemoryRequirements2Module::on_get_image_memory_requirements2(
    VkDevice device,
    const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements
) {
    if (is_device_native(device)) return false;

    PFN_vkGetImageMemoryRequirements2 real_fn =
        (PFN_vkGetImageMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements2");
    if (!real_fn) {
        real_fn = (PFN_vkGetImageMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements2KHR");
    }
    if (real_fn) {
        real_fn(device, pInfo, pMemoryRequirements);
        return true;
    }

    if (!pInfo || !pMemoryRequirements) return true;

    PFN_vkGetImageMemoryRequirements real_gmr =
        (PFN_vkGetImageMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements");
    if (real_gmr) {
        real_gmr(device, pInfo->image, &pMemoryRequirements->memoryRequirements);
    }

    auto* dedicated = vku::find_pnext_mut<VkMemoryDedicatedRequirements>(
        pMemoryRequirements->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
    if (dedicated) {
        dedicated->prefersDedicatedAllocation = VK_FALSE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
    }
    return true;
}

bool GetMemoryRequirements2Module::on_get_image_sparse_memory_requirements2(
    VkDevice device,
    const VkImageSparseMemoryRequirementsInfo2* pInfo,
    uint32_t* pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2* pSparseMemoryRequirements
) {
    if (is_device_native(device)) return false;

    PFN_vkGetImageSparseMemoryRequirements2 real_fn =
        (PFN_vkGetImageSparseMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageSparseMemoryRequirements2");
    if (!real_fn) {
        real_fn = (PFN_vkGetImageSparseMemoryRequirements2) get_real_proc(get_last_instance(), device, "vkGetImageSparseMemoryRequirements2KHR");
    }
    if (real_fn) {
        real_fn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
        return true;
    }

    if (!pInfo || !pSparseMemoryRequirementCount) return true;

    PFN_vkGetImageSparseMemoryRequirements real_smr =
        (PFN_vkGetImageSparseMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetImageSparseMemoryRequirements");
    if (real_smr) {
        if (!pSparseMemoryRequirements) {
            real_smr(device, pInfo->image, pSparseMemoryRequirementCount, nullptr);
        } else {
            uint32_t count = *pSparseMemoryRequirementCount;
            std::vector<VkSparseImageMemoryRequirements> nativeReqs(count);
            real_smr(device, pInfo->image, &count, nativeReqs.data());
            for (uint32_t i = 0; i < count; ++i) {
                pSparseMemoryRequirements[i].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2;
                pSparseMemoryRequirements[i].pNext = nullptr;
                pSparseMemoryRequirements[i].memoryRequirements = nativeReqs[i];
            }
            *pSparseMemoryRequirementCount = count;
        }
        return true;
    }

    *pSparseMemoryRequirementCount = 0;
    return true;
}

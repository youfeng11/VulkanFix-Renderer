#ifndef GET_MEMORY_REQUIREMENTS_2_H
#define GET_MEMORY_REQUIREMENTS_2_H

#include "extension_module_base.h"

class GetMemoryRequirements2Module : public ExtensionModuleBase {
public:
    GetMemoryRequirements2Module();
    ~GetMemoryRequirements2Module() override = default;

    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    bool on_get_buffer_memory_requirements2(
        VkDevice device,
        const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) override;

    bool on_get_image_memory_requirements2(
        VkDevice device,
        const VkImageMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) override;

    bool on_get_image_sparse_memory_requirements2(
        VkDevice device,
        const VkImageSparseMemoryRequirementsInfo2* pInfo,
        uint32_t* pSparseMemoryRequirementCount,
        VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) override;
};

#endif // GET_MEMORY_REQUIREMENTS_2_H

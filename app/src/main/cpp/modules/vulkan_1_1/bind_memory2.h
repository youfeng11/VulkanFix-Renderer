#ifndef BIND_MEMORY_2_H
#define BIND_MEMORY_2_H

#include "extension_module_base.h"

class BindMemory2Module : public ExtensionModuleBase {
public:
    BindMemory2Module();
    ~BindMemory2Module() override = default;

    bool on_bind_buffer_memory2(
        VkDevice device,
        uint32_t bindInfoCount,
        const VkBindBufferMemoryInfo* pBindInfos,
        VkResult& outResult) override;

    bool on_bind_image_memory2(
        VkDevice device,
        uint32_t bindInfoCount,
        const VkBindImageMemoryInfo* pBindInfos,
        VkResult& outResult) override;
};

#endif // BIND_MEMORY_2_H

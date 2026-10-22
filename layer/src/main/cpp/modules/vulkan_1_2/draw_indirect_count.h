#ifndef DRAW_INDIRECT_COUNT_H
#define DRAW_INDIRECT_COUNT_H

#include "extension_module_base.h"

class DrawIndirectCountModule : public ExtensionModuleBase {
public:
    DrawIndirectCountModule();
    ~DrawIndirectCountModule() override = default;

    bool on_cmd_draw_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride) override;

    bool on_cmd_draw_indexed_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride) override;

    // Only advertise the extension when the driver implements it. The old
    // fallback ignored countBuffer and issued maxDrawCount draws, which is a
    // semantic change (and a huge over-draw), so it must not be exposed.
    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;
};

#endif // DRAW_INDIRECT_COUNT_H

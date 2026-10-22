#include "draw_indirect_count.h"
#include "layer_manager.h"
#include "driver_loader.h"

REGISTER_LAYER_MODULE(DrawIndirectCountModule);

void DrawIndirectCountModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    // Deliberately do NOT inject VK_KHR_draw_indirect_count on drivers that lack it.
    // The previous fallback ignored countBuffer and issued maxDrawCount draws, which
    // changes rendering semantics and massively over-draws. Applications must be able
    // to take their own fallback path instead.
    if (is_phys_device_native(physicalDevice)) return;
    LOGI("DrawIndirectCount: driver lacks native VK_KHR_draw_indirect_count, extension not advertised");
}

DrawIndirectCountModule::DrawIndirectCountModule()
    : ExtensionModuleBase(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME, VK_KHR_DRAW_INDIRECT_COUNT_SPEC_VERSION, VK_API_VERSION_1_2) {
    LOGI("DrawIndirectCountModule initialized");
}

bool DrawIndirectCountModule::on_cmd_draw_indirect_count(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.CmdDrawIndirectCount) {
        dt.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
        return true;
    }

    if (maxDrawCount == 0) return true;
    if (stride == 0) stride = sizeof(VkDrawIndirectCommand);

    // No native vkCmdDrawIndirectCount and no correct emulation available: never
    // fake it by replaying maxDrawCount draws (the GPU-written count would be
    // ignored). Fall through so the caller/application handles the absence.
    LOGW("vkCmdDrawIndirectCount: no native support; call dropped instead of over-drawing");
    (void)stride;
    (void)countBuffer;
    (void)countBufferOffset;
    return false;
}

bool DrawIndirectCountModule::on_cmd_draw_indexed_indirect_count(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.CmdDrawIndexedIndirectCount) {
        dt.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
        return true;
    }

    if (maxDrawCount == 0) return true;
    if (stride == 0) stride = sizeof(VkDrawIndexedIndirectCommand);

    LOGW("vkCmdDrawIndexedIndirectCount: no native support; call dropped instead of over-drawing");
    (void)stride;
    (void)countBuffer;
    (void)countBufferOffset;
    return false;
}

#include "draw_indirect_count.h"
#include "layer_manager.h"
#include "driver_loader.h"

REGISTER_LAYER_MODULE(DrawIndirectCountModule);

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

    LayerManager::get().dispatch_cmd_draw_indirect(commandBuffer, buffer, offset, maxDrawCount, stride);
    return true;
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

    LayerManager::get().dispatch_cmd_draw_indexed_indirect(commandBuffer, buffer, offset, maxDrawCount, stride);
    return true;
}

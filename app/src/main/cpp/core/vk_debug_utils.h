#ifndef VK_DEBUG_UTILS_H
#define VK_DEBUG_UTILS_H

#include "layer_manager.h"
#include "renderdoc_manager.h"

namespace vku {

inline void cmd_begin_label(VkCommandBuffer cmd, const char* name, const float color[4] = nullptr) {
    if (!cmd || !name || !RenderDocManager::get().is_available()) return;
    VkDevice dev = LayerManager::get().get_device_for_cmd(cmd);
    const auto& dt = LayerManager::get().get_dispatch_table(dev);
    if (dt.CmdBeginDebugUtilsLabelEXT) {
        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = name;
        if (color) {
            label.color[0] = color[0];
            label.color[1] = color[1];
            label.color[2] = color[2];
            label.color[3] = color[3];
        } else {
            label.color[0] = 0.2f;
            label.color[1] = 0.7f;
            label.color[2] = 0.95f;
            label.color[3] = 1.0f;
        }
        dt.CmdBeginDebugUtilsLabelEXT(cmd, &label);
    }
}

inline void cmd_end_label(VkCommandBuffer cmd) {
    if (!cmd || !RenderDocManager::get().is_available()) return;
    VkDevice dev = LayerManager::get().get_device_for_cmd(cmd);
    const auto& dt = LayerManager::get().get_dispatch_table(dev);
    if (dt.CmdEndDebugUtilsLabelEXT) {
        dt.CmdEndDebugUtilsLabelEXT(cmd);
    }
}

inline void cmd_insert_label(VkCommandBuffer cmd, const char* name, const float color[4] = nullptr) {
    if (!cmd || !name || !RenderDocManager::get().is_available()) return;
    VkDevice dev = LayerManager::get().get_device_for_cmd(cmd);
    const auto& dt = LayerManager::get().get_dispatch_table(dev);
    if (dt.CmdInsertDebugUtilsLabelEXT) {
        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = name;
        if (color) {
            label.color[0] = color[0];
            label.color[1] = color[1];
            label.color[2] = color[2];
            label.color[3] = color[3];
        } else {
            label.color[0] = 0.95f;
            label.color[1] = 0.65f;
            label.color[2] = 0.2f;
            label.color[3] = 1.0f;
        }
        dt.CmdInsertDebugUtilsLabelEXT(cmd, &label);
    }
}

inline void set_object_name(VkDevice device, VkObjectType type, uint64_t objectHandle, const char* name) {
    if (device == VK_NULL_HANDLE || !objectHandle || !name) return;
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.SetDebugUtilsObjectNameEXT) {
        VkDebugUtilsObjectNameInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        info.objectType = type;
        info.objectHandle = objectHandle;
        info.pObjectName = name;
        dt.SetDebugUtilsObjectNameEXT(device, &info);
    }
}

inline void set_object_tag(VkDevice device, VkObjectType type, uint64_t objectHandle, uint64_t tagName, size_t tagSize, const void* tagData) {
    if (device == VK_NULL_HANDLE || !objectHandle || !tagData || tagSize == 0) return;
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.SetDebugUtilsObjectTagEXT) {
        VkDebugUtilsObjectTagInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT;
        info.objectType = type;
        info.objectHandle = objectHandle;
        info.tagName = tagName;
        info.tagSize = tagSize;
        info.pTag = tagData;
        dt.SetDebugUtilsObjectTagEXT(device, &info);
    }
}

} // namespace vku

#endif // VK_DEBUG_UTILS_H

#pragma once
#include <cstdint>

namespace vkfix {
// Backend routes enabled on a particular logical device, not the extensions
// advertised by the compatibility layer. These also form part of shader keys.
enum NativeSpirvExtension : uint32_t {
    NativeStorageBufferClass = 1u << 0,
    NativeMultiview = 1u << 1,
    NativeDeviceGroup = 1u << 2,
    NativeDrawParameters = 1u << 3,
    Native16BitStorage = 1u << 4,
    NativeVariablePointers = 1u << 5,
    NativeVulkanMemoryModel = 1u << 6,
    NativeRelaxedBlockLayout = 1u << 7,
    NativeInputAttachmentAspects = 1u << 8,
    NativeMaintenance1 = 1u << 9,
};
}

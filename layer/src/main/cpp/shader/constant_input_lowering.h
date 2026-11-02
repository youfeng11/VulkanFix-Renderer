#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkfix {
enum class ConstantInputLowering { Unchanged, Changed, Invalid };
using DeviceIndexLowering = ConstantInputLowering;
bool has_device_index(const uint32_t* code, size_t words);
// A logical device with one physical device has DeviceIndex == 0 in all
// stages. Removes the unsupported input, including interface-block members,
// while preserving the other inputs and the original aggregate value types.
DeviceIndexLowering lower_singleton_device_index(std::vector<uint32_t>& code,
                                                std::string& error);
bool has_view_index(const uint32_t* code, size_t words);
// Used by software multiview pipeline variants, after choosing a view. Keeps
// specialization-sized array values valid without freezing their lengths.
ConstantInputLowering lower_view_index(std::vector<uint32_t>& code, uint32_t view,
                                      std::string& error);
// Pipeline VIEW_INDEX_FROM_DEVICE_INDEX on a native device group: coalesce
// ViewIndex and existing DeviceIndex inputs into one actual native input.
ConstantInputLowering lower_view_index_from_device(std::vector<uint32_t>& code,
                                                  std::string& error);
}

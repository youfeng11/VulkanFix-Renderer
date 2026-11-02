#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace vkfix {
// Apply the stage's scalar specialization values (or defaults) and fold
// derived constants. Transactional; unknown constantIDs do not read pData.
bool specialize_constants(std::vector<uint32_t>&, const VkSpecializationInfo*, std::string& error);
}

#pragma once
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace vkfix {
// Replay supplies required multiview when no native route is available.
// Keep an explicit diagnostic opt-out for comparing the underlying driver.
inline bool software_multiview_enabled() {
    const auto* value = std::getenv("VULKAN_FIX_SOFTWARE_MULTIVIEW");
    return !value || std::strcmp(value, "0") != 0;
}
inline constexpr uint32_t software_multiview_view_count = 32;
inline constexpr uint32_t software_multiview_instance_index = (1u << 27) - 1;
}

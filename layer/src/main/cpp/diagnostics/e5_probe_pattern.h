#pragma once
#include <cstdint>

namespace vkfix::nativeprobe {
constexpr uint32_t pack_e5(uint32_t r, uint32_t g, uint32_t b, uint32_t exponent) {
    return r | (g << 9) | (b << 18) | (exponent << 27);
}
// R is large enough that every fourth word is already canonical. The other
// words have exactly one equivalent encoding with a smaller exponent.
constexpr uint32_t e5_probe_word(uint32_t index) {
    const auto red = (index % 4 ? 128u : 256u) + (index * 31u % 128u);
    return pack_e5(red, 1u + (index * 17u % 247u), 1u + (index * 73u % 241u), 9u + (index % 19u));
}
constexpr uint32_t canonical_e5_probe_word(uint32_t word) {
    const auto red = word & 511u;
    if (red >= 256u) return word;
    return pack_e5(red * 2u, ((word >> 9) & 511u) * 2u, ((word >> 18) & 511u) * 2u, (word >> 27) - 1u);
}
} // namespace vkfix::nativeprobe

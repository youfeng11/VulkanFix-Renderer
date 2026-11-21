#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <spirv/unified1/spirv.hpp>

namespace vkfix {
// Domain origin changes the interpretation of CW/CCW domain winding, not the
// shader-visible TessCoord values. Reverse only the selected TES entry point.
inline bool lower_tessellation_domain_origin(std::vector<uint32_t>& code, const char* entry) {
    if (!entry || code.size() < 5 || code[0] != spv::MagicNumber) return false;
    uint32_t selected = 0;
    for (size_t at = 5; at < code.size();) {
        const uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        if (!count || count > code.size() - at) return false;
        if (op == spv::OpEntryPoint && count >= 4 && code[at + 1] == spv::ExecutionModelTessellationEvaluation) {
            const auto* name = reinterpret_cast<const char*>(code.data() + at + 3);
            if (!std::memchr(name, 0, (count - 3) * 4)) return false;
            if (!std::strcmp(name, entry)) selected = code[at + 2];
        }
        at += count;
    }
    if (!selected) return false;
    for (size_t at = 5; at < code.size(); at += code[at] >> 16)
        if ((code[at] & 0xffff) == spv::OpExecutionMode && (code[at] >> 16) >= 3 && code[at + 1] == selected) {
            auto& mode = code[at + 2];
            if (mode == spv::ExecutionModeVertexOrderCw) mode = spv::ExecutionModeVertexOrderCcw;
            else if (mode == spv::ExecutionModeVertexOrderCcw) mode = spv::ExecutionModeVertexOrderCw;
        }
    return true;
}
}

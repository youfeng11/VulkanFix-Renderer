#include "shader/sampler_alias_dedup.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>

namespace vkfix {
namespace {
using Aliases = std::unordered_map<uint32_t, uint32_t>;
struct Decoration {
    uint32_t set = 0, binding = 0;
    bool has_set = false, has_binding = false, other = false;
};
struct Variable { uint32_t id, type; };
struct Instruction { size_t offset, count; };
enum class Scan { None, Aliased, Invalid };

// Metadata grows with actual instructions, never with the untrusted ID bound.
Scan scan(const uint32_t* w, size_t n, Aliases& aliases) {
    if (!w || n < 5 || w[0] != spv::MagicNumber) return Scan::Invalid;
    std::unordered_map<uint32_t, Instruction> types;
    std::unordered_map<uint32_t, Decoration> decorations;
    std::vector<Variable> variables;
    bool in_function = false, groups = false;
    for (size_t i = 5; i < n;) {
        const size_t c = w[i] >> 16;
        const auto op = static_cast<spv::Op>(w[i] & 0xffff);
        if (!c || c > n - i) return Scan::Invalid;
        if (op == spv::OpDecorationGroup || op == spv::OpGroupDecorate ||
            op == spv::OpGroupMemberDecorate) groups = true;
        if (op == spv::OpFunction) in_function = true;
        if (op == spv::OpFunctionEnd) in_function = false;
        if ((op == spv::OpTypeSampler && c == 2) ||
            (op == spv::OpTypeInt && c == 4) ||
            (op == spv::OpTypeArray && c == 4) ||
            (op == spv::OpTypePointer && c == 4))
            types.emplace(w[i + 1], Instruction{i, c});
        if (op == spv::OpConstant && c == 4)
            types.emplace(w[i + 2], Instruction{i, c});
        if (op == spv::OpVariable && c == 4 && !in_function &&
            w[i + 3] == spv::StorageClassUniformConstant)
            variables.push_back({w[i + 2], w[i + 1]});
        if (op == spv::OpDecorate && c >= 3) {
            auto& d = decorations[w[i + 1]];
            if (c == 4 && w[i + 2] == spv::DecorationDescriptorSet && !d.has_set) {
                d.set = w[i + 3]; d.has_set = true;
            } else if (c == 4 && w[i + 2] == spv::DecorationBinding && !d.has_binding) {
                d.binding = w[i + 3]; d.has_binding = true;
            } else d.other = true;
        }
        if ((op == spv::OpDecorateId || op == spv::OpDecorateStringGOOGLE ||
             op == spv::OpMemberDecorate || op == spv::OpMemberDecorateStringGOOGLE) && c >= 2)
            decorations[w[i + 1]].other = true;
        i += c;
    }
    if (groups || variables.size() < 2) return Scan::None;
    auto instruction = [&](uint32_t id) -> const uint32_t* {
        auto it = types.find(id);
        return it == types.end() ? nullptr : w + it->second.offset;
    };
    std::vector<Variable> canonical;
    for (const auto& v : variables) {
        auto dit = decorations.find(v.id);
        if (dit == decorations.end()) continue;
        const auto& d = dit->second;
        if (!d.has_set || !d.has_binding || d.other) continue;
        const auto* pointer = instruction(v.type);
        if (!pointer || (pointer[0] & 0xffff) != spv::OpTypePointer ||
            pointer[2] != spv::StorageClassUniformConstant) continue;
        const auto* value = instruction(pointer[3]);
        if (!value) continue;
        if ((value[0] & 0xffff) == spv::OpTypeArray) {
            const auto* length = instruction(value[3]);
            if (!length || (length[0] & 0xffff) != spv::OpConstant) continue;
            const auto* integer = instruction(length[1]);
            if (!integer || (integer[0] & 0xffff) != spv::OpTypeInt ||
                integer[2] != 32 || !length[3] ||
                (integer[3] && (length[3] & 0x80000000u))) continue;
            value = instruction(value[2]);
        }
        if (!value || (value[0] & 0xffff) != spv::OpTypeSampler) continue;
        auto first = std::find_if(canonical.begin(), canonical.end(), [&](const Variable& a) {
            const auto& ad = decorations.at(a.id);
            return a.type == v.type && ad.set == d.set && ad.binding == d.binding;
        });
        if (first == canonical.end()) canonical.push_back(v);
        else aliases.emplace(v.id, first->id);
    }
    return aliases.empty() ? Scan::None : Scan::Aliased;
}

spv_target_env environment(uint32_t version) {
    if (version >= 0x10600) return SPV_ENV_VULKAN_1_3;
    if (version >= 0x10500) return SPV_ENV_VULKAN_1_2;
    if (version >= 0x10400) return SPV_ENV_VULKAN_1_1_SPIRV_1_4;
    if (version >= 0x10300) return SPV_ENV_VULKAN_1_1;
    return SPV_ENV_VULKAN_1_0;
}
bool reference(spv_operand_type_t type) {
    return type == SPV_OPERAND_TYPE_ID || type == SPV_OPERAND_TYPE_TYPE_ID ||
           type == SPV_OPERAND_TYPE_SCOPE_ID || type == SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID;
}
struct Rewrite {
    const Aliases& aliases;
    std::vector<uint32_t>& output;
};
spv_result_t rewrite_instruction(void* opaque, const spv_parsed_instruction_t* inst) {
    auto& state = *static_cast<Rewrite*>(opaque);
    const auto& aliases = state.aliases;
    if ((inst->opcode == spv::OpVariable && aliases.count(inst->result_id)) ||
        ((inst->opcode == spv::OpName || inst->opcode == spv::OpDecorate) &&
         inst->num_words > 1 && aliases.count(inst->words[1]))) return SPV_SUCCESS;
    const size_t start = state.output.size();
    state.output.insert(state.output.end(), inst->words, inst->words + inst->num_words);
    size_t interfaces = inst->num_words;
    for (uint16_t j = 0; j < inst->num_operands; ++j) {
        const auto& operand = inst->operands[j];
        if (inst->opcode == spv::OpEntryPoint && operand.type == SPV_OPERAND_TYPE_LITERAL_STRING)
            interfaces = operand.offset + operand.num_words;
        if (!reference(operand.type)) continue;
        for (uint16_t k = 0; k < operand.num_words; ++k) {
            auto& id = state.output[start + operand.offset + k];
            auto it = aliases.find(id);
            if (it != aliases.end()) id = it->second;
        }
    }
    if (inst->opcode == spv::OpEntryPoint) {
        std::unordered_set<uint32_t> seen;
        size_t next = start + interfaces;
        for (size_t j = interfaces; j < inst->num_words; ++j) {
            const auto id = state.output[start + j];
            if (seen.insert(id).second) state.output[next++] = id;
        }
        state.output.resize(next);
        state.output[start] = static_cast<uint32_t>((next - start) << 16) | inst->opcode;
    }
    return SPV_SUCCESS;
}
} // namespace

bool has_sampler_aliases(const uint32_t* words, size_t word_count) {
    Aliases aliases;
    return scan(words, word_count, aliases) == Scan::Aliased;
}
SamplerAliasResult dedup_sampler_aliases(std::vector<uint32_t>& code,
                                        uint32_t max_spirv_version, std::string& error) {
    error.clear();
    Aliases aliases;
    const auto candidate = scan(code.data(), code.size(), aliases);
    if (candidate == Scan::None) return SamplerAliasResult::Unchanged;
    if (candidate == Scan::Invalid) {
        error = "Malformed SPIR-V module";
        return SamplerAliasResult::Invalid;
    }
    const auto env = environment(max_spirv_version);
    spvtools::SpirvTools validator(env);
    validator.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* text) {
        if (error.empty() && text) error.assign(text, std::min<size_t>(std::strlen(text), 4096));
    });
    if (!validator.Validate(code)) return SamplerAliasResult::Invalid;
    std::vector<uint32_t> rewritten(code.begin(), code.begin() + 5);
    rewritten.reserve(code.size());
    Rewrite state{aliases, rewritten};
    const auto context = spvContextCreate(env);
    if (!context) {
        error = "Cannot create SPIR-V parser";
        return SamplerAliasResult::Invalid;
    }
    spv_diagnostic diagnostic = nullptr;
    const auto parsed = spvBinaryParse(context, &state, code.data(), code.size(),
                                      nullptr, rewrite_instruction, &diagnostic);
    if (parsed != SPV_SUCCESS && error.empty())
        error = diagnostic && diagnostic->error ? diagnostic->error : "Sampler alias parsing failed";
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    if (parsed != SPV_SUCCESS || !validator.Validate(rewritten)) return SamplerAliasResult::Invalid;
    code.swap(rewritten);
    error.clear();
    return SamplerAliasResult::Changed;
}
} // namespace vkfix

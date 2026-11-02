#include "shader_rewriter.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>

namespace {
using Words = std::vector<uint32_t>;
struct Instruction {
    Words words;
    std::vector<uint16_t> uses;
    uint32_t type, result;
    uint32_t op() const { return words[0] & 0xffffu; }
};
spv_result_t parse(void* data, const spv_parsed_instruction_t* instruction) {
    Instruction out{{instruction->words, instruction->words + instruction->num_words}, {},
                    instruction->type_id, instruction->result_id};
    for (uint16_t i = 0; i < instruction->num_operands; ++i) {
        const auto& operand = instruction->operands[i];
        if (operand.type == SPV_OPERAND_TYPE_ID || operand.type == SPV_OPERAND_TYPE_TYPE_ID ||
            operand.type == SPV_OPERAND_TYPE_SCOPE_ID || operand.type == SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID)
            out.uses.push_back(operand.offset);
    }
    static_cast<std::vector<Instruction>*>(data)->push_back(std::move(out));
    return SPV_SUCCESS;
}
void emit(Words& words, spv::Op op, std::initializer_list<uint32_t> operands) {
    words.push_back((uint32_t(operands.size() + 1) << 16) | uint32_t(op));
    words.insert(words.end(), operands);
}
} // namespace

bool ShaderRewriter::offset_invocation_ids(Words& code, uint32_t base_x, uint32_t base_y,
                                          uint32_t base_z, const char* entry_point, bool* no_offset) {
    if (no_offset) *no_offset = false;
    if (!(base_x | base_y | base_z) || code.size() < 5 || code[0] != 0x07230203u) return false;
    // Normalize helper reads and discard unreachable helpers before deciding
    // whether this entry point observes invocation IDs. Vulkan shaders cannot
    // recurse, so exhaustive inlining preserves their execution semantics.
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
    optimizer.RegisterPass(spvtools::CreateInlineExhaustivePass());
    optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    spvtools::OptimizerOptions options;
    options.set_run_validator(false);
    Words input;
    if (!optimizer.Run(code.data(), code.size(), &input, options)) return false;
    std::vector<Instruction> instructions;
    auto context = spvContextCreate(SPV_ENV_VULKAN_1_1);
    const auto parsed = spvBinaryParse(context, &instructions, input.data(), input.size(), nullptr, parse, nullptr);
    spvContextDestroy(context);
    if (parsed != SPV_SUCCESS) return false;
    std::map<uint32_t, const Instruction*> definitions;
    std::map<uint32_t, uint32_t> builtins;
    uint32_t entry = 0, workgroup_size = 0;
    for (const auto& instruction : instructions) {
        const auto& w = instruction.words;
        if (instruction.result) definitions[instruction.result] = &instruction;
        if (instruction.op() == spv::OpDecorate && w.size() == 4 && w[2] == spv::DecorationBuiltIn) {
            builtins[w[1]] = w[3];
            if (w[3] == spv::BuiltInWorkgroupSize) workgroup_size = w[1];
        }
        if (instruction.op() == spv::OpEntryPoint && w.size() >= 4 &&
            w[1] == spv::ExecutionModelGLCompute &&
            !std::strcmp(reinterpret_cast<const char*>(&w[3]), entry_point ? entry_point : "main"))
            entry = w[2];
    }
    if (!entry) return false;
    std::array<uint32_t, 3> local_size{};
    for (const auto& instruction : instructions) {
        const auto& w = instruction.words;
        if (instruction.op() == spv::OpExecutionMode && w.size() == 6 &&
            w[1] == entry && w[2] == spv::ExecutionModeLocalSize)
            std::copy(w.begin() + 3, w.end(), local_size.begin());
    }
    struct Pointer { uint32_t variable, vector_type, index = 0; bool component = false; };
    std::map<uint32_t, Pointer> pointers;
    for (const auto& builtin : builtins) {
        if (builtin.second != spv::BuiltInGlobalInvocationId && builtin.second != spv::BuiltInWorkgroupId) continue;
        auto variable = definitions.find(builtin.first);
        if (variable == definitions.end() || variable->second->op() != spv::OpVariable) continue;
        auto pointer = definitions.find(variable->second->type);
        if (pointer == definitions.end() || pointer->second->op() != spv::OpTypePointer) return false;
        auto type = definitions.find(pointer->second->words[3]);
        if (type == definitions.end() || type->second->op() != spv::OpTypeVector || type->second->words[3] != 3) return false;
        pointers[builtin.first] = {builtin.first, type->first};
    }
    // CopyObject aliases and dynamic component indexes remain legal after
    // inlining. Preserve the index ID instead of assuming a literal x/y/z.
    bool changed;
    do {
        changed = false;
        for (const auto& instruction : instructions) {
            const auto& w = instruction.words;
            if (pointers.count(instruction.result)) continue;
            if (instruction.op() == spv::OpCopyObject && w.size() == 4 && pointers.count(w[3])) {
                pointers[instruction.result] = pointers.at(w[3]); changed = true;
            } else if ((instruction.op() == spv::OpAccessChain || instruction.op() == spv::OpInBoundsAccessChain) &&
                       w.size() == 5 && pointers.count(w[3]) && !pointers.at(w[3]).component) {
                auto pointer = pointers.at(w[3]); pointer.component = true; pointer.index = w[4];
                pointers[instruction.result] = pointer; changed = true;
            }
        }
    } while (changed);
    // CopyMemory can read a builtin without an explicit Load. In Core 1.1's
    // SPIR-V <= 1.3 it has one optional memory-access operand shared by both
    // accesses. Expand it to an equivalent load/store, then use the same pass.
    bool expanded = false;
    Words normalized(input.begin(), input.begin() + 5);
    uint32_t normalized_bound = input[3];
    for (const auto& instruction : instructions) {
        const auto& w = instruction.words;
        if (instruction.op() == spv::OpCopyMemory && w.size() >= 3 && pointers.count(w[2])) {
            if (input[1] > 0x00010300u) return false;
            const auto pointer = pointers.at(w[2]);
            const auto type = pointer.component ? definitions.at(pointer.vector_type)->words[2] : pointer.vector_type;
            const auto temporary = normalized_bound++;
            normalized.push_back((uint32_t(w.size() + 1) << 16) | spv::OpLoad);
            normalized.insert(normalized.end(), {type, temporary, w[2]});
            normalized.insert(normalized.end(), w.begin() + 3, w.end());
            normalized.push_back((uint32_t(w.size()) << 16) | spv::OpStore);
            normalized.insert(normalized.end(), {w[1], temporary});
            normalized.insert(normalized.end(), w.begin() + 3, w.end());
            expanded = true;
        } else normalized.insert(normalized.end(), w.begin(), w.end());
    }
    if (expanded) {
        normalized[3] = normalized_bound;
        if (!offset_invocation_ids(normalized, base_x, base_y, base_z, entry_point, no_offset)) return false;
        code = std::move(normalized);
        return true;
    }
    struct Patch { Pointer pointer; uint32_t sum; Words arithmetic; };
    std::map<uint32_t, Patch> patches;
    std::map<uint32_t, uint32_t> replacements;
    Words declarations;
    uint32_t next = input[3];
    const std::array<uint32_t, 3> base{base_x, base_y, base_z};
    auto constant_vector = [&](uint32_t type, const std::array<uint32_t, 3>& values) {
        const auto scalar = definitions.at(type)->words[2];
        uint32_t ids[3];
        for (unsigned i = 0; i < 3; ++i) {
            ids[i] = next++;
            emit(declarations, spv::OpConstant, {scalar, ids[i], values[i]});
        }
        const auto id = next++;
        emit(declarations, spv::OpConstantComposite, {type, id, ids[0], ids[1], ids[2]});
        return id;
    };
    for (const auto& instruction : instructions) {
        const auto& w = instruction.words;
        if (instruction.op() == spv::OpLoad && w.size() >= 4 && pointers.count(w[3])) {
            const auto pointer = pointers.at(w[3]);
            Patch patch{pointer, next++, {}};
            const auto type = pointer.vector_type;
            uint32_t offset = constant_vector(type, base);
            if (builtins.at(pointer.variable) == spv::BuiltInGlobalInvocationId) {
                // GlobalInvocationId = WorkgroupId * WorkgroupSize + LocalInvocationId.
                // Keep a specialized WorkgroupSize as an ID so pipeline specialization
                // controls this multiplication just as it controls native execution.
                uint32_t size = workgroup_size;
                if (!size) {
                    if (!local_size[0] || !local_size[1] || !local_size[2]) return false;
                    size = constant_vector(type, local_size);
                } else if (definitions.at(size)->type != type) {
                    const auto cast = next++;
                    emit(patch.arithmetic, spv::OpBitcast, {type, cast, size});
                    size = cast;
                }
                const auto scaled = next++;
                emit(patch.arithmetic, spv::OpIMul, {type, scaled, offset, size});
                offset = scaled;
            }
            if (pointer.component) {
                const auto extracted = next++;
                emit(patch.arithmetic, spv::OpVectorExtractDynamic, {instruction.type, extracted, offset, pointer.index});
                offset = extracted;
            }
            emit(patch.arithmetic, spv::OpIAdd, {instruction.type, patch.sum, instruction.result, offset});
            replacements[instruction.result] = patch.sum;
            patches.emplace(instruction.result, std::move(patch));
        } else if (instruction.op() != spv::OpName && instruction.op() != spv::OpDecorate &&
                   instruction.op() != spv::OpEntryPoint && instruction.op() != spv::OpAccessChain &&
                   instruction.op() != spv::OpInBoundsAccessChain && instruction.op() != spv::OpCopyObject) {
            // Do not mistake an unsupported pointer consumer (e.g. CopyMemory)
            // for an entry point that does not observe invocation IDs.
            for (auto operand : instruction.uses)
                if (pointers.count(w[operand])) return false;
        }
    }
    if (patches.empty()) { if (no_offset) *no_offset = true; return false; }
    Words output(input.begin(), input.begin() + 5);
    bool emitted = false;
    for (const auto& instruction : instructions) {
        if (!emitted && instruction.op() == spv::OpFunction) {
            output.insert(output.end(), declarations.begin(), declarations.end()); emitted = true;
        }
        auto words = instruction.words;
        // Grammar-based ID operands include Phi, ExtInst and all control-flow
        // consumers, while never rewriting a literal that happens to equal an ID.
        for (auto operand : instruction.uses) {
            auto replacement = replacements.find(words[operand]);
            if (replacement != replacements.end()) words[operand] = replacement->second;
        }
        output.insert(output.end(), words.begin(), words.end());
        auto patch = patches.find(instruction.result);
        if (patch != patches.end())
            output.insert(output.end(), patch->second.arithmetic.begin(), patch->second.arithmetic.end());
    }
    output[3] = next;
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    if (!tools.Validate(output)) return false;
    code = std::move(output);
    return true;
}

#include "subgroup_broadcast_lowering.h"
#include <algorithm>
#include <map>
#include <set>
#include <spirv-tools/libspirv.h>
#include <spirv/unified1/spirv.hpp>
#include <stdexcept>
#include <utility>
namespace vkfix {
namespace {
using Words = std::vector<uint32_t>;
struct Instruction {
    Words words;
    uint32_t type, result;
    uint32_t op() const {
        return words[0] & 65535;
    }
};
spv_result_t parse(void *data, const spv_parsed_instruction_t *inst) {
    static_cast<std::vector<Instruction> *>(data)->push_back(
        {Words(inst->words, inst->words + inst->num_words), inst->type_id, inst->result_id});
    return SPV_SUCCESS;
}
void put(Words &out, uint32_t op, const Words &args) {
    out.push_back((uint32_t(args.size() + 1) << 16) | op);
    out.insert(out.end(), args.begin(), args.end());
}
struct Lowering {
    uint32_t next, boolean = 0;
    Words declarations;
    std::map<uint32_t, Instruction> definitions;
    std::set<uint32_t> constants;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> literal_ids;
    std::map<uint32_t, uint32_t> bool_vectors;
    uint32_t id() {
        if (next >= 0x3fffff)
            throw std::runtime_error("SPIR-V ID bound exhausted");
        return next++;
    }
    uint32_t literal(uint32_t type, uint32_t value) {
        auto key = std::make_pair(type, value);
        if (literal_ids.count(key))
            return literal_ids[key];
        const auto &words = definitions.at(type).words;
        if (words.size() != 4 || (words[0] & 65535) != spv::OpTypeInt || words[3] || words[2] > 64)
            throw std::runtime_error("invalid broadcast index type");
        auto result = id();
        Words args{type, result, value};
        if (words[2] > 32)
            args.push_back(0);
        put(declarations, spv::OpConstant, args);
        literal_ids[key] = result;
        return result;
    }
    uint32_t bool_type() {
        if (!boolean) {
            boolean = id();
            put(declarations, spv::OpTypeBool, {boolean});
        }
        return boolean;
    }
    uint32_t condition(Words &body, uint32_t result_type, uint32_t index, uint32_t value) {
        auto condition = id();
        put(body, spv::OpIEqual, {bool_type(), condition, index, value});
        const auto &type = definitions.at(result_type).words;
        if ((type[0] & 65535) == spv::OpTypeVector) {
            const auto count = type[3];
            if (!bool_vectors.count(count)) {
                auto vector = id();
                put(declarations, spv::OpTypeVector, {vector, bool_type(), count});
                bool_vectors[count] = vector;
            }
            auto vector = id();
            Words args{bool_vectors[count], vector};
            args.insert(args.end(), count, condition);
            put(body, spv::OpCompositeConstruct, args);
            condition = vector;
        }
        return condition;
    }
    void expand(Words &body, const Instruction &inst, uint32_t count) {
        auto &w = inst.words;
        const auto index_type = definitions.at(w[5]).type;
        const auto &index_words = definitions.at(index_type).words;
        if (index_words.size() != 4 || (index_words[0] & 65535) != spv::OpTypeInt)
            throw std::runtime_error("invalid broadcast index type");
        if (index_words[2] < 32)
            count = std::min(count, 1u << index_words[2]);
        auto accumulated = id();
        put(body, inst.op(), {w[1], accumulated, w[3], w[4], literal(index_type, 0)});
        if (count == 1) {
            put(body, spv::OpCopyObject, {w[1], w[2], accumulated});
            return;
        }
        for (uint32_t i = 1; i < count; ++i) {
            auto source = literal(index_type, i);
            auto value = id();
            put(body, inst.op(), {w[1], value, w[3], w[4], source});
            auto cond = condition(body, w[1], w[5], source);
            auto result = i + 1 == count ? w[2] : id();
            put(body, spv::OpSelect, {w[1], result, cond, value, accumulated});
            accumulated = result;
        }
    }
};
} // namespace
BroadcastLowering lower_subgroup_broadcasts(Words &code, uint32_t maximum, bool shuffle,
                                            std::string &error) {
    error.clear();
    if (code.size() < 5 || code[0] != 0x07230203 || !maximum || (maximum & (maximum - 1))) {
        error = "invalid SPIR-V header or subgroup size";
        return BroadcastLowering::Invalid;
    }
    std::vector<Instruction> instructions;
    auto context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
    spv_diagnostic diagnostic = nullptr;
    auto result = spvBinaryParse(context, &instructions, code.data(), code.size(), nullptr, parse,
                                 &diagnostic);
    if (result != SPV_SUCCESS)
        error = diagnostic && diagnostic->error ? diagnostic->error : "SPIR-V parse failed";
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    if (result != SPV_SUCCESS)
        return BroadcastLowering::Invalid;
    Lowering lowering{code[3]};
    bool have_shuffle_capability = false, changed = false, need_shuffle = false;
    for (auto &inst : instructions) {
        if (inst.result &&
            (inst.result >= code[3] || !lowering.definitions.emplace(inst.result, inst).second)) {
            error = "invalid or duplicate SPIR-V result ID";
            return BroadcastLowering::Invalid;
        }
        switch (inst.op()) {
        case spv::OpTypeBool:
            lowering.boolean = inst.result;
            break;
        case spv::OpConstant:
        case spv::OpConstantNull:
        case spv::OpSpecConstant:
        case spv::OpSpecConstantOp:
            lowering.constants.insert(inst.result);
            break;
        case spv::OpCapability:
            have_shuffle_capability |= inst.words[1] == spv::CapabilityGroupNonUniformShuffle;
            break;
        default:
            break;
        }
    }
    for (auto &inst : instructions)
        if (inst.op() == spv::OpTypeVector && inst.words[2] == lowering.boolean)
            lowering.bool_vectors[inst.words[3]] = inst.result;
    Words body;
    try {
        for (auto &inst : instructions) {
            if (inst.op() == spv::OpGroupNonUniformBroadcast ||
                inst.op() == spv::OpGroupNonUniformQuadBroadcast) {
                if (inst.words.size() != 6)
                    throw std::runtime_error("invalid broadcast instruction length");
                const auto &type = lowering.definitions.at(inst.type).words;
                if ((type[0] & 65535) == spv::OpTypeVector) {
                    // Some pre-1.2 compilers miscompile mixed vector subgroup
                    // and quad broadcasts with equal constant source indices.
                    // Scalarize both families before any dynamic expansion.
                    Words components{inst.type, inst.result};
                    for (uint32_t component = 0; component < type[3]; ++component) {
                        auto value = lowering.id();
                        auto result = lowering.id();
                        put(body, spv::OpCompositeExtract,
                            {type[2], value, inst.words[4], component});
                        Instruction scalar{{(6u << 16) | inst.op(), type[2], result, inst.words[3],
                                            value, inst.words[5]},
                                           type[2],
                                           result};
                        if (!lowering.constants.count(inst.words[5])) {
                            if (shuffle && inst.op() == spv::OpGroupNonUniformBroadcast) {
                                scalar.words[0] = (6u << 16) | spv::OpGroupNonUniformShuffle;
                                body.insert(body.end(), scalar.words.begin(), scalar.words.end());
                                need_shuffle = true;
                            } else
                                lowering.expand(
                                    body, scalar,
                                    inst.op() == spv::OpGroupNonUniformQuadBroadcast ? 4 : maximum);
                        } else
                            body.insert(body.end(), scalar.words.begin(), scalar.words.end());
                        components.push_back(result);
                    }
                    put(body, spv::OpCompositeConstruct, components);
                    changed = true;
                    continue;
                }
                if (!lowering.constants.count(inst.words[5])) {
                    if (shuffle && inst.op() == spv::OpGroupNonUniformBroadcast) {
                        auto words = inst.words;
                        words[0] = (6u << 16) | spv::OpGroupNonUniformShuffle;
                        body.insert(body.end(), words.begin(), words.end());
                        need_shuffle = true;
                    } else
                        lowering.expand(body, inst,
                                        inst.op() == spv::OpGroupNonUniformQuadBroadcast ? 4
                                                                                         : maximum);
                    changed = true;
                    continue;
                }
            }
            body.insert(body.end(), inst.words.begin(), inst.words.end());
        }
    } catch (const std::exception &exception) {
        error = exception.what();
        return BroadcastLowering::Invalid;
    }
    if (!changed)
        return BroadcastLowering::Unchanged;
    Words output(code.begin(), code.begin() + 5);
    if (need_shuffle && !have_shuffle_capability)
        put(output, spv::OpCapability, {spv::CapabilityGroupNonUniformShuffle});
    bool declarations_inserted = false;
    for (size_t offset = 0; offset < body.size();) {
        if (!declarations_inserted && (body[offset] & 65535) == spv::OpFunction) {
            output.insert(output.end(), lowering.declarations.begin(), lowering.declarations.end());
            declarations_inserted = true;
        }
        auto count = body[offset] >> 16;
        output.insert(output.end(), body.begin() + offset, body.begin() + offset + count);
        offset += count;
    }
    if (!declarations_inserted) {
        error = "broadcast module has no function";
        return BroadcastLowering::Invalid;
    }
    output[3] = lowering.next;
    code = std::move(output);
    return BroadcastLowering::Changed;
}
} // namespace vkfix

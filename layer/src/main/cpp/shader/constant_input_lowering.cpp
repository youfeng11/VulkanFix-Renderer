#include "constant_input_lowering.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>

namespace vkfix {
namespace {
using Words = std::vector<uint32_t>;
struct Instruction {
    Words w;
    std::vector<uint16_t> ids, masks;
    uint32_t type, result;
    uint32_t op() const { return w[0] & 0xffffu; }
};
spv_result_t parse(void* data, const spv_parsed_instruction_t* inst) {
    Instruction out{{inst->words, inst->words + inst->num_words}, {}, {}, inst->type_id, inst->result_id};
    for (uint16_t i = 0; i < inst->num_operands; ++i) {
        const auto& operand = inst->operands[i];
        if (operand.type == SPV_OPERAND_TYPE_ID) out.ids.push_back(operand.offset);
        if (operand.type == SPV_OPERAND_TYPE_MEMORY_ACCESS) out.masks.push_back(operand.offset);
    }
    static_cast<std::vector<Instruction>*>(data)->push_back(std::move(out));
    return SPV_SUCCESS;
}
void append(Words& out, const Words& w) { out.insert(out.end(), w.begin(), w.end()); }
void emit(Words& out, spv::Op op, std::initializer_list<uint32_t> operands) {
    out.push_back((uint32_t(operands.size() + 1) << 16) | uint32_t(op));
    out.insert(out.end(), operands);
}
bool interface_decoration(uint32_t decoration) {
    return decoration == spv::DecorationBlock || decoration == spv::DecorationBuiltIn ||
        decoration == spv::DecorationLocation || decoration == spv::DecorationComponent ||
        decoration == spv::DecorationIndex || decoration == spv::DecorationPatch ||
        decoration == spv::DecorationFlat || decoration == spv::DecorationNoPerspective ||
        decoration == spv::DecorationCentroid || decoration == spv::DecorationSample ||
        decoration == spv::DecorationInvariant;
}
struct Lowering {
    uint32_t next, replacement = 0, uint_type = 0, bool_type = 0, device_input = 0;
    Words declarations, functions;
    std::map<uint32_t, Instruction> defs;
    std::set<std::pair<uint32_t, uint32_t>> removed_members;
    std::map<uint32_t, uint32_t> native_types, nulls;
    std::map<uint32_t, Words> cloned_types;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> pointers, literals;
    std::map<std::pair<uint32_t, Words>, uint32_t> helpers;
    uint32_t id() {
        if (next >= 0x3fffffu) throw std::runtime_error("constant input lowering exhausted ID space");
        return next++;
    }
    uint32_t literal(uint32_t type, uint32_t value) {
        auto& result = literals[{type, value}];
        if (!result) {
            const auto& integer = defs.at(type).w;
            if ((integer[0] & 0xffffu) != spv::OpTypeInt ||
                (integer[2] != 8 && integer[2] != 16 && integer[2] != 32 && integer[2] != 64))
                throw std::runtime_error("unsupported integer width in an input array");
            result = id();
            if (integer[2] == 64) emit(declarations, spv::OpConstant, {type, result, value, 0});
            else emit(declarations, spv::OpConstant, {type, result, value});
        }
        return result;
    }
    uint32_t index(uint32_t value) {
        if (!uint_type) {
            uint_type = id();
            emit(declarations, spv::OpTypeInt, {uint_type, 32, 0});
            defs[uint_type] = {{(4u << 16) | spv::OpTypeInt, uint_type, 32, 0}, {}, {}, 0, uint_type};
        }
        return literal(uint_type, value);
    }
    uint32_t boolean() {
        if (!bool_type) { bool_type = id(); emit(declarations, spv::OpTypeBool, {bool_type}); }
        return bool_type;
    }
    uint32_t zero(uint32_t type) {
        auto& result = nulls[type];
        if (!result) { result = id(); emit(declarations, spv::OpConstantNull, {type, result}); }
        return result;
    }
    uint32_t constant(Words& out, uint32_t type) {
        if (!replacement && !device_input) return zero(type);
        const auto source = defs.at(type).w;
        if ((source[0] & 0xffffu) == spv::OpTypeInt) {
            if (!device_input) return literal(type, replacement);
            if (source[2] != 32) throw std::runtime_error("DeviceIndex input is not 32-bit");
            const auto value = id(); emit(out, spv::OpLoad, {uint_type, value, device_input});
            if (type == uint_type) return value;
            const auto converted = id(); emit(out, spv::OpBitcast, {type, converted, value});
            return converted;
        }
        if ((source[0] & 0xffffu) == spv::OpTypeStruct) {
            Words construct{0, type, id()};
            for (size_t i = 2; i < source.size(); ++i) construct.push_back(constant(out, source[i]));
            construct[0] = (uint32_t(construct.size()) << 16) | spv::OpCompositeConstruct;
            append(out, construct); return construct[2];
        }
        if ((source[0] & 0xffffu) == spv::OpTypeArray) {
            // OpConstantComposite cannot represent an array whose length can
            // still change through specialization. Fill it at invocation time.
            const auto value = id();
            emit(out, spv::OpFunctionCall, {type, value, helper(type, {})});
            return value;
        }
        throw std::runtime_error("unsupported constant input value type");
    }
    uint32_t pointer(uint32_t storage, uint32_t type) {
        auto& result = pointers[{storage, type}];
        if (!result) { result = id(); emit(declarations, spv::OpTypePointer, {result, storage, type}); }
        return result;
    }
    bool removed(uint32_t structure, uint32_t member) {
        return removed_members.count({structure, member}) || !native_type(defs.at(structure).w.at(member + 2));
    }
    uint32_t member_index(uint32_t type, uint32_t member) {
        uint32_t index = 0;
        for (uint32_t m = 0; m < member; ++m) if (!removed(type, m)) ++index;
        return index;
    }
    uint32_t native_type(uint32_t type) {
        const auto found = native_types.find(type);
        if (found != native_types.end()) return found->second;
        auto w = defs.at(type).w;
        bool changed = false;
        if ((w[0] & 0xffffu) == spv::OpTypeStruct) {
            Words members;
            for (uint32_t m = 0; m + 2 < w.size(); ++m) {
                const auto replacement = removed_members.count({type, m}) ? 0 : native_type(w[m + 2]);
                if (replacement != w[m + 2]) changed = true;
                if (replacement) members.push_back(replacement);
            }
            if (changed && members.empty()) return native_types[type] = 0;
            if (changed) { w.resize(2); append(w, members); }
        } else if ((w[0] & 0xffffu) == spv::OpTypeArray) {
            const auto element = native_type(w[2]);
            if (!element) return native_types[type] = 0;
            changed = element != w[2]; w[2] = element;
        }
        if (!changed) return native_types[type] = type;
        w[1] = id(); w[0] = (uint32_t(w.size()) << 16) | (w[0] & 0xffffu);
        cloned_types[type] = w;
        return native_types[type] = w[1];
    }
    uint32_t read(Words& out, uint32_t type, uint32_t address, Words flags) {
        if (!native_type(type)) return constant(out, type);
        // Removing an interface member can change the native layout even when
        // the selected leaf keeps its original scalar/vector value type.
        if (!flags.empty() && (flags[0] & spv::MemoryAccessAlignedMask)) {
            flags[0] &= ~uint32_t(spv::MemoryAccessAlignedMask); flags.erase(flags.begin() + 1);
        }
        const uint32_t value = id();
        if (native_type(type) != type)
            emit(out, spv::OpFunctionCall, {type, value, helper(type, flags), address});
        else {
            Words w{0, type, value, address}; append(w, flags);
            w[0] = (uint32_t(w.size()) << 16) | spv::OpLoad; append(out, w);
        }
        return value;
    }
    uint32_t helper(uint32_t type, const Words& original_flags) {
        Words flags = original_flags;
        // A whole-input alignment promise does not imply that each member has
        // that alignment. Keep ordering/visibility operands, discard this hint.
        if (!flags.empty() && (flags[0] & spv::MemoryAccessAlignedMask)) {
            flags[0] &= ~uint32_t(spv::MemoryAccessAlignedMask); flags.erase(flags.begin() + 1);
        }
        const auto key = std::make_pair(type, flags);
        const auto known = helpers.find(key);
        if (known != helpers.end()) return known->second;
        const auto source = defs.at(type).w;
        const bool has_input = native_type(type) != 0;
        const auto input_pointer = has_input ? pointer(spv::StorageClassInput, native_type(type)) : 0;
        const uint32_t fn_type = id(), fn = id(), address = id(), entry = id();
        helpers[key] = fn;
        if (has_input) emit(declarations, spv::OpTypeFunction, {fn_type, type, input_pointer});
        else emit(declarations, spv::OpTypeFunction, {fn_type, type});
        Words body;
        emit(body, spv::OpFunction, {type, fn, 0, fn_type});
        if (has_input) emit(body, spv::OpFunctionParameter, {input_pointer, address});
        emit(body, spv::OpLabel, {entry});
        if ((source[0] & 0xffffu) == spv::OpTypeStruct) {
            Words result{0, type, id()};
            for (uint32_t m = 0; m + 2 < source.size(); ++m) {
                const uint32_t element = source[m + 2];
                if (removed(type, m)) { result.push_back(constant(body, element)); continue; }
                const uint32_t ptr = id();
                emit(body, spv::OpAccessChain, {pointer(spv::StorageClassInput, native_type(element)),
                    ptr, address, index(member_index(type, m))});
                result.push_back(read(body, element, ptr, flags));
            }
            result[0] = (uint32_t(result.size()) << 16) | spv::OpCompositeConstruct;
            append(body, result); emit(body, spv::OpReturnValue, {result[2]});
        } else if ((source[0] & 0xffffu) == spv::OpTypeArray) {
            const auto element = source[2], length = source[3], integer = defs.at(length).type;
            const uint32_t local = id(), header = id(), loop = id(), cont = id(), merge = id();
            const uint32_t iteration = id(), next_index = id(), condition = id();
            emit(body, spv::OpVariable, {pointer(spv::StorageClassFunction, type), local, spv::StorageClassFunction});
            emit(body, spv::OpBranch, {header}); emit(body, spv::OpLabel, {header});
            emit(body, spv::OpPhi, {integer, iteration, literal(integer, 0), entry, next_index, cont});
            emit(body, spv::OpULessThan, {boolean(), condition, iteration, length});
            emit(body, spv::OpLoopMerge, {merge, cont, 0});
            emit(body, spv::OpBranchConditional, {condition, loop, merge});
            emit(body, spv::OpLabel, {loop});
            const uint32_t input = id(), output = id();
            if (has_input) emit(body, spv::OpAccessChain, {pointer(spv::StorageClassInput, native_type(element)), input, address, iteration});
            emit(body, spv::OpAccessChain, {pointer(spv::StorageClassFunction, element), output, local, iteration});
            const auto value = has_input ? read(body, element, input, flags) : constant(body, element);
            emit(body, spv::OpStore, {output, value});
            emit(body, spv::OpBranch, {cont}); emit(body, spv::OpLabel, {cont});
            emit(body, spv::OpIAdd, {integer, next_index, iteration, literal(integer, 1)});
            emit(body, spv::OpBranch, {header}); emit(body, spv::OpLabel, {merge});
            const uint32_t result = id(); emit(body, spv::OpLoad, {type, result, local});
            emit(body, spv::OpReturnValue, {result});
        } else throw std::runtime_error("unsupported constant input aggregate");
        emit(body, spv::OpFunctionEnd, {}); append(functions, body);
        return fn;
    }
};
}

static bool has_capability(const uint32_t* code, size_t words, uint32_t capability) {
    if (!code || words < 5 || code[0] != spv::MagicNumber) return false;
    for (size_t at = 5; at < words;) {
        const uint32_t count = code[at] >> 16, op = code[at] & 0xffffu;
        if (!count || count > words - at) return false;
        if (op == spv::OpCapability && count == 2 && code[at + 1] == capability) return true;
        if (op == spv::OpMemoryModel) break;
        at += count;
    }
    return false;
}

bool has_device_index(const uint32_t* code, size_t words) {
    return has_capability(code, words, spv::CapabilityDeviceGroup);
}
bool has_view_index(const uint32_t* code, size_t words) {
    return has_capability(code, words, spv::CapabilityMultiView);
}
static ConstantInputLowering lower_constant_input(Words& code, uint32_t builtin, uint32_t capability,
                                                 const char* extension, uint32_t replacement, std::string& error,
                                                 bool from_device = false) {
    error.clear();
    if (!has_capability(code.data(), code.size(), capability)) return ConstantInputLowering::Unchanged;
    auto message = [&](spv_message_level_t, const char*, const spv_position_t&, const char* text) { if (text) error = text; };
    spvtools::SpirvTools validator(SPV_ENV_UNIVERSAL_1_6);
    validator.SetMessageConsumer(message);
    if (!validator.Validate(code)) return DeviceIndexLowering::Invalid;
    try {
        spvtools::Optimizer prepare(SPV_ENV_UNIVERSAL_1_6);
        prepare.SetMessageConsumer(message);
        prepare.RegisterPass(spvtools::CreateCompactIdsPass());
        prepare.RegisterPass(spvtools::CreateFlattenDecorationPass());
        Words source;
        if (!prepare.Run(code.data(), code.size(), &source)) return DeviceIndexLowering::Invalid;
        std::vector<Instruction> instructions;
        {
            std::unique_ptr<spv_context_t, decltype(&spvContextDestroy)> context(
                spvContextCreate(SPV_ENV_UNIVERSAL_1_6), spvContextDestroy);
            if (!context) throw std::bad_alloc();
            if (spvBinaryParse(context.get(), &instructions, source.data(), source.size(), nullptr, parse, nullptr) != SPV_SUCCESS)
                throw std::runtime_error("constant input instruction parsing failed");
        }
        Lowering lower; lower.next = source[3]; lower.replacement = replacement;
        std::set<uint32_t> constant_variables, omitted;
        struct Input { uint32_t type; bool constant; };
        std::map<uint32_t, Input> inputs;
        for (const auto& inst : instructions) {
            const auto& w = inst.w;
            if (inst.result) lower.defs[inst.result] = inst;
            if (builtin == spv::BuiltInViewIndex && inst.op() == spv::OpCapability && w[1] == spv::CapabilityPerViewAttributesNV)
                throw std::runtime_error("NVX per-view attributes require an additional shader lowering");
            const auto selected = [&](uint32_t value) { return value == builtin || (from_device && value == spv::BuiltInDeviceIndex); };
            if (inst.op() == spv::OpDecorate && w.size() == 4 && w[2] == spv::DecorationBuiltIn && selected(w[3]))
                constant_variables.insert(w[1]);
            if (inst.op() == spv::OpMemberDecorate && w.size() == 5 && w[3] == spv::DecorationBuiltIn && selected(w[4]))
                lower.removed_members.insert({w[1], w[2]});
        }
        if (from_device) {
            lower.index(0); // Own a uint32 type before declaring the new input.
            lower.device_input = lower.id();
            emit(lower.declarations, spv::OpVariable, {
                lower.pointer(spv::StorageClassInput, lower.uint_type), lower.device_input, spv::StorageClassInput});
        }
        for (const auto& member : lower.removed_members) lower.native_type(member.first);
        // Compute all replacements before emitting type declarations, including
        // unused Input pointer types and their array shells.
        for (const auto& inst : instructions)
            if (inst.op() == spv::OpTypePointer && inst.w[2] == spv::StorageClassInput)
                lower.native_type(inst.w[3]);
        for (const auto& inst : instructions) if (inst.op() == spv::OpVariable && inst.w[3] == spv::StorageClassInput) {
            const auto type = lower.defs.at(inst.type).w[3];
            const bool constant = constant_variables.count(inst.result) || !lower.native_type(type);
            if (constant || lower.native_type(type) != type) inputs[inst.result] = {type, constant};
            if (constant) omitted.insert(inst.result);
        }
        Words output(source.begin(), source.begin() + 5), annotations;
        if (from_device && !has_device_index(source.data(), source.size()))
            emit(output, spv::OpCapability, {spv::CapabilityDeviceGroup});
        if (from_device)
            emit(annotations, spv::OpDecorate, {lower.device_input, spv::DecorationBuiltIn, spv::BuiltInDeviceIndex});
        bool group_extension = from_device && source[1] < 0x00010300u;
        for (const auto& inst : instructions)
            if (inst.op() == spv::OpExtension &&
                !std::strcmp(reinterpret_cast<const char*>(inst.w.data() + 1), "SPV_KHR_device_group")) group_extension = false;
        size_t function_at = 0, annotation_at = 0;
        for (const auto& inst : instructions) {
            auto w = inst.w;
            if (group_extension && inst.op() != spv::OpCapability && inst.op() != spv::OpExtension) {
                const char name[] = "SPV_KHR_device_group";
                const size_t count = (sizeof(name) + 3) / 4;
                Words declaration(count + 1, 0);
                declaration[0] = (uint32_t(count + 1) << 16) | spv::OpExtension;
                std::memcpy(declaration.data() + 1, name, sizeof(name));
                append(output, declaration); group_extension = false;
            }
            if (!annotation_at && inst.op() >= spv::OpTypeVoid && inst.op() <= spv::OpTypeForwardPointer) annotation_at = output.size();
            if (!function_at && inst.op() == spv::OpFunction) function_at = output.size();
            if (inst.op() == spv::OpCapability && w[1] == capability) continue;
            if (inst.op() == spv::OpExtension && !std::strcmp(reinterpret_cast<const char*>(w.data() + 1), extension)) continue;
            if ((inst.op() == spv::OpName || inst.op() == spv::OpDecorate || inst.op() == spv::OpDecorateId) && omitted.count(w[1])) continue;
            if (inst.op() == spv::OpDecorate || inst.op() == spv::OpMemberDecorate) {
                const bool member = inst.op() == spv::OpMemberDecorate;
                const auto type = lower.native_types.find(w[1]);
                if (type != lower.native_types.end() && type->second != w[1]) {
                    if (type->second && (!member || !lower.removed(w[1], w[2]))) {
                        auto copy = w; copy[1] = type->second;
                        if (member) copy[2] = lower.member_index(w[1], w[2]);
                        append(annotations, copy);
                    }
                    if (interface_decoration(w[member ? 3 : 2])) continue;
                }
            }
            if (inst.op() == spv::OpEntryPoint) {
                const auto* name = reinterpret_cast<const char*>(w.data() + 3);
                const auto* end = static_cast<const char*>(std::memchr(name, 0, (w.size() - 3) * 4));
                if (!end) throw std::runtime_error("unterminated entry-point name");
                const size_t first = 3 + size_t(end - name) / 4 + 1;
                w.erase(std::remove_if(w.begin() + first, w.end(), [&](uint32_t id) { return omitted.count(id); }), w.end());
                if (from_device) w.push_back(lower.device_input);
                w[0] = (uint32_t(w.size()) << 16) | spv::OpEntryPoint;
            }
            if (inst.op() == spv::OpTypePointer && w[2] == spv::StorageClassInput) {
                const auto replacement = lower.native_type(w[3]);
                if (replacement) w[3] = replacement;
            }
            if (inst.op() == spv::OpVariable && omitted.count(inst.result)) continue;
            if ((inst.op() == spv::OpAccessChain || inst.op() == spv::OpInBoundsAccessChain || inst.op() == spv::OpCopyObject) && inputs.count(w[3])) {
                auto input = inputs.at(w[3]);
                if (inst.op() != spv::OpCopyObject) for (size_t i = 4; i < w.size(); ++i) {
                    const auto& type = lower.defs.at(input.type).w;
                    if ((type[0] & 0xffffu) == spv::OpTypeStruct) {
                        const auto& index = lower.defs.at(w[i]).w;
                        if ((index[0] & 0xffffu) != spv::OpConstant || index.size() < 4) throw std::runtime_error("input structure index is not constant");
                        const uint32_t member = index[3];
                        const bool constant = lower.removed(input.type, member);
                        if (!input.constant && !constant) w[i] = lower.literal(index[1], lower.member_index(input.type, member));
                        input.constant |= constant; input.type = type.at(member + 2);
                    } else if ((type[0] & 0xffffu) == spv::OpTypeArray || (type[0] & 0xffffu) == spv::OpTypeVector || (type[0] & 0xffffu) == spv::OpTypeMatrix)
                        input.type = type[2];
                    else throw std::runtime_error("unsupported input access chain");
                }
                inputs[inst.result] = input;
                if (input.constant) { omitted.insert(inst.result); continue; }
            }
            if (inst.op() == spv::OpLoad && inputs.count(w[3])) {
                const auto& input = inputs.at(w[3]);
                const auto value = input.constant ? lower.constant(output, inst.type) : lower.read(output, inst.type, w[3], Words(w.begin() + 4, w.end()));
                emit(output, spv::OpCopyObject, {inst.type, inst.result, value}); continue;
            }
            if (inst.op() == spv::OpCopyMemory && inputs.count(w[2])) {
                const auto& input = inputs.at(w[2]);
                const size_t split = inst.masks.size() > 1 ? inst.masks[1] : w.size();
                const Words target_flags(w.begin() + 3, w.begin() + split);
                const Words source_flags = inst.masks.size() > 1 ? Words(w.begin() + split, w.end()) : target_flags;
                const auto value = input.constant ? lower.constant(output, input.type) : lower.read(output, input.type, w[2], source_flags);
                Words store{0, w[1], value}; append(store, target_flags);
                store[0] = (uint32_t(store.size()) << 16) | spv::OpStore; append(output, store); continue;
            }
            if (inst.op() != spv::OpEntryPoint)
                for (const auto operand : inst.ids) if (omitted.count(w[operand]))
                    throw std::runtime_error("unsupported use of a constant built-in input pointer");
            append(output, w);
            const auto clone = lower.cloned_types.find(inst.result);
            if (clone != lower.cloned_types.end()) append(output, clone->second);
        }
        if (!function_at || !annotation_at) throw std::runtime_error("constant input module has no executable declarations");
        output.insert(output.begin() + function_at, lower.declarations.begin(), lower.declarations.end());
        output.insert(output.begin() + annotation_at, annotations.begin(), annotations.end());
        append(output, lower.functions); output[3] = lower.next;
        // Debug names and annotations precede the function-local pointer
        // aliases they reference. Their removal set is complete only now.
        Words cleaned(output.begin(), output.begin() + 5);
        for (size_t at = 5; at < output.size();) {
            const uint32_t count = output[at] >> 16, op = output[at] & 0xffffu;
            const bool metadata = op == spv::OpName || op == spv::OpDecorate || op == spv::OpDecorateId;
            if (!metadata || !omitted.count(output[at + 1]))
                cleaned.insert(cleaned.end(), output.begin() + at, output.begin() + at + count);
            at += count;
        }
        // Input pointer parameters are only an intermediate representation.
        // Inline all helpers before Vulkan admission; keep caller specializations.
        spvtools::Optimizer finish(SPV_ENV_UNIVERSAL_1_6);
        finish.SetMessageConsumer(message);
        finish.RegisterPass(spvtools::CreateInlineExhaustivePass());
        finish.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
        finish.RegisterPass(spvtools::CreateRemoveDuplicatesPass());
        spvtools::OptimizerOptions options; options.set_run_validator(false);
        Words result;
        if (!finish.Run(cleaned.data(), cleaned.size(), &result, options) || !validator.Validate(result)) return DeviceIndexLowering::Invalid;
        code = std::move(result);
        return DeviceIndexLowering::Changed;
    } catch (const std::runtime_error& failure) {
        error = failure.what();
    } catch (const std::out_of_range&) {
        error = "unknown constant input operand or type";
    }
    return DeviceIndexLowering::Invalid;
}
DeviceIndexLowering lower_singleton_device_index(Words& code, std::string& error) {
    return lower_constant_input(code, spv::BuiltInDeviceIndex, spv::CapabilityDeviceGroup,
                                "SPV_KHR_device_group", 0, error);
}
ConstantInputLowering lower_view_index(Words& code, uint32_t view, std::string& error) {
    if (view >= 32) { error = "ViewIndex is outside the 32-bit view mask"; return ConstantInputLowering::Invalid; }
    return lower_constant_input(code, spv::BuiltInViewIndex, spv::CapabilityMultiView,
                                "SPV_KHR_multiview", view, error);
}
ConstantInputLowering lower_view_index_from_device(Words& code, std::string& error) {
    return lower_constant_input(code, spv::BuiltInViewIndex, spv::CapabilityMultiView,
                                "SPV_KHR_multiview", 0, error, true);
}
}

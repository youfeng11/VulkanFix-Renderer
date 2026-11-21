#include "relaxed_block_layout.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
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
    uint32_t type = 0, result = 0;
    uint32_t op() const { return w[0] & 0xffff; }
};
spv_result_t parse(void* data, const spv_parsed_instruction_t* inst) {
    Instruction out{{inst->words, inst->words + inst->num_words}, {}, {}, inst->type_id, inst->result_id};
    for (uint16_t i = 0; i < inst->num_operands; ++i) {
        const auto& operand = inst->operands[i];
        if (operand.type == SPV_OPERAND_TYPE_ID || operand.type == SPV_OPERAND_TYPE_SCOPE_ID ||
            operand.type == SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID) out.ids.push_back(operand.offset);
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
void finish(Words& w, spv::Op op) { w[0] = (uint32_t(w.size()) << 16) | uint32_t(op); }
bool memory_class(uint32_t storage) {
    return storage == spv::StorageClassUniform || storage == spv::StorageClassStorageBuffer ||
        storage == spv::StorageClassPushConstant;
}
Words unaligned(Words flags) {
    // Splitting an aggregate changes each address's alignment. Omit the
    // original aggregate hint; retain volatile, availability and visibility.
    if (!flags.empty() && (flags[0] & spv::MemoryAccessAlignedMask)) {
        flags[0] &= ~uint32_t(spv::MemoryAccessAlignedMask);
        flags.erase(flags.begin() + 1);
    }
    return flags;
}
struct Pointer {
    uint32_t type = 0, storage = 0, native = 0;
    Words components;
    uint32_t selector = 0;
};
struct Member { uint32_t first, count, type; };
struct Lowering {
    std::map<uint32_t, const Instruction*> defs;
    std::vector<Instruction> instructions;
    std::map<uint32_t, uint32_t> memory_types;
    std::map<uint32_t, std::vector<Member>> members;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> pointer_types, constants;
    std::map<uint32_t, uint32_t> literal_values, generated_types;
    std::map<uint32_t, Instruction> generated_defs;
    std::map<uint32_t, uint32_t> widened;
    std::map<uint32_t, Pointer> pointers;
    std::set<uint32_t> omitted;
    Words declarations, annotations, helpers;
    uint32_t next = 0, uint_type = 0, void_type = 0, bool_type = 0;
    uint32_t id() { return next++; }
    const Instruction& def(uint32_t type) const { return *defs.at(type); }
    uint32_t value_type(uint32_t value) const {
        const auto found = defs.find(value);
        return found != defs.end() ? found->second->type : generated_types.at(value);
    }
    uint32_t pointer(uint32_t storage, uint32_t type) {
        auto& result = pointer_types[{storage, type}];
        if (!result) { result = id(); emit(declarations, spv::OpTypePointer, {result, storage, type}); }
        return result;
    }
    uint32_t literal(uint32_t type, uint32_t value) {
        auto& result = constants[{type, value}];
        if (!result) {
            result = id();
            if (def(type).w[2] == 64) emit(declarations, spv::OpConstant, {type, result, value, 0});
            else emit(declarations, spv::OpConstant, {type, result, value});
        }
        literal_values[result] = value;
        return result;
    }
    uint32_t index(uint32_t value) { return literal(uint_type, value); }
    uint32_t declare(Words w, spv::Op op) {
        const auto result = id(); w.insert(w.begin(), {0, result}); finish(w, op);
        append(declarations, w); generated_defs[result] = {w, {}, {}, 0, result};
        defs[result] = &generated_defs.at(result); return result;
    }
    uint32_t wide(uint32_t type) {
        if (auto found = widened.find(type); found != widened.end()) return found->second;
        const auto& w = def(type).w; const auto op = def(type).op();
        uint32_t result = type;
        if (op == spv::OpTypeVector) {
            const auto element = wide(w[2]);
            if (element != w[2]) result = declare({element, w[3]}, spv::OpTypeVector);
        } else if ((op == spv::OpTypeFloat || op == spv::OpTypeInt) && w[2] < 32) {
            // Storage-only 8/16-bit capabilities permit loads/stores and width
            // conversions, but not CompositeConstruct/Extract at that width.
            for (const auto& item : defs) if (item.second->op() == op && item.second->w[2] == 32 &&
                (op != spv::OpTypeInt || item.second->w[3] == w[3])) { result = item.first; break; }
            if (result == type) result = op == spv::OpTypeFloat ? declare({32}, spv::OpTypeFloat) : declare({32, w[3]}, spv::OpTypeInt);
        }
        widened[type] = result; return result;
    }
    void convert(Words& out, uint32_t to, uint32_t result, uint32_t value) {
        const auto& type = def(to); const auto scalar = type.op() == spv::OpTypeVector ? type.w[2] : to;
        const auto& base = def(scalar);
        emit(out, base.op() == spv::OpTypeFloat ? spv::OpFConvert : base.w[3] ? spv::OpSConvert : spv::OpUConvert,
            {to, result, value});
        generated_types[result] = to;
    }
    void initialize(const Words& code) {
        auto context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
        auto result = spvBinaryParse(context, &instructions, code.data(), code.size(), nullptr, parse, nullptr);
        spvContextDestroy(context);
        if (result != SPV_SUCCESS) throw std::runtime_error("block-layout instruction parsing failed");
        next = code[3];
        for (const auto& inst : instructions) {
            if (inst.result) defs[inst.result] = &inst;
            if (inst.op() == spv::OpTypeVoid) void_type = inst.result;
            if (inst.op() == spv::OpTypeBool) bool_type = inst.result;
            if (inst.op() == spv::OpTypeInt && inst.w[2] == 32) uint_type = inst.result;
            if (inst.op() == spv::OpTypePointer) pointer_types[{inst.w[2], inst.w[3]}] = inst.result;
            if (inst.op() == spv::OpConstant && inst.w.size() == 4) {
                constants[{inst.type, inst.w[3]}] = inst.result; literal_values[inst.result] = inst.w[3];
            }
        }
        if (!void_type) throw std::runtime_error("block-layout module lacks void type");
        if (!uint_type) uint_type = declare({32, 0}, spv::OpTypeInt);
        if (!bool_type) { bool_type = id(); emit(declarations, spv::OpTypeBool, {bool_type}); }
    }
    void decorations(uint32_t original, uint32_t native) {
        for (const auto& inst : instructions) {
            auto w = inst.w;
            if (inst.op() == spv::OpDecorate && w[1] == original) { w[1] = native; append(annotations, w); }
            if (inst.op() != spv::OpMemberDecorate || w[1] != original) continue;
            const auto& member = members.at(original).at(w[2]);
            for (uint32_t c = 0; c < member.count; ++c) {
                auto copy = w; copy[1] = native; copy[2] = member.first + c;
                if (w[3] == spv::DecorationOffset && member.count > 1)
                    copy[4] += c * (def(def(member.type).w[2]).w[2] / 8);
                append(annotations, copy);
            }
        }
    }
    uint32_t memory_type(uint32_t type) {
        if (auto found = memory_types.find(type); found != memory_types.end()) return found->second;
        const auto& inst = def(type); const auto& w = inst.w;
        memory_types[type] = type;
        if (inst.op() == spv::OpTypeStruct) {
            Words fields;
            auto& mapping = members[type];
            for (size_t m = 2; m < w.size(); ++m) {
                const auto& child = def(w[m]);
                const auto count = child.op() == spv::OpTypeVector ? child.w[3] : 1u;
                mapping.push_back({uint32_t(fields.size()), count, w[m]});
                if (child.op() == spv::OpTypeVector) fields.insert(fields.end(), count, child.w[2]);
                else fields.push_back(memory_type(w[m]));
            }
            const bool changed = fields.size() != w.size() - 2 || !std::equal(fields.begin(), fields.end(), w.begin() + 2);
            if (changed) {
                const auto native = id(); memory_types[type] = native;
                Words declaration{0, native}; append(declaration, fields); finish(declaration, spv::OpTypeStruct);
                append(declarations, declaration); decorations(type, native);
            }
        } else if (inst.op() == spv::OpTypeArray || inst.op() == spv::OpTypeRuntimeArray) {
            const auto element = memory_type(w[2]);
            if (element != w[2]) {
                auto copy = w; copy[1] = id(); copy[2] = element; memory_types[type] = copy[1];
                append(declarations, copy); decorations(type, copy[1]);
            }
        }
        return memory_types[type];
    }
    Pointer scalar(uint32_t type, uint32_t storage, uint32_t native) { return {type, storage, native, {}, 0}; }
    uint32_t representation(uint32_t storage, uint32_t type) {
        return memory_class(storage) ? memory_type(type) : type;
    }
    Pointer access(Words& out, const Pointer& source, const Words& indices) {
        Pointer selected = source;
        Words path;
        auto flush = [&] {
            if (path.empty()) return;
            const auto result = id();
            Words inst{0, pointer(selected.storage, representation(selected.storage, selected.type)), result, selected.native};
            append(inst, path); finish(inst, spv::OpAccessChain); append(out, inst);
            selected.native = result; path.clear();
        };
        for (const auto offset : indices) {
            const auto& type = def(selected.type);
            if (type.op() == spv::OpTypeStruct) {
                const auto constant = literal_values.find(offset);
                if (constant == literal_values.end())
                    throw std::runtime_error("nonconstant buffer structure index");
                if (!memory_class(selected.storage)) {
                    path.push_back(offset); selected.type = type.w.at(constant->second + 2); continue;
                }
                const auto& field = members.at(selected.type).at(constant->second);
                const auto& child = def(field.type);
                if (child.op() == spv::OpTypeVector) {
                    flush(); selected.components.clear();
                    for (uint32_t c = 0; c < field.count; ++c) {
                        const auto result = id();
                        emit(out, spv::OpAccessChain, {pointer(selected.storage, child.w[2]), result, selected.native, index(field.first + c)});
                        selected.components.push_back(result);
                    }
                    selected.native = 0; selected.type = field.type;
                } else { path.push_back(index(field.first)); selected.type = field.type; }
            } else if (type.op() == spv::OpTypeVector && !selected.components.empty()) {
                if (auto constant = literal_values.find(offset); constant != literal_values.end()) {
                    selected.native = selected.components.at(constant->second); selected.components.clear();
                } else selected.selector = offset;
                selected.type = type.w[2];
            } else if (type.op() == spv::OpTypeArray || type.op() == spv::OpTypeRuntimeArray ||
                       type.op() == spv::OpTypeMatrix || type.op() == spv::OpTypeVector) {
                path.push_back(offset); selected.type = type.w[2];
            } else throw std::runtime_error("invalid block-layout access chain");
        }
        flush();
        return selected;
    }
    // Switch helpers touch only the selected scalar address. Reading or writing
    // all vector components would introduce races between independent elements.
    void dynamic_operation(Words& out, const Instruction& original, size_t operand, const Pointer& selected) {
        const auto return_type = original.type ? wide(original.type) : void_type;
        const auto fn_type = id(), fn = id();
        Words signature{0, fn_type, return_type}, call{0, return_type,
            original.result && return_type == original.type ? original.result : id(), fn};
        Words body;
        std::vector<uint32_t> addresses;
        for (auto address : selected.components) {
            const auto ptype = pointer(selected.storage, memory_type(selected.type)), parameter = id();
            signature.push_back(ptype); call.push_back(address); addresses.push_back(parameter);
            emit(body, spv::OpFunctionParameter, {ptype, parameter});
        }
        const auto selector_type = value_type(selected.selector), selector = id();
        signature.push_back(selector_type); call.push_back(selected.selector);
        emit(body, spv::OpFunctionParameter, {selector_type, selector});
        auto operation = original.w;
        for (auto at : original.ids) if (at != operand) {
            const auto type = value_type(operation[at]), parameter = id();
            signature.push_back(type); call.push_back(operation[at]); operation[at] = parameter;
            emit(body, spv::OpFunctionParameter, {type, parameter});
        }
        finish(signature, spv::OpTypeFunction); append(declarations, signature);
        Words function; emit(function, spv::OpFunction, {return_type, fn, 0, fn_type}); append(function, body);
        const auto entry = id(), merge = id(), fallback = id();
        emit(function, spv::OpLabel, {entry});
        Words labels; for (size_t i = 0; i < addresses.size(); ++i) labels.push_back(id());
        emit(function, spv::OpSelectionMerge, {merge, 0});
        Words branch{0, selector, fallback};
        for (uint32_t i = 0; i < labels.size(); ++i) {
            branch.push_back(i); if (def(selector_type).w[2] == 64) branch.push_back(0);
            branch.push_back(labels[i]);
        }
        finish(branch, spv::OpSwitch); append(function, branch);
        Words phi{0, return_type, id()};
        for (uint32_t i = 0; i < labels.size(); ++i) {
            emit(function, spv::OpLabel, {labels[i]});
            auto inst = operation; inst[operand] = addresses[i];
            if (original.result) inst[2] = id();
            append(function, inst);
            if (original.result) {
                auto value = inst[2];
                if (return_type != original.type) { const auto converted = id(); convert(function, return_type, converted, value); value = converted; }
                phi.push_back(value); phi.push_back(labels[i]);
            }
            emit(function, spv::OpBranch, {merge});
        }
        emit(function, spv::OpLabel, {fallback});
        if (original.result) {
            const auto undefined = id(); emit(declarations, spv::OpUndef, {return_type, undefined});
            phi.push_back(undefined); phi.push_back(fallback);
        }
        emit(function, spv::OpBranch, {merge}); emit(function, spv::OpLabel, {merge});
        if (original.result) { finish(phi, spv::OpPhi); append(function, phi); emit(function, spv::OpReturnValue, {phi[2]}); }
        else emit(function, spv::OpReturn, {});
        emit(function, spv::OpFunctionEnd, {}); append(helpers, function);
        finish(call, spv::OpFunctionCall); append(out, call);
        if (original.result && return_type != original.type) convert(out, original.type, original.result, call[2]);
    }
    uint32_t load(Words& out, const Pointer& p, const Words& flags, uint32_t desired = 0) {
        const auto result = desired ? desired : id();
        generated_types[result] = p.type;
        if (p.selector) {
            Instruction inst{{0, p.type, result, 0}, {3}, {}, p.type, result};
            append(inst.w, flags); finish(inst.w, spv::OpLoad); dynamic_operation(out, inst, 3, p); return result;
        }
        const auto& type = def(p.type);
        if (!p.components.empty()) {
            const auto native_value = wide(p.type), native_scalar = wide(type.w[2]);
            Words construct{0, native_value, native_value == p.type ? result : id()};
            for (auto address : p.components) {
                auto value = load(out, scalar(type.w[2], p.storage, address), unaligned(flags));
                if (native_scalar != type.w[2]) { const auto converted = id(); convert(out, native_scalar, converted, value); value = converted; }
                construct.push_back(value);
            }
            finish(construct, spv::OpCompositeConstruct); append(out, construct);
            if (native_value != p.type) convert(out, p.type, result, construct[2]);
        } else if (representation(p.storage, p.type) == p.type) {
            Words inst{0, p.type, result, p.native}; append(inst, flags); finish(inst, spv::OpLoad); append(out, inst);
        } else if (type.op() == spv::OpTypeStruct) {
            Words construct{0, p.type, result};
            for (uint32_t i = 0; i < type.w.size() - 2; ++i)
                construct.push_back(load(out, access(out, p, {index(i)}), unaligned(flags)));
            finish(construct, spv::OpCompositeConstruct); append(out, construct);
        } else if (type.op() == spv::OpTypeArray) array_memory(out, p, flags, 0, result);
        else throw std::runtime_error("unsupported aggregate block load");
        return result;
    }
    void store(Words& out, const Pointer& p, uint32_t value, const Words& flags) {
        if (p.selector) {
            Instruction inst{{0, 0, value}, {1, 2}, {}, 0, 0};
            append(inst.w, flags); finish(inst.w, spv::OpStore); dynamic_operation(out, inst, 1, p); return;
        }
        const auto& type = def(p.type);
        if (!p.components.empty()) {
            const auto native_value = wide(p.type), native_scalar = wide(type.w[2]);
            if (native_value != p.type) { const auto converted = id(); convert(out, native_value, converted, value); value = converted; }
            for (uint32_t i = 0; i < p.components.size(); ++i) {
                auto component = id(); emit(out, spv::OpCompositeExtract, {native_scalar, component, value, i});
                if (native_scalar != type.w[2]) { const auto converted = id(); convert(out, type.w[2], converted, component); component = converted; }
                store(out, scalar(type.w[2], p.storage, p.components[i]), component, unaligned(flags));
            }
        } else if (representation(p.storage, p.type) == p.type) {
            Words inst{0, p.native, value}; append(inst, flags); finish(inst, spv::OpStore); append(out, inst);
        } else if (type.op() == spv::OpTypeStruct) {
            for (uint32_t i = 0; i < type.w.size() - 2; ++i) {
                const auto component = id(); emit(out, spv::OpCompositeExtract, {type.w[i + 2], component, value, i});
                store(out, access(out, p, {index(i)}), component, unaligned(flags));
            }
        } else if (type.op() == spv::OpTypeArray) array_memory(out, p, flags, value, 0);
        else throw std::runtime_error("unsupported aggregate block store");
    }
    void array_memory(Words& out, const Pointer& p, const Words& flags, uint32_t value, uint32_t result) {
        const bool writing = value != 0;
        const auto& array = def(p.type); const auto element = array.w[2], length = array.w[3];
        const auto integer = def(length).type, return_type = writing ? void_type : p.type;
        const auto fn_type = id(), fn = id(), address = id(), parameter = writing ? id() : 0;
        const auto native_pointer = pointer(p.storage, memory_type(p.type));
        Words signature{0, fn_type, return_type, native_pointer}; if (writing) signature.push_back(p.type);
        finish(signature, spv::OpTypeFunction); append(declarations, signature);
        Words function; emit(function, spv::OpFunction, {return_type, fn, 0, fn_type});
        emit(function, spv::OpFunctionParameter, {native_pointer, address});
        if (writing) emit(function, spv::OpFunctionParameter, {p.type, parameter});
        const auto entry = id(), local = id(); emit(function, spv::OpLabel, {entry});
        emit(function, spv::OpVariable, {pointer(spv::StorageClassFunction, p.type), local, spv::StorageClassFunction});
        if (writing) emit(function, spv::OpStore, {local, parameter});
        const auto header = id(), body = id(), cont = id(), merge = id(), idx = id(), after = id(), condition = id();
        generated_types[idx] = integer;
        emit(function, spv::OpBranch, {header}); emit(function, spv::OpLabel, {header});
        emit(function, spv::OpPhi, {integer, idx, literal(integer, 0), entry, after, cont});
        emit(function, spv::OpULessThan, {bool_type, condition, idx, length});
        emit(function, spv::OpLoopMerge, {merge, cont, 0}); emit(function, spv::OpBranchConditional, {condition, body, merge});
        emit(function, spv::OpLabel, {body});
        const auto native_element = access(function, scalar(p.type, p.storage, address), {idx});
        const auto local_element = id();
        emit(function, spv::OpAccessChain, {pointer(spv::StorageClassFunction, element), local_element, local, idx});
        if (writing) {
            const auto item = id(); emit(function, spv::OpLoad, {element, item, local_element});
            store(function, native_element, item, unaligned(flags));
        } else {
            const auto item = load(function, native_element, unaligned(flags));
            emit(function, spv::OpStore, {local_element, item});
        }
        emit(function, spv::OpBranch, {cont}); emit(function, spv::OpLabel, {cont});
        emit(function, spv::OpIAdd, {integer, after, idx, literal(integer, 1)}); emit(function, spv::OpBranch, {header});
        emit(function, spv::OpLabel, {merge});
        if (writing) emit(function, spv::OpReturn, {});
        else { const auto loaded = id(); emit(function, spv::OpLoad, {p.type, loaded, local}); emit(function, spv::OpReturnValue, {loaded}); }
        emit(function, spv::OpFunctionEnd, {}); append(helpers, function);
        Words call{0, return_type, writing ? id() : result, fn, p.native}; if (writing) call.push_back(value);
        finish(call, spv::OpFunctionCall); append(out, call);
    }
};
}

bool has_block_vectors(const uint32_t* code, size_t words) {
    if (!code || words < 5 || code[0] != spv::MagicNumber) return false;
    std::map<uint32_t, Words> types;
    Words roots;
    for (size_t at = 5; at < words;) {
        const uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        if (!count || count > words - at) return false;
        const auto* w = code + at;
        if (op == spv::OpTypeStruct || op == spv::OpTypeArray || op == spv::OpTypeRuntimeArray ||
            op == spv::OpTypePointer || op == spv::OpTypeVector) types[w[1]] = {w, w + count};
        if (op == spv::OpVariable && count >= 4 && memory_class(w[3])) roots.push_back(w[1]);
        if (op == spv::OpFunction) break;
        at += count;
    }
    std::set<uint32_t> visited;
    std::function<bool(uint32_t)> contains = [&](uint32_t id) {
        if (!visited.insert(id).second) return false;
        const auto found = types.find(id); if (found == types.end()) return false;
        const auto& w = found->second; const auto op = w[0] & 0xffff;
        if (op == spv::OpTypePointer) return contains(w[3]);
        if (op == spv::OpTypeArray || op == spv::OpTypeRuntimeArray) return contains(w[2]);
        if (op == spv::OpTypeStruct) for (size_t i = 2; i < w.size(); ++i) {
            const auto child = types.find(w[i]);
            if ((child != types.end() && (child->second[0] & 0xffff) == spv::OpTypeVector) || contains(w[i])) return true;
        }
        return false;
    };
    for (auto root : roots) if (contains(root)) return true;
    return false;
}

BlockLayoutLowering lower_relaxed_block_layout(Words& code, std::string& error) {
    error.clear();
    if (!has_block_vectors(code.data(), code.size())) return BlockLayoutLowering::Unchanged;
    try {
        auto message = [&](spv_message_level_t, const char*, const spv_position_t&, const char* text) { if (text) error = text; };
        spvtools::SpirvTools source_validator(SPV_ENV_UNIVERSAL_1_6); source_validator.SetMessageConsumer(message);
        if (!source_validator.Validate(code)) return BlockLayoutLowering::Invalid;
        spvtools::Optimizer prepare(SPV_ENV_UNIVERSAL_1_6); prepare.SetMessageConsumer(message);
        prepare.RegisterPass(spvtools::CreateFlattenDecorationPass());
        prepare.RegisterPass(spvtools::CreateInlineExhaustivePass());
        prepare.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
        prepare.RegisterPass(spvtools::CreateCompactIdsPass());
        spvtools::OptimizerOptions options; options.set_run_validator(false);
        Words source;
        if (!prepare.Run(code.data(), code.size(), &source, options)) return BlockLayoutLowering::Invalid;
        Lowering lower; lower.initialize(source);
        Words globals, body;
        bool function = false;
        for (const auto& inst : lower.instructions) {
            const auto& w = inst.w;
            if (inst.op() == spv::OpFunction) function = true;
            if (!function) {
                if (inst.op() != spv::OpVariable) continue;
                const auto& pointer = lower.def(inst.type);
                if (memory_class(w[3])) {
                    auto copy = w; copy[1] = lower.pointer(w[3], lower.memory_type(pointer.w[3]));
                    append(globals, copy); lower.pointers[inst.result] = lower.scalar(pointer.w[3], w[3], inst.result);
                } else append(globals, w);
                continue;
            }
            if ((inst.op() == spv::OpAccessChain || inst.op() == spv::OpInBoundsAccessChain) && lower.pointers.count(w[3])) {
                auto p = lower.access(body, lower.pointers.at(w[3]), {w.begin() + 4, w.end()});
                lower.pointers[inst.result] = p; lower.omitted.insert(inst.result); continue;
            }
            if (inst.op() == spv::OpCopyObject && lower.pointers.count(w[3])) {
                lower.pointers[inst.result] = lower.pointers.at(w[3]); lower.omitted.insert(inst.result); continue;
            }
            if (inst.op() == spv::OpLoad && lower.pointers.count(w[3])) {
                lower.load(body, lower.pointers.at(w[3]), {w.begin() + 4, w.end()}, inst.result); continue;
            }
            if (inst.op() == spv::OpStore && lower.pointers.count(w[1])) {
                lower.store(body, lower.pointers.at(w[1]), w[2], {w.begin() + 3, w.end()}); continue;
            }
            if (inst.op() == spv::OpCopyMemory && (lower.pointers.count(w[1]) || lower.pointers.count(w[2]))) {
                auto address = [&](uint32_t value) {
                    if (auto p = lower.pointers.find(value); p != lower.pointers.end()) return p->second;
                    const auto& ptr = lower.def(lower.def(value).type);
                    return lower.scalar(ptr.w[3], ptr.w[2], value);
                };
                Words dst, src;
                if (!inst.masks.empty()) {
                    const size_t first = inst.masks[0], second = inst.masks.size() > 1 ? inst.masks[1] : w.size();
                    dst.assign(w.begin() + first, w.begin() + second);
                    src = inst.masks.size() > 1 ? Words(w.begin() + second, w.end()) : dst;
                }
                const auto value = lower.load(body, address(w[2]), src);
                lower.store(body, address(w[1]), value, dst); continue;
            }
            if (inst.op() == spv::OpArrayLength && lower.pointers.count(w[3])) {
                auto copy = w; const auto& p = lower.pointers.at(w[3]); copy[3] = p.native;
                copy[4] = lower.members.at(p.type).at(w[4]).first; append(body, copy); continue;
            }
            auto copy = w;
            bool handled = false;
            for (const auto at : inst.ids) if (auto found = lower.pointers.find(w[at]); found != lower.pointers.end()) {
                const auto& p = found->second;
                if (p.selector) {
                    lower.dynamic_operation(body, inst, at, p); handled = true; break;
                }
                if (!p.native) throw std::runtime_error("aggregate buffer pointer requires scalar memory access");
                copy[at] = p.native;
            }
            if (!handled) append(body, copy);
        }
        if (lower.declarations.empty()) return BlockLayoutLowering::Unchanged;
        Words output(source.begin(), source.begin() + 5), types;
        for (const auto& inst : lower.instructions) {
            if (inst.op() == spv::OpFunction) break;
            if (inst.op() == spv::OpVariable) continue;
            const bool type = (inst.op() >= spv::OpTypeVoid && inst.op() <= spv::OpTypeForwardPointer) ||
                inst.op() == spv::OpConstant || inst.op() == spv::OpConstantComposite || inst.op() == spv::OpConstantNull ||
                inst.op() == spv::OpConstantTrue || inst.op() == spv::OpConstantFalse || inst.op() == spv::OpUndef ||
                (inst.op() >= spv::OpSpecConstantTrue && inst.op() <= spv::OpSpecConstantOp);
            if (type) append(types, inst.w);
            else if (!((inst.op() == spv::OpName || inst.op() == spv::OpDecorate) && lower.omitted.count(inst.w[1])))
                append(output, inst.w);
        }
        append(output, lower.annotations); append(output, types); append(output, lower.declarations);
        append(output, globals); append(output, body); append(output, lower.helpers); output[3] = lower.next;
        spvtools::Optimizer cleanup(SPV_ENV_UNIVERSAL_1_6); cleanup.SetMessageConsumer(message);
        cleanup.RegisterPass(spvtools::CreateInlineExhaustivePass());
        cleanup.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
        cleanup.RegisterPass(spvtools::CreateRemoveDuplicatesPass());
        Words cleaned;
        if (!cleanup.Run(output.data(), output.size(), &cleaned, options)) return BlockLayoutLowering::Invalid;
        spvtools::SpirvTools validator(SPV_ENV_UNIVERSAL_1_6); validator.SetMessageConsumer(message);
        if (!validator.Validate(cleaned)) return BlockLayoutLowering::Invalid;
        code = std::move(cleaned); return BlockLayoutLowering::Changed;
    } catch (const std::runtime_error& failure) {
        error = failure.what(); return BlockLayoutLowering::Invalid;
    } catch (const std::out_of_range& failure) {
        error = failure.what(); return BlockLayoutLowering::Invalid;
    }
}
}

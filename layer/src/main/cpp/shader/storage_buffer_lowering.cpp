#include "storage_buffer_lowering.h"
#include "spirv_extensions.h"
#include <functional>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>

namespace vkfix {
namespace {
using Words = std::vector<uint32_t>;
struct Instruction {
    Words w;
    uint32_t type, result;
    uint32_t op() const { return w[0] & 0xffff; }
};
spv_result_t parse(void* state, const spv_parsed_instruction_t* inst) {
    static_cast<std::vector<Instruction>*>(state)->push_back({
        {inst->words, inst->words + inst->num_words}, inst->type_id, inst->result_id});
    return SPV_SUCCESS;
}
void emit(Words& out, spv::Op op, std::initializer_list<uint32_t> operands) {
    out.push_back((uint32_t(operands.size() + 1) << 16) | uint32_t(op));
    out.insert(out.end(), operands);
}
void append(Words& out, const Words& instruction) { out.insert(out.end(), instruction.begin(), instruction.end()); }
bool descriptor_indices_static(const Words& code) {
    std::vector<Instruction> instructions;
    auto context=spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
    auto status=spvBinaryParse(context,&instructions,code.data(),code.size(),nullptr,parse,nullptr);
    spvContextDestroy(context);
    if (status!=SPV_SUCCESS) return false;
    std::map<uint32_t,const Instruction*> definitions;
    std::set<uint32_t> blocks,buffer_blocks;
    for (const auto& inst:instructions) {
        if (inst.result) definitions[inst.result]=&inst;
        if (inst.op()==spv::OpDecorate && inst.w.size()==3) {
            if (inst.w[2]==spv::DecorationBlock) blocks.insert(inst.w[1]);
            if (inst.w[2]==spv::DecorationBufferBlock) buffer_blocks.insert(inst.w[1]);
        }
    }
    for (const auto& inst:instructions) {
        if (inst.op()!=spv::OpAccessChain && inst.op()!=spv::OpInBoundsAccessChain) continue;
        auto base=definitions.find(inst.w[3]);
        if (base==definitions.end()) return false;
        auto pointer=definitions.find(base->second->type);
        if (pointer==definitions.end() || pointer->second->op()!=spv::OpTypePointer) continue;
        const auto storage=pointer->second->w[2];
        uint32_t type=pointer->second->w[3], root=type;
        auto element=definitions.find(root);
        while (element!=definitions.end() && element->second->op()==spv::OpTypeArray) {
            root=element->second->w[2]; element=definitions.find(root);
        }
        if (!(storage==spv::StorageClassStorageBuffer && blocks.count(root)) &&
            !(storage==spv::StorageClassUniform && buffer_blocks.count(root))) continue;
        for (size_t i=4;i<inst.w.size();++i) {
            auto array=definitions.find(type);
            if (array==definitions.end() || array->second->op()!=spv::OpTypeArray) break;
            auto index=definitions.find(inst.w[i]);
            if (index==definitions.end() || (index->second->op()!=spv::OpConstant &&
                index->second->op()!=spv::OpSpecConstant && index->second->op()!=spv::OpSpecConstantOp)) return false;
            type=array->second->w[2];
        }
    }
    return true;
}
}

bool has_storage_buffer_class(const uint32_t* code, size_t count) {
    if (!code || count < 5 || code[0] != spv::MagicNumber) return false;
    for (size_t at = 5; at < count;) {
        const uint32_t wc = code[at] >> 16, op = code[at] & 0xffff;
        if (!wc || at + wc > count) return false;
        if (op == spv::OpExtension && wc > 1) {
            const auto* text = reinterpret_cast<const char*>(code + at + 1);
            if (std::memchr(text, 0, (wc - 1) * 4) &&
                !std::strcmp(text, "SPV_KHR_storage_buffer_storage_class")) return true;
        }
        if (op == spv::OpTypePointer && wc == 4 && code[at + 2] == spv::StorageClassStorageBuffer) return true;
        if (op == spv::OpFunction) break;
        at += wc;
    }
    return false;
}

bool has_storage_buffer_array_access(const uint32_t* code, size_t count) {
    if (!code || count<5 || code[0]!=spv::MagicNumber) return false;
    std::set<uint32_t> arrays, array_pointers, values;
    for (size_t at=5; at<count;) {
        const uint32_t wc=code[at]>>16, op=code[at]&0xffff;
        if (!wc || at+wc>count) return false;
        const auto* w=code+at;
        if (op==spv::OpTypeArray && wc==4) arrays.insert(w[1]);
        if (op==spv::OpTypePointer && wc==4 && w[2]==spv::StorageClassStorageBuffer && arrays.count(w[3]))
            array_pointers.insert(w[1]);
        if (op==spv::OpFunction && array_pointers.empty()) return false;
        switch (op) {
        case spv::OpVariable: case spv::OpAccessChain: case spv::OpInBoundsAccessChain:
        case spv::OpPtrAccessChain: case spv::OpInBoundsPtrAccessChain:
        case spv::OpCopyObject: case spv::OpSelect: case spv::OpPhi:
        case spv::OpFunctionParameter: case spv::OpFunctionCall: case spv::OpLoad:
        case spv::OpConstantNull: case spv::OpUndef: case spv::OpCompositeExtract:
            if (wc>=3 && array_pointers.count(w[1])) values.insert(w[2]);
            break;
        default: break;
        }
        if (op==spv::OpLoad && wc>=4 && arrays.count(w[1]) && values.count(w[3])) return true;
        if (op==spv::OpStore && wc>=3 && values.count(w[1])) return true;
        if (op==spv::OpCopyMemory && wc>=3 && (values.count(w[1]) || values.count(w[2]))) return true;
        at+=wc;
    }
    return false;
}

static StorageBufferLowering transform_storage_buffers(Words& code, std::string& error, bool legacy,
                                                       uint32_t native_extensions = 0) {
    error.clear();
    auto invalid = [&](const char* reason) { error = reason; return StorageBufferLowering::Invalid; };
    if (code.size() < 5 || code[0] != spv::MagicNumber) return invalid("invalid SPIR-V header");
    spvtools::SpirvTools source(SPV_ENV_UNIVERSAL_1_6);
    if (!source.Validate(code)) return invalid("invalid source module");
    // This pass can run between version lowering and final Vulkan validation.
    // Flatten annotations before cloning memory types; never strip decorations.
    spvtools::Optimizer optimizer(SPV_ENV_UNIVERSAL_1_6);
    optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
    optimizer.RegisterPass(spvtools::CreateFlattenDecorationPass());
    spvtools::OptimizerOptions options;
    options.set_run_validator(false);
    Words input;
    if (!optimizer.Run(code.data(), code.size(), &input, options)) return invalid("decoration normalization failed");
    std::vector<Instruction> instructions;
    const auto context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
    const auto result = spvBinaryParse(context, &instructions, input.data(), input.size(), nullptr, parse, nullptr);
    spvContextDestroy(context);
    if (result != SPV_SUCCESS) return invalid("instruction parsing failed");
    std::map<uint32_t, const Instruction*> definitions;
    std::set<uint32_t> blocks;
    bool has_storage = false, dynamic_indexing=false;
    for (const auto& inst : instructions) {
        if (inst.result) definitions[inst.result] = &inst;
        const auto& w = inst.w;
        if (inst.op()==spv::OpCapability && w.size()==2 && w[1]==spv::CapabilityStorageBufferArrayDynamicIndexing)
            dynamic_indexing=true;
        if (inst.op() == spv::OpDecorate && w.size() == 3 && w[2] == spv::DecorationBlock) blocks.insert(w[1]);
        if (inst.op() == spv::OpTypePointer && w.size() == 4 && w[2] == spv::StorageClassStorageBuffer) has_storage = true;
        if (inst.op() == spv::OpExtension && w.size() > 1 &&
            !std::strcmp(reinterpret_cast<const char*>(w.data() + 1), "SPV_KHR_storage_buffer_storage_class")) has_storage = true;
    }
    if (!has_storage) return StorageBufferLowering::Unchanged;
    uint32_t bound = input[3];
    std::map<uint32_t, uint32_t> memory_types;
    std::map<uint32_t, Words> cloned_types;
    Words annotations;
    // Clone just the block and its descriptor-array shells, not scalar/member
    // types. SSA values retain their application types at memory boundaries.
    std::function<uint32_t(uint32_t)> clone = [&](uint32_t type) -> uint32_t {
        const auto existing = memory_types.find(type);
        if (existing != memory_types.end()) return existing->second;
        const auto it = definitions.find(type);
        if (it == definitions.end()) return 0;
        const auto& inst = *it->second;
        Words w = inst.w;
        if (inst.op() == spv::OpTypeStruct && blocks.count(type)) {
            w[1] = bound++;
        } else if (inst.op() == spv::OpTypeArray || inst.op() == spv::OpTypeRuntimeArray) {
            const uint32_t element = clone(w[2]);
            if (!element) return 0;
            if (element == w[2]) return type;
            w[1] = bound++; w[2] = element;
        } else return type;
        memory_types[type] = w[1];
        cloned_types[type] = w;
        return w[1];
    };
    if (legacy) for (const auto& inst : instructions)
        if (inst.op() == spv::OpTypePointer && inst.w[2] == spv::StorageClassStorageBuffer)
            if (!clone(inst.w[3])) return invalid("unknown storage-buffer pointee type");
    for (const auto& inst : instructions) {
        const auto& w = inst.w;
        if ((inst.op() == spv::OpDecorate || inst.op() == spv::OpMemberDecorate) &&
            memory_types.count(w[1])) {
            auto copy = w; copy[1] = memory_types.at(w[1]);
            if (inst.op() == spv::OpDecorate && w[2] == spv::DecorationBlock) copy[2] = spv::DecorationBufferBlock;
            append(annotations, copy);
        }
    }
    auto memory_type = [&](uint32_t type) {
        const auto it = memory_types.find(type);
        return it == memory_types.end() ? type : it->second;
    };
    auto pointer_type = [&](uint32_t id) -> uint32_t {
        const auto value = definitions.find(id);
        if (value == definitions.end()) return 0;
        const auto ptr = definitions.find(value->second->type);
        if (ptr == definitions.end() || ptr->second->op() != spv::OpTypePointer) return 0;
        return ptr->second->w[3];
    };
    auto storage_pointer = [&](uint32_t id) {
        const auto value = definitions.find(id);
        if (value == definitions.end()) return false;
        const auto ptr = definitions.find(value->second->type);
        return ptr != definitions.end() && ptr->second->op() == spv::OpTypePointer &&
            ptr->second->w[2] == spv::StorageClassStorageBuffer;
    };
    auto descriptor_array = [&](uint32_t type) {
        auto it=definitions.find(type);
        if (it==definitions.end() || it->second->op()!=spv::OpTypeArray) return false;
        do {
            type=it->second->w[2]; it=definitions.find(type);
        } while (it!=definitions.end() && it->second->op()==spv::OpTypeArray);
        return blocks.count(type)!=0;
    };
    bool unsupported = false, needs_specialization=false;
    Words helper_declarations, helper_functions;
    std::map<uint32_t, uint32_t> function_pointers;
    for (const auto& def : definitions)
        if (def.second->op() == spv::OpTypePointer && def.second->w[2] == spv::StorageClassFunction)
            function_pointers[def.second->w[3]] = def.first;
    auto function_pointer = [&](uint32_t type) {
        auto& pointer = function_pointers[type];
        if (!pointer) {
            pointer = bound++;
            emit(helper_declarations, spv::OpTypePointer, {pointer, spv::StorageClassFunction, type});
        }
        return pointer;
    };
    // Convert a cloned aggregate to/from its source value type. This matters for
    // whole-block loads/stores and CopyMemory between a UBO and an SSBO sharing
    // a structure. Member accesses keep their existing types and layout offsets.
    std::function<uint32_t(Words&, uint32_t, uint32_t, bool, uint32_t)> convert;
    convert = [&](Words& out, uint32_t type, uint32_t value, bool to_memory, uint32_t desired) -> uint32_t {
        if (memory_type(type) == type) return value;
        const auto& inst = *definitions.at(type);
        const uint32_t target = to_memory ? memory_type(type) : type;
        std::vector<uint32_t> elements;
        if (inst.op() == spv::OpTypeStruct) elements.assign(inst.w.begin() + 2, inst.w.end());
        else { unsupported = true; return 0; }
        Words construct{0, target, desired ? desired : bound++};
        for (uint32_t i = 0; i < elements.size(); ++i) {
            const uint32_t element_type = elements[i], extracted = bound++;
            emit(out, spv::OpCompositeExtract,
                {to_memory ? element_type : memory_type(element_type), extracted, value, i});
            construct.push_back(convert(out, element_type, extracted, to_memory, 0));
        }
        construct[0] = (uint32_t(construct.size()) << 16) | spv::OpCompositeConstruct;
        append(out, construct);
        return construct[2];
    };
    // Access descriptor arrays one buffer at a time. A native whole-array load
    // would leave the descriptor-array aggregate operation in the legacy shader.
    Words helper_annotations;
    std::map<std::tuple<uint32_t, bool, Words>, uint32_t> memory_helpers;
    std::map<uint32_t, uint32_t> memory_pointers;
    auto memory_pointer = [&](uint32_t type) {
        auto& pointer = memory_pointers[type];
        if (!pointer) {
            pointer=bound++;
            emit(helper_declarations,spv::OpTypePointer,{pointer,
                legacy?spv::StorageClassUniform:spv::StorageClassStorageBuffer,memory_type(type)});
        }
        return pointer;
    };
    std::function<uint32_t(uint32_t,uint32_t,bool,const Words&)> memory_helper;
    memory_helper = [&](uint32_t type,uint32_t pointer_type,bool store,const Words& flags) -> uint32_t {
        Words access=flags;
        // Array elements refer to separate descriptors. Do not infer each
        // descriptor's alignment from the aggregate pointer's alignment hint.
        if (!access.empty() && (access[0]&spv::MemoryAccessAlignedMask)) {
            access[0]&=~uint32_t(spv::MemoryAccessAlignedMask);
            access.erase(access.begin()+1);
        }
        auto key=std::make_tuple(type,store,access);
        auto known=memory_helpers.find(key);
        if (known!=memory_helpers.end()) return known->second;
        const auto& array=*definitions.at(type);
        const uint32_t element=array.w[2], length=array.w[3], integer=definitions.at(length)->type;
        if (!dynamic_indexing && definitions.at(length)->op()!=spv::OpConstant) {
            error="descriptor-array specialization needs pipeline-stage processing without dynamic indexing";
            needs_specialization=true;
            unsupported=true;
            return 0; // Requires pipeline-stage specialization before static expansion.
        }
        uint32_t void_type=0;
        for (const auto& def:definitions) if (def.second->op()==spv::OpTypeVoid) void_type=def.first;
        const uint32_t fn_type=bound++, fn=bound++, pointer=bound++, parameter=store?bound++:0;
        const uint32_t return_type=store?void_type:type;
        Words signature{0,fn_type,return_type,pointer_type};
        if (store) signature.push_back(type);
        signature[0]=(uint32_t(signature.size())<<16)|spv::OpTypeFunction;
        append(helper_declarations,signature);
        emit(helper_annotations,spv::OpDecorate,{pointer,spv::DecorationAliased});
        memory_helpers[key]=fn;
        Words function;
        emit(function,spv::OpFunction,{return_type,fn,0,fn_type});
        emit(function,spv::OpFunctionParameter,{pointer_type,pointer});
        if (store) emit(function,spv::OpFunctionParameter,{type,parameter});
        const uint32_t entry=bound++, local=bound++;
        emit(function,spv::OpLabel,{entry});
        emit(function,spv::OpVariable,{function_pointer(type),local,spv::StorageClassFunction});
        if (store) emit(function,spv::OpStore,{local,parameter});
        auto copy_element=[&](uint32_t index) {
        const uint32_t mp=bound++, lp=bound++, value=bound++;
        const uint32_t mp_type=memory_pointer(element);
        emit(function,spv::OpAccessChain,{mp_type,mp,pointer,index});
        emit(function,spv::OpAccessChain,{function_pointer(element),lp,local,index});
        const bool nested=definitions.at(element)->op()==spv::OpTypeArray;
        if (store) {
            emit(function,spv::OpLoad,{element,value,lp});
            if (nested) emit(function,spv::OpFunctionCall,{void_type,bound++,memory_helper(element,mp_type,true,access),mp,value});
            else {
                Words instruction{0,mp,convert(function,element,value,true,0)};
                instruction.insert(instruction.end(),access.begin(),access.end());
                instruction[0]=(uint32_t(instruction.size())<<16)|spv::OpStore;
                append(function,instruction);
            }
        } else {
            uint32_t converted;
            if (nested) {
                emit(function,spv::OpFunctionCall,{element,value,memory_helper(element,mp_type,false,access),mp});
                converted=value;
            } else {
                Words instruction{0,memory_type(element),value,mp};
                instruction.insert(instruction.end(),access.begin(),access.end());
                instruction[0]=(uint32_t(instruction.size())<<16)|spv::OpLoad;
                append(function,instruction);
                converted=convert(function,element,value,false,0);
            }
            emit(function,spv::OpStore,{lp,converted});
        }
        };
        const bool wide=definitions.at(integer)->w[2]==64;
        if (!dynamic_indexing) {
            const auto& size=definitions.at(length)->w;
            if ((wide && size.size()>4 && size[4]) || size[3]>0x3fffff) {
                unsupported=true; error="static descriptor array exceeds the SPIR-V ID space"; return 0;
            }
            for (uint32_t i=0;i<size[3] && !unsupported;++i) {
                const uint32_t index=bound++;
                if (wide) emit(helper_declarations,spv::OpConstant,{integer,index,i,0});
                else emit(helper_declarations,spv::OpConstant,{integer,index,i});
                copy_element(index);
            }
        } else {
            uint32_t boolean=0;
            for (const auto& def:definitions) if (def.second->op()==spv::OpTypeBool) boolean=def.first;
            if (!boolean) { boolean=bound++; emit(helper_declarations,spv::OpTypeBool,{boolean}); }
            const uint32_t zero=bound++, one=bound++;
            if (wide) {
                emit(helper_declarations,spv::OpConstant,{integer,zero,0,0});
                emit(helper_declarations,spv::OpConstant,{integer,one,1,0});
            } else {
                emit(helper_declarations,spv::OpConstant,{integer,zero,0});
                emit(helper_declarations,spv::OpConstant,{integer,one,1});
            }
            const uint32_t header=bound++, body=bound++, cont=bound++, merge=bound++;
            const uint32_t index=bound++, next=bound++, condition=bound++;
            emit(function,spv::OpBranch,{header});
            emit(function,spv::OpLabel,{header});
            emit(function,spv::OpPhi,{integer,index,zero,entry,next,cont});
            emit(function,spv::OpULessThan,{boolean,condition,index,length});
            emit(function,spv::OpLoopMerge,{merge,cont,0});
            emit(function,spv::OpBranchConditional,{condition,body,merge});
            emit(function,spv::OpLabel,{body});
            copy_element(index);
            emit(function,spv::OpBranch,{cont});
            emit(function,spv::OpLabel,{cont});
            emit(function,spv::OpIAdd,{integer,next,index,one});
            emit(function,spv::OpBranch,{header});
            emit(function,spv::OpLabel,{merge});
        }
        if (store) emit(function,spv::OpReturn,{});
        else {
            const uint32_t output=bound++;
            emit(function,spv::OpLoad,{type,output,local});
            emit(function,spv::OpReturnValue,{output});
        }
        emit(function,spv::OpFunctionEnd,{});
        append(helper_functions,function);
        return fn;
    };
    Words output(input.begin(), input.begin() + 5);
    bool wrote_annotations = false;
    bool changed=legacy;
    size_t function_start = 0, annotation_start=0;
    for (const auto& inst : instructions) {
        auto w = inst.w;
        if (!function_start && inst.op() == spv::OpFunction) function_start = output.size();
        if (!wrote_annotations && inst.op() >= spv::OpTypeVoid && inst.op() <= spv::OpTypeForwardPointer) {
            annotation_start=output.size();
            append(output, annotations); wrote_annotations = true;
        }
        if (legacy && inst.op() == spv::OpTypePointer && w[2] == spv::StorageClassStorageBuffer) {
            w[2] = spv::StorageClassUniform; w[3] = memory_type(w[3]);
        } else if (legacy && inst.op() == spv::OpTypeForwardPointer && w[2] == spv::StorageClassStorageBuffer) {
            w[2] = spv::StorageClassUniform;
        } else if (legacy && inst.op() == spv::OpVariable && w[3] == spv::StorageClassStorageBuffer) {
            w[3] = spv::StorageClassUniform;
        } else if (inst.op() == spv::OpLoad && storage_pointer(w[3]) &&
            (memory_type(inst.type) != inst.type || descriptor_array(inst.type))) {
            if (definitions.at(inst.type)->op()==spv::OpTypeArray) {
                emit(output,spv::OpFunctionCall,{inst.type,w[2],memory_helper(inst.type,definitions.at(w[3])->type,false,Words(w.begin()+4,w.end())),w[3]});
                changed=true;
                continue;
            }
            const uint32_t original_id = w[2];
            w[1] = memory_type(inst.type); w[2] = bound++;
            append(output, w);
            convert(output, inst.type, w[2], false, original_id);
            continue;
        } else if (inst.op() == spv::OpStore && storage_pointer(w[1])) {
            const uint32_t type = pointer_type(w[1]);
            if (definitions.at(type)->op()==spv::OpTypeArray && (legacy || descriptor_array(type))) {
                uint32_t void_type=0;
                for (const auto& def:definitions) if (def.second->op()==spv::OpTypeVoid) void_type=def.first;
                emit(output,spv::OpFunctionCall,{void_type,bound++,memory_helper(type,definitions.at(w[1])->type,true,Words(w.begin()+3,w.end())),w[1],w[2]});
                changed=true;
                continue;
            }
            if (memory_type(type) != type) w[2] = convert(output, type, w[2], true, 0);
        } else if (inst.op() == spv::OpCopyMemory && (storage_pointer(w[1]) || storage_pointer(w[2])) &&
            (legacy || descriptor_array(pointer_type(w[2])))) {
            changed=true;
            if (w.size() > 5) return invalid("separate CopyMemory memory-access operands need further lowering");
            const uint32_t type = pointer_type(w[2]), loaded = bound++;
            if (!type) return invalid("unknown CopyMemory pointee");
            const bool array=definitions.at(type)->op()==spv::OpTypeArray;
            const Words flags(w.begin()+3,w.end());
            if (array && storage_pointer(w[2])) {
                emit(output,spv::OpFunctionCall,{type,loaded,memory_helper(type,definitions.at(w[2])->type,false,flags),w[2]});
            } else {
                Words load{0, storage_pointer(w[2]) ? memory_type(type) : type, loaded, w[2]};
                load.insert(load.end(), w.begin() + 3, w.end());
                load[0] = (uint32_t(load.size()) << 16) | spv::OpLoad;
                append(output, load);
            }
            uint32_t value = storage_pointer(w[2]) && !array ? convert(output, type, loaded, false, 0) : loaded;
            if (array && storage_pointer(w[1])) {
                uint32_t void_type=0;
                for (const auto& def:definitions) if (def.second->op()==spv::OpTypeVoid) void_type=def.first;
                emit(output,spv::OpFunctionCall,{void_type,bound++,memory_helper(type,definitions.at(w[1])->type,true,flags),w[1],value});
                continue;
            }
            if (storage_pointer(w[1])) value = convert(output, type, value, true, 0);
            Words store{0, w[1], value};
            store.insert(store.end(), w.begin() + 3, w.end());
            store[0] = (uint32_t(store.size()) << 16) | spv::OpStore;
            append(output, store);
            continue;
        } else if (legacy && inst.op() == spv::OpExtension && w.size() > 1 &&
            !std::strcmp(reinterpret_cast<const char*>(w.data() + 1), "SPV_KHR_storage_buffer_storage_class")) continue;
        append(output, w);
        const auto clone = cloned_types.find(inst.result);
        if (clone != cloned_types.end()) append(output, clone->second);
    }
    if (unsupported) {
        if (error.empty()) error="invalid aggregate value conversion";
        return needs_specialization?StorageBufferLowering::NeedsSpecialization:StorageBufferLowering::Invalid;
    }
    if (!changed) return StorageBufferLowering::Unchanged;
    if (!helper_declarations.empty()) output.insert(output.begin() + function_start,
        helper_declarations.begin(), helper_declarations.end());
    if (!helper_annotations.empty()) output.insert(output.begin()+annotation_start,
        helper_annotations.begin(),helper_annotations.end());
    append(output, helper_functions);
    output[3] = bound;
    // Uniform scalar pointer types can now coincide with existing UBO pointer
    // types. Retarget all ID operands and function signatures consistently.
    spvtools::Optimizer dedup(SPV_ENV_UNIVERSAL_1_6);
    // Uniform descriptor-array pointers cannot be function arguments in the
    // legacy Vulkan environment. Helpers are an intermediate CFG construction;
    // inline them and remove their definitions before final validation.
    dedup.RegisterPass(spvtools::CreateInlineExhaustivePass());
    dedup.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    dedup.RegisterPass(spvtools::CreateRemoveDuplicatesPass());
    Words cleaned;
    if (!dedup.Run(output.data(), output.size(), &cleaned, options)) return invalid("type normalization failed");
    if (!dynamic_indexing && !descriptor_indices_static(cleaned))
        return invalid("static descriptor-array expansion left a dynamic index");
    if (!legacy) {
        const auto version=cleaned[1];
        const auto environment=version<=0x10000 ? SPV_ENV_VULKAN_1_0 :
            version<=0x10300 ? SPV_ENV_VULKAN_1_1 :
            version<=0x10500 ? SPV_ENV_VULKAN_1_2 : SPV_ENV_VULKAN_1_3;
        spvtools::SpirvTools target(environment);
        target.SetMessageConsumer([&](spv_message_level_t,const char*,const spv_position_t&,const char* message) {
            if (error.empty() && message) error=message;
        });
        spvtools::ValidatorOptions validation;
        validation.SetRelaxBlockLayout((native_extensions & NativeRelaxedBlockLayout) != 0);
        if (!target.Validate(cleaned.data(),cleaned.size(),validation)) return StorageBufferLowering::Invalid;
    }
    code = std::move(cleaned);
    return StorageBufferLowering::Changed;
}
StorageBufferLowering lower_storage_buffer_class(Words& code,std::string& error) {
    return transform_storage_buffers(code,error,true);
}
StorageBufferLowering normalize_storage_buffer_arrays(Words& code,std::string& error,
                                                     uint32_t native_extensions) {
    if (!has_storage_buffer_array_access(code.data(),code.size())) {
        error.clear(); return StorageBufferLowering::Unchanged;
    }
    return transform_storage_buffers(code,error,false,native_extensions);
}
}

#include "subgroup_basic_lowering.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>

namespace vkfix {
namespace {
using Words=std::vector<uint32_t>;
struct Instruction {
    Words w;
    std::vector<uint16_t> scopes;
    std::vector<uint16_t> memory_masks;
    uint32_t type,result;
    uint32_t op() const { return w[0]&0xffff; }
};
spv_result_t parse(void* data,const spv_parsed_instruction_t* inst) {
    Instruction result{{inst->words,inst->words+inst->num_words},{},{},inst->type_id,inst->result_id};
    for (uint16_t i=0;i<inst->num_operands;++i) {
        const auto& operand=inst->operands[i];
        if (operand.type==SPV_OPERAND_TYPE_SCOPE_ID) result.scopes.push_back(operand.offset);
        if (operand.type==SPV_OPERAND_TYPE_MEMORY_ACCESS) result.memory_masks.push_back(operand.offset);
    }
    static_cast<std::vector<Instruction>*>(data)->push_back(std::move(result));
    return SPV_SUCCESS;
}
void append(Words& out,const Words& w) { out.insert(out.end(),w.begin(),w.end()); }
void emit(Words& out,spv::Op op,std::initializer_list<uint32_t> operands) {
    out.push_back((uint32_t(operands.size()+1)<<16)|uint32_t(op));
    out.insert(out.end(),operands);
}
bool capability(uint32_t cap) {
    return cap==spv::CapabilityGroupNonUniform || cap==spv::CapabilitySubgroupBallotKHR ||
           cap==spv::CapabilitySubgroupVoteKHR;
}
bool subgroup_builtin(uint32_t value) {
    return value==spv::BuiltInSubgroupSize || value==spv::BuiltInSubgroupLocalInvocationId ||
           value==spv::BuiltInNumSubgroups || value==spv::BuiltInSubgroupId ||
           (value>=spv::BuiltInSubgroupEqMask && value<=spv::BuiltInSubgroupLtMask);
}
bool legacy_extension(const Words& w) {
    if (w.size()<2) return false;
    const auto* name=reinterpret_cast<const char*>(w.data()+1);
    return std::memchr(name,0,(w.size()-1)*4) &&
        (!std::strcmp(name,"SPV_KHR_shader_ballot") || !std::strcmp(name,"SPV_KHR_subgroup_vote"));
}
struct Lowering {
    uint32_t next;
    Words declarations,annotations;
    std::map<uint32_t,Instruction> defs;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> literals;
    std::map<uint32_t,uint32_t> pointers;
    uint32_t id() {
        if (next>=0x3fffff) throw std::runtime_error("subgroup lowering exhausted ID space");
        return next++;
    }
    uint32_t literal(uint32_t type,uint32_t value) {
        auto& found=literals[{type,value}];
        if (!found) { found=id(); emit(declarations,spv::OpConstant,{type,found,value}); }
        return found;
    }
    uint32_t private_pointer(uint32_t source) {
        auto& found=pointers[source];
        if (!found) {
            const auto& pointer=defs.at(source).w;
            if ((pointer[0]&0xffff)!=spv::OpTypePointer) throw std::runtime_error("expected typed input pointer");
            found=id(); emit(declarations,spv::OpTypePointer,{found,spv::StorageClassPrivate,pointer[3]});
        }
        return found;
    }
    uint32_t initializer(uint32_t type,uint32_t builtin) {
        if (builtin<spv::BuiltInSubgroupEqMask || builtin>spv::BuiltInSubgroupLtMask)
            return literal(type,builtin==spv::BuiltInSubgroupSize?1:0);
        const auto& vector=defs.at(type).w;
        const auto zero=literal(vector[2],0);
        const auto first=literal(vector[2],builtin==spv::BuiltInSubgroupGtMask || builtin==spv::BuiltInSubgroupLtMask?0:1);
        const auto result=id();
        emit(declarations,spv::OpConstantComposite,{type,result,first,zero,zero,zero});
        return result;
    }
};
// Input alignment guarantees need not apply to private replacements.
void weaken_alignment(Words& w,size_t flags) {
    if (w.size()>flags && (w[flags]&spv::MemoryAccessAlignedMask)) {
        w[flags]&=~uint32_t(spv::MemoryAccessAlignedMask);
        w.erase(w.begin()+flags+1);
        w[0]=(uint32_t(w.size())<<16)|(w[0]&0xffff);
    }
}
}
bool has_basic_subgroups(const uint32_t* code,size_t size) {
    if (!code || size<5 || code[0]!=spv::MagicNumber) return false;
    for (size_t at=5;at<size;) {
        const uint32_t wc=code[at]>>16, op=code[at]&0xffff;
        if (!wc || at+wc>size) return false;
        if (op==spv::OpCapability && wc==2 && capability(code[at+1])) return true;
        if (op==spv::OpMemoryModel) break;
        at+=wc;
    }
    return false;
}
BasicSubgroupLowering lower_basic_subgroups(Words& code,std::string& error) {
    error.clear();
    if (!has_basic_subgroups(code.data(),code.size())) return BasicSubgroupLowering::Unchanged;
    auto message=[&](spv_message_level_t,const char*,const spv_position_t&,const char* text) { error=text; };
    spvtools::SpirvTools validator(SPV_ENV_UNIVERSAL_1_6);
    validator.SetMessageConsumer(message);
    if (!validator.Validate(code)) return BasicSubgroupLowering::Invalid;
    try {
        // Vulkan Input pointers cannot be function arguments or participate in
        // Phi/Select. Retyping their AccessChain/CopyObject descendants suffices;
        // helpers may keep reading the same globals without being inlined.
        spvtools::Optimizer prepare(SPV_ENV_UNIVERSAL_1_6);
        prepare.SetMessageConsumer(message);
        prepare.RegisterPass(spvtools::CreateCompactIdsPass());
        prepare.RegisterPass(spvtools::CreateFlattenDecorationPass());
        prepare.RegisterPass(spvtools::CreateRemoveDuplicatesPass());
        Words source;
        if (!prepare.Run(code.data(),code.size(),&source)) return BasicSubgroupLowering::Invalid;
        std::vector<Instruction> instructions;
        auto context=spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
        const auto parsed=spvBinaryParse(context,&instructions,source.data(),source.size(),nullptr,parse,nullptr);
        spvContextDestroy(context);
        if (parsed!=SPV_SUCCESS) throw std::runtime_error("cannot parse subgroup module");
        Lowering lower{source[3]};
        std::map<uint32_t,uint32_t> builtins,private_variables;
        std::map<uint32_t,std::array<uint32_t,3>> local_sizes;
        std::set<uint32_t> compute_entries,private_pointers;
        std::set<uint32_t> noncompute_functions;
        std::map<uint32_t,std::set<uint32_t>> calls;
        uint32_t current_function=0;
        uint32_t native_index=0,workgroup_size=0,uint_type=0;
        for (const auto& inst:instructions) {
            const auto& w=inst.w;
            if (inst.result) lower.defs[inst.result]=inst;
            if (inst.op()==spv::OpDecorate && w.size()==4 && w[2]==spv::DecorationBuiltIn) builtins[w[1]]=w[3];
            if (inst.op()==spv::OpTypeInt && w[2]==32 && !w[3]) uint_type=w[1];
            if (inst.op()==spv::OpConstant && w.size()==4) lower.literals[{w[1],w[3]}]=w[2];
            if (inst.op()==spv::OpEntryPoint && w[1]==spv::ExecutionModelGLCompute) compute_entries.insert(w[2]);
            if (inst.op()==spv::OpEntryPoint && w[1]!=spv::ExecutionModelGLCompute) noncompute_functions.insert(w[2]);
            if (inst.op()==spv::OpFunction) current_function=inst.result;
            if (inst.op()==spv::OpFunctionCall) calls[current_function].insert(w[3]);
            if (inst.op()==spv::OpFunctionEnd) current_function=0;
            if (inst.op()==spv::OpExecutionMode && w[2]==spv::ExecutionModeLocalSize)
                local_sizes[w[1]]={w[3],w[4],w[5]};
        }
        std::vector<uint32_t> reachable(noncompute_functions.begin(),noncompute_functions.end());
        for (size_t i=0;i<reachable.size();++i) for (const auto callee:calls[reachable[i]])
            if (noncompute_functions.insert(callee).second) reachable.push_back(callee);
        bool needs_index=false,needs_count=false;
        for (const auto& builtin:builtins) {
            if (builtin.second==spv::BuiltInWorkgroupSize) workgroup_size=builtin.first;
            if (builtin.second==spv::BuiltInSubgroupId) needs_index=true;
            if (builtin.second==spv::BuiltInNumSubgroups) needs_count=true;
            if (subgroup_builtin(builtin.second)) {
                const auto& variable=lower.defs.at(builtin.first);
                if (variable.op()!=spv::OpVariable) throw std::runtime_error("subgroup built-in is not a variable");
                private_variables[builtin.first]=builtin.second;
                private_pointers.insert(builtin.first);
            }
        }
        if ((needs_index || needs_count) && !uint_type) {
            uint_type=lower.id(); emit(lower.declarations,spv::OpTypeInt,{uint_type,32,0});
        }
        if (needs_index) {
            // Existing LocalInvocationIndex variables may have signed types or
            // belong to different entry points. Keep their identities as private
            // copies and initialize them from one new native uint input.
            for (const auto& builtin:builtins) if (builtin.second==spv::BuiltInLocalInvocationIndex) {
                private_variables[builtin.first]=builtin.second;
                private_pointers.insert(builtin.first);
            }
            const auto pointer=lower.id(); native_index=lower.id();
            emit(lower.declarations,spv::OpTypePointer,{pointer,spv::StorageClassInput,uint_type});
            emit(lower.declarations,spv::OpVariable,{pointer,native_index,spv::StorageClassInput});
            emit(lower.annotations,spv::OpDecorate,{native_index,spv::DecorationBuiltIn,spv::BuiltInLocalInvocationIndex});
        }
        std::map<uint32_t,Words> prefixes;
        if (needs_index || needs_count) for (const auto entry:compute_entries) {
            auto& prefix=prefixes[entry];
            uint32_t index=0,count=0;
            if (needs_index) { index=lower.id(); emit(prefix,spv::OpLoad,{uint_type,index,native_index}); }
            if (needs_count) {
                std::array<uint32_t,3> dimensions{};
                if (workgroup_size) for (uint32_t i=0;i<3;++i) {
                    const auto component=lower.defs.at(lower.defs.at(workgroup_size).type).w[2];
                    const auto extracted=lower.id();
                    emit(prefix,spv::OpCompositeExtract,{component,extracted,workgroup_size,i});
                    dimensions[i]=extracted;
                    if (component!=uint_type) {
                        dimensions[i]=lower.id(); emit(prefix,spv::OpBitcast,{uint_type,dimensions[i],extracted});
                    }
                } else {
                    auto size=local_sizes.find(entry);
                    if (size==local_sizes.end()) throw std::runtime_error("compute workgroup size is not specified");
                    for (unsigned i=0;i<3;++i) dimensions[i]=lower.literal(uint_type,size->second[i]);
                }
                const auto xy=lower.id(); count=lower.id();
                emit(prefix,spv::OpIMul,{uint_type,xy,dimensions[0],dimensions[1]});
                emit(prefix,spv::OpIMul,{uint_type,count,xy,dimensions[2]});
            }
            for (const auto& variable:private_variables) {
                uint32_t value=0;
                if (variable.second==spv::BuiltInSubgroupId || variable.second==spv::BuiltInLocalInvocationIndex) value=index;
                if (variable.second==spv::BuiltInNumSubgroups) value=count;
                if (!value) continue;
                const auto type=lower.defs.at(lower.defs.at(variable.first).type).w[3];
                if (type!=uint_type) {
                    const auto converted=lower.id(); emit(prefix,spv::OpBitcast,{type,converted,value}); value=converted;
                }
                emit(prefix,spv::OpStore,{variable.first,value});
            }
        }
        // All original globals move after the new pointer/constant declarations.
        Words output(source.begin(),source.begin()+5),globals;
        size_t annotation_at=0,function_at=0;
        uint32_t function=0;
        std::map<size_t,Words> insertions;
        for (size_t n=0;n<instructions.size();++n) {
            auto inst=instructions[n]; auto& w=inst.w;
            if (inst.op()==spv::OpCapability && capability(w[1])) continue;
            if (inst.op()==spv::OpExtension && legacy_extension(w)) continue;
            if (inst.op()==spv::OpDecorate && private_variables.count(w[1])) {
                if (w[2]==spv::DecorationBuiltIn || w[2]==spv::DecorationFlat || w[2]==spv::DecorationNoPerspective ||
                    w[2]==spv::DecorationCentroid || w[2]==spv::DecorationSample || w[2]==spv::DecorationInvariant) continue;
            }
            if (!annotation_at && inst.op()>=spv::OpTypeVoid && inst.op()<=spv::OpTypeForwardPointer)
                annotation_at=output.size();
            if (inst.op()==spv::OpEntryPoint) {
                const char* name=reinterpret_cast<const char*>(w.data()+3);
                size_t begin=3+(std::strlen(name)+1+3)/4;
                Words interfaces;
                for (size_t i=begin;i<w.size();++i) {
                    if (source[1]<0x10400 && private_variables.count(w[i])) continue;
                    if (std::find(interfaces.begin(),interfaces.end(),w[i])==interfaces.end()) interfaces.push_back(w[i]);
                }
                if (needs_index && w[1]==spv::ExecutionModelGLCompute &&
                    std::find(interfaces.begin(),interfaces.end(),native_index)==interfaces.end()) interfaces.push_back(native_index);
                if (source[1]>=0x10400 && w[1]==spv::ExecutionModelGLCompute) for (const auto& variable:private_variables) {
                    if (variable.second!=spv::BuiltInSubgroupId && variable.second!=spv::BuiltInNumSubgroups &&
                        variable.second!=spv::BuiltInLocalInvocationIndex) continue;
                    if (std::find(interfaces.begin(),interfaces.end(),variable.first)==interfaces.end()) interfaces.push_back(variable.first);
                }
                w.resize(begin); append(w,interfaces); w[0]=(uint32_t(w.size())<<16)|spv::OpEntryPoint;
            }
            if (inst.op()==spv::OpVariable && !function) {
                if (auto variable=private_variables.find(inst.result);variable!=private_variables.end()) {
                    const auto type=lower.defs.at(inst.type).w[3];
                    w={uint32_t((5u<<16)|spv::OpVariable),lower.private_pointer(inst.type),inst.result,
                       spv::StorageClassPrivate,lower.initializer(type,variable->second)};
                }
                append(globals,w); continue;
            }
            if (inst.op()==spv::OpFunction) {
                if (!function_at) function_at=output.size();
                function=inst.result;
                auto prefix=prefixes.find(function);
                if (prefix!=prefixes.end()) {
                    size_t label=n+1;
                    while (label<instructions.size() && instructions[label].op()!=spv::OpLabel) ++label;
                    size_t after=label+1;
                    for (size_t i=label+1;i<instructions.size() && instructions[i].op()!=spv::OpLabel &&
                         instructions[i].op()!=spv::OpFunctionEnd;++i)
                        if (instructions[i].op()==spv::OpVariable) after=i+1;
                    insertions[after]=prefix->second;
                }
            }
            if (auto insertion=insertions.find(n);insertion!=insertions.end()) append(output,insertion->second);
            if ((inst.op()==spv::OpCopyObject && private_pointers.count(w[3])) ||
                ((inst.op()==spv::OpAccessChain || inst.op()==spv::OpInBoundsAccessChain) && private_pointers.count(w[3]))) {
                w[1]=lower.private_pointer(inst.type); private_pointers.insert(inst.result);
            }
            if (inst.op()==spv::OpGroupNonUniformElect || inst.op()==spv::OpSubgroupAllEqualKHR) {
                const auto value=lower.id(); emit(lower.declarations,spv::OpConstantTrue,{inst.type,value});
                emit(output,spv::OpCopyObject,{inst.type,inst.result,value}); continue;
            }
            if (inst.op()==spv::OpSubgroupAllKHR || inst.op()==spv::OpSubgroupAnyKHR ||
                inst.op()==spv::OpSubgroupFirstInvocationKHR || inst.op()==spv::OpSubgroupReadInvocationKHR) {
                emit(output,spv::OpCopyObject,{inst.type,inst.result,w[3]}); continue;
            }
            if (inst.op()==spv::OpSubgroupBallotKHR) {
                const auto scalar=lower.defs.at(inst.type).w[2], zero=lower.literal(scalar,0),one=lower.literal(scalar,1);
                const auto bit=lower.id(); emit(output,spv::OpSelect,{scalar,bit,w[3],one,zero});
                emit(output,spv::OpCompositeConstruct,{inst.type,inst.result,bit,zero,zero,zero}); continue;
            }
            auto scope=[&](uint32_t original) {
                const auto& value=lower.defs.at(original).w;
                if ((value[0]&0xffff)!=spv::OpConstant || value.size()!=4)
                    throw std::runtime_error("memory scope requires a scalar constant");
                // A wider memory scope preserves the single-invocation
                // subgroup's ordering without introducing an execution wait.
                // Compute uses Workgroup so VulkanMemoryModelDeviceScope is
                // not required; non-compute legacy subgroup extensions use
                // Device because Workgroup memory scope is illegal there.
                const auto wider=noncompute_functions.count(function)?spv::ScopeDevice:spv::ScopeWorkgroup;
                return value[3]==spv::ScopeSubgroup?lower.literal(value[1],wider):original;
            };
            if (inst.op()==spv::OpControlBarrier) {
                const auto& execution=lower.defs.at(w[1]).w;
                if ((execution[0]&0xffff)==spv::OpConstant && execution[3]==spv::ScopeSubgroup) {
                    const auto& semantics=lower.defs.at(w[3]).w;
                    if ((semantics[0]&0xffff)!=spv::OpConstant) throw std::runtime_error("barrier semantics require a constant");
                    if (semantics[3]) emit(output,spv::OpMemoryBarrier,{scope(w[2]),w[3]});
                    continue;
                }
                w[2]=scope(w[2]);
            } else if (inst.op()==spv::OpMemoryBarrier) w[1]=scope(w[1]);
            else if (!inst.scopes.empty() && (!std::strncmp(spvOpcodeString(inst.op()),"Atomic",6) ||
                inst.op()==spv::OpLoad || inst.op()==spv::OpStore || inst.op()==spv::OpCopyMemory ||
                inst.op()==spv::OpCopyMemorySized || inst.op()==spv::OpImageRead || inst.op()==spv::OpImageWrite ||
                inst.op()==spv::OpImageSparseRead))
                for (const auto operand:inst.scopes) w[operand]=scope(w[operand]);
            if ((inst.op()==spv::OpLoad && private_pointers.count(w[3])) ||
                (inst.op()==spv::OpCopyMemory && (private_pointers.count(w[1]) || private_pointers.count(w[2])))) {
                // Rewrite original scope offsets before deleting alignment
                // operands. Reverse order also handles separate source/target
                // memory-access masks in SPIR-V 1.4 modules.
                for (auto mask=inst.memory_masks.rbegin();mask!=inst.memory_masks.rend();++mask) weaken_alignment(w,*mask);
            }
            append(output,w);
            if (inst.op()==spv::OpFunctionEnd) function=0;
        }
        if (!function_at || !annotation_at) throw std::runtime_error("subgroup module has no executable declarations");
        append(lower.declarations,globals);
        output.insert(output.begin()+function_at,lower.declarations.begin(),lower.declarations.end());
        output.insert(output.begin()+annotation_at,lower.annotations.begin(),lower.annotations.end());
        output[3]=lower.next;
        if (!validator.Validate(output)) return BasicSubgroupLowering::Invalid;
        code=std::move(output);
        return BasicSubgroupLowering::Changed;
    } catch (const std::runtime_error& failure) {
        error=failure.what(); return BasicSubgroupLowering::Invalid;
    } catch (const std::out_of_range&) {
        error="unknown subgroup operand or type"; return BasicSubgroupLowering::Invalid;
    }
}
}

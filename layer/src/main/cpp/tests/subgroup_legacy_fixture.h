#pragma once
#include <string>
inline std::string subgroup_legacy_fixture(bool fragment=false) {
    std::string text=R"(OpCapability Shader
OpCapability SubgroupBallotKHR
OpCapability SubgroupVoteKHR
OpExtension "SPV_KHR_shader_ballot"
OpExtension "SPV_KHR_subgroup_vote"
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %size %lane %eq %ge %gt %le %lt
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %size BuiltIn SubgroupSize
OpDecorate %lane BuiltIn SubgroupLocalInvocationId
OpDecorate %eq BuiltIn SubgroupEqMaskKHR
OpDecorate %ge BuiltIn SubgroupGeMaskKHR
OpDecorate %gt BuiltIn SubgroupGtMaskKHR
OpDecorate %le BuiltIn SubgroupLeMaskKHR
OpDecorate %lt BuiltIn SubgroupLtMaskKHR
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%v4uint = OpTypeVector %uint 4
%ptr = OpTypePointer Input %uint
%ptr4 = OpTypePointer Input %v4uint
%zero = OpConstant %uint 0
%scope = OpConstant %uint 3
%semantics = OpConstant %uint 72
%true = OpConstantTrue %bool
%false = OpConstantFalse %bool
%fn = OpTypeFunction %void
%size = OpVariable %ptr Input
%lane = OpVariable %ptr Input
%eq = OpVariable %ptr4 Input
%ge = OpVariable %ptr4 Input
%gt = OpVariable %ptr4 Input
%le = OpVariable %ptr4 Input
%lt = OpVariable %ptr4 Input
%main = OpFunction %void None %fn
%entry = OpLabel
%s = OpLoad %uint %size
%l = OpLoad %uint %lane
%e = OpLoad %v4uint %eq
%g = OpLoad %v4uint %ge
%t = OpLoad %v4uint %gt
%a = OpLoad %v4uint %le
%b = OpLoad %v4uint %lt
%component = OpAccessChain %ptr %eq %zero
%firstBit = OpLoad %uint %component Volatile|Aligned|Nontemporal 4
%ballot = OpSubgroupBallotKHR %v4uint %true
%empty = OpSubgroupBallotKHR %v4uint %false
%first = OpSubgroupFirstInvocationKHR %uint %s
%read = OpSubgroupReadInvocationKHR %uint %s %zero
%all = OpSubgroupAllKHR %bool %true
%any = OpSubgroupAnyKHR %bool %false
%equal = OpSubgroupAllEqualKHR %bool %l
OpMemoryBarrier %scope %semantics
OpReturn
OpFunctionEnd
)";
    if (fragment) {
        const std::string entry="OpEntryPoint GLCompute";
        text.replace(text.find(entry),entry.size(),"OpEntryPoint Fragment");
        const std::string mode="OpExecutionMode %main LocalSize 1 1 1";
        text.replace(text.find(mode),mode.size(),"OpExecutionMode %main OriginUpperLeft");
        std::string flat;
        for (const char* name:{"size","lane","eq","ge","gt","le","lt"})
            flat+=std::string("OpDecorate %")+name+" Flat\n";
        text.insert(text.find("%void = OpTypeVoid"),flat);
    }
    return text;
}
inline std::string subgroup_atomic_fixture(const std::string& operation,bool floating=false,bool signed_type=false) {
    auto text=std::string("OpCapability Shader\nOpCapability GroupNonUniform\n")+
        (floating?"OpCapability AtomicFloat32AddEXT\nOpExtension \"SPV_EXT_shader_atomic_float_add\"\n":"")+R"(OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %Block Block
OpMemberDecorate %Block 0 Offset 0
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 0
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%scope = OpConstant %uint 3
%fOne = OpConstant %float 1
%Block = OpTypeStruct %uint
%ptrBlock = OpTypePointer StorageBuffer %Block
%ptrScalar = OpTypePointer StorageBuffer %uint
%fn = OpTypeFunction %void
%buffer = OpVariable %ptrBlock StorageBuffer
%main = OpFunction %void None %fn
%entry = OpLabel
%pointer = OpAccessChain %ptrScalar %buffer %zero
%elected = OpGroupNonUniformElect %bool %scope
OPERATION
OpReturn
OpFunctionEnd
)";
    if (floating) {
        for (const std::string old:{"%Block = OpTypeStruct %uint","%ptrScalar = OpTypePointer StorageBuffer %uint"}) {
            auto at=text.find(old);
            auto replacement=old; replacement.replace(replacement.rfind("%uint"),5,"%float");
            text.replace(at,old.size(),replacement);
        }
    }
    if (signed_type) {
        const std::string integer="%uint = OpTypeInt 32 0";
        text.replace(text.find(integer),integer.size(),"%uint = OpTypeInt 32 1");
    }
    text.replace(text.find("OPERATION"),9,operation);
    return text;
}

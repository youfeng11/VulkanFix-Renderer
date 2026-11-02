#pragma once
#include <sstream>
#include <string>

// Writes both global and workgroup coordinates. Non-unit sizes, specialized
// sizes, helper calls, Phi, dynamic component loads and CopyMemory are distinct
// legal consumers that a blanket constant offset cannot implement correctly.
inline std::string dispatch_base_fixture(bool specialized = false, bool helper = false,
                                         bool copy_memory = false) {
    std::ostringstream s;
    s << R"(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "kernel" %gid %wg %lid
)";
    s << "OpExecutionMode %main LocalSize " << (specialized ? 1 : 8) << " 2 1\n";
    s << R"(OpDecorate %gid BuiltIn GlobalInvocationId
OpDecorate %wg BuiltIn WorkgroupId
OpDecorate %lid BuiltIn LocalInvocationId
OpDecorate %array ArrayStride 4
OpDecorate %Block Block
OpMemberDecorate %Block 0 Offset 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 0
)";
    if (specialized) s << "OpDecorate %sx SpecId 0\nOpDecorate %size BuiltIn WorkgroupSize\n";
    s << R"(%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%bool = OpTypeBool
%v3 = OpTypeVector %uint 3
%in3 = OpTypePointer Input %v3
%in1 = OpTypePointer Input %uint
%local3 = OpTypePointer Function %v3
%helperfn = OpTypeFunction %v3
%array = OpTypeRuntimeArray %uint
%Block = OpTypeStruct %array
%outBlock = OpTypePointer StorageBuffer %Block
%out1 = OpTypePointer StorageBuffer %uint
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%four = OpConstant %uint 4
%eight = OpConstant %uint 8
%sixtyfour = OpConstant %uint 64
%plane = OpConstant %uint 2048
%byte = OpConstant %uint 256
%half = OpConstant %uint 65536
)";
    if (specialized) s << "%sx = OpSpecConstant %uint 4\n%size = OpSpecConstantComposite %v3 %sx %two %one\n";
    s << R"(%gid = OpVariable %in3 Input
%wg = OpVariable %in3 Input
%lid = OpVariable %in3 Input
%output = OpVariable %outBlock StorageBuffer
%main = OpFunction %void None %fn
%entry = OpLabel
)";
    if (copy_memory) s << "%temporary = OpVariable %local3 Function\nOpCopyMemory %temporary %gid\n%raw = OpLoad %v3 %temporary\n";
    else if (helper) s << "%raw = OpFunctionCall %v3 %reader\n";
    else s << "%raw = OpLoad %v3 %gid\n";
    s << R"(%l = OpLoad %v3 %lid
%lx = OpCompositeExtract %uint %l 0
%condition = OpULessThan %bool %lx %four
OpSelectionMerge %merge None
OpBranchConditional %condition %then %else
%then = OpLabel
OpBranch %merge
%else = OpLabel
OpBranch %merge
%merge = OpLabel
%g = OpPhi %v3 %raw %then %raw %else
%x = OpCompositeExtract %uint %g 0
%y = OpCompositeExtract %uint %g 1
%z = OpCompositeExtract %uint %g 2
%row = OpIMul %uint %y %sixtyfour
%slice = OpIMul %uint %z %plane
%xy = OpIAdd %uint %x %row
%index = OpIAdd %uint %xy %slice
%w = OpLoad %v3 %wg
%wx = OpCompositeExtract %uint %w 0
%wy = OpCompositeExtract %uint %w 1
%wz = OpCompositeExtract %uint %w 2
%wxpart = OpIMul %uint %wx %half
%wypart = OpIMul %uint %wy %byte
%wxy = OpIAdd %uint %wxpart %wypart
%wxyz = OpIAdd %uint %wxy %wz
%component = OpUMod %uint %lx %three
%pointer = OpAccessChain %in1 %gid %component
%selected = OpLoad %uint %pointer
%value = OpIAdd %uint %wxyz %selected
%destination = OpAccessChain %out1 %output %zero %index
OpStore %destination %value
OpReturn
OpFunctionEnd
)";
    if (helper) s << R"(%reader = OpFunction %v3 None %helperfn
%helperentry = OpLabel
%read = OpLoad %v3 %gid
OpReturnValue %read
OpFunctionEnd
)";
    return s.str();
}

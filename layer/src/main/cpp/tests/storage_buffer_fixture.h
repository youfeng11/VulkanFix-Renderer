#pragma once
#include <string>
inline const char* storage_probe_vertex() {
    return R"(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %vertexIndex %position
OpDecorate %vertexIndex BuiltIn VertexIndex
OpDecorate %position BuiltIn Position
%void = OpTypeVoid
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%fn = OpTypeFunction %void
%ptrIndex = OpTypePointer Input %int
%ptrPosition = OpTypePointer Output %v4
%minus = OpConstant %float -1
%three = OpConstant %float 3
%zero = OpConstant %float 0
%one = OpConstant %float 1
%p0 = OpConstantComposite %v4 %minus %minus %zero %one
%p1 = OpConstantComposite %v4 %three %minus %zero %one
%p2 = OpConstantComposite %v4 %minus %three %zero %one
%vertexIndex = OpVariable %ptrIndex Input
%position = OpVariable %ptrPosition Output
%main = OpFunction %void None %fn
%entry = OpLabel
%index = OpLoad %int %vertexIndex
OpSelectionMerge %merge None
OpSwitch %index %a 1 %b 2 %c
%a = OpLabel
OpBranch %merge
%b = OpLabel
OpBranch %merge
%c = OpLabel
OpBranch %merge
%merge = OpLabel
%value = OpPhi %v4 %p0 %a %p1 %b %p2 %c
OpStore %position %value
OpReturn
OpFunctionEnd
)";
}
inline std::string storage_buffer_fixture(bool grouped = false, bool extension = false, bool specialized = false,
                                          bool memory_access = false) {
    std::string code = std::string("OpCapability Shader\n") +
        (specialized ? "OpCapability StorageBufferArrayDynamicIndexing\n" : "") +
        (extension ? "OpExtension \"SPV_KHR_storage_buffer_storage_class\"\n" : "") + R"(
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
)" + (extension ? "" : "OpModuleProcessed \"storage-buffer-equivalence-probe\"\n") + (grouped ? R"(
%dec = OpDecorationGroup
OpDecorate %dec Block
OpGroupDecorate %dec %Block
)" : "OpDecorate %Block Block\n") + R"(
OpMemberDecorate %Block 0 Offset 0
OpMemberDecorate %Block 1 Offset 4
OpMemberDecorate %Block 2 Offset 8
OpMemberDecorate %Block 3 Offset 12
OpDecorate %ubo DescriptorSet 0
OpDecorate %ubo Binding 0
OpDecorate %a DescriptorSet 0
OpDecorate %a Binding 1
OpDecorate %b DescriptorSet 0
OpDecorate %b Binding 2
OpDecorate %array DescriptorSet 0
OpDecorate %array Binding 3
OpDecorate %a Aliased
OpDecorate %b Aliased
OpDecorate %array Aliased
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%Block = OpTypeStruct %uint %uint %uint %uint
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%device = OpConstant %uint 1
%ptrUBlock = OpTypePointer Uniform %Block
%ptrU = OpTypePointer Uniform %uint
%ptrSBlock = OpTypePointer StorageBuffer %Block
%ptrS = OpTypePointer StorageBuffer %uint
%Array = OpTypeArray %Block %two
%ptrArray = OpTypePointer StorageBuffer %Array
%fn = OpTypeFunction %void
%helperFn = OpTypeFunction %void %Block
%ubo = OpVariable %ptrUBlock Uniform
%a = OpVariable %ptrSBlock StorageBuffer
%b = OpVariable %ptrSBlock StorageBuffer
%array = OpVariable %ptrArray StorageBuffer
%helper = OpFunction %void None %helperFn
%value = OpFunctionParameter %Block
%helperLabel = OpLabel
OpStore %a %value
OpReturn
OpFunctionEnd
%main = OpFunction %void None %fn
%label = OpLabel
%u = OpLoad %Block %ubo
%oldB = OpLoad %Block %b
%call = OpFunctionCall %void %helper %u
OpCopyMemory %b %ubo Volatile|Aligned|Nontemporal 16
%arrayA = OpAccessChain %ptrSBlock %array %zero
%arrayB = OpAccessChain %ptrSBlock %array %one
%fieldA = OpAccessChain %ptrS %arrayA %two
%oldB3 = OpCompositeExtract %uint %oldB 3
%old = OpAtomicIAdd %uint %fieldA %device %zero %oldB3
%uField1 = OpAccessChain %ptrU %ubo %one
%u1 = OpLoad %uint %uField1
%double = OpIAdd %uint %u1 %u1
%fieldB = OpAccessChain %ptrS %arrayB %one
OpStore %fieldB %double
OpReturn
OpFunctionEnd
)";
    if (specialized) {
        auto at=code.find("OpMemberDecorate %Block 0");
        code.insert(at,"OpDecorate %arrayLength SpecId 7\n");
        const std::string array="%Array = OpTypeArray %Block %two";
        at=code.find(array);
        code.replace(at,array.size(),"%arrayLength = OpSpecConstant %uint 2\n%Array = OpTypeArray %Block %arrayLength");
        at=code.find("%arrayA = OpAccessChain");
        code.insert(at,R"(%whole = OpLoad %Array %array
%lastIndex = OpISub %uint %arrayLength %one
%lastBlock = OpAccessChain %ptrSBlock %array %lastIndex
%lastField = OpAccessChain %ptrS %lastBlock %zero
OpStore %lastField %zero
OpStore %array %whole
)");
        if (memory_access) {
            const std::string load="%whole = OpLoad %Array %array";
            at=code.find(load); code.insert(at+load.size()," Volatile|Aligned|Nontemporal 16");
            const std::string store="OpStore %array %whole";
            at=code.find(store); code.insert(at+store.size()," Volatile|Aligned|Nontemporal 16");
        }
    }
    return code;
}

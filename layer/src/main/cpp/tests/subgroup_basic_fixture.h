#pragma once
#include <string>
#include <map>
#include <sstream>
#include <cctype>
inline std::string subgroup_basic_fixture(bool grouped=false,bool existing_index=false,
                                         bool specialized=false,bool divergent=false,bool signed_types=false) {
    std::string text=R"(OpCapability Shader
OpCapability GroupNonUniform
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %sgSize %sgLane %sgId %sgCount %localId %groupId
OpExecutionMode %main LocalSize 2 3 2
OpDecorate %sgSize BuiltIn SubgroupSize
OpDecorate %sgLane BuiltIn SubgroupLocalInvocationId
OpDecorate %sgId BuiltIn SubgroupId
OpDecorate %sgCount BuiltIn NumSubgroups
OpDecorate %localId BuiltIn LocalInvocationId
OpDecorate %groupId BuiltIn WorkgroupId
OpDecorate %Words ArrayStride 4
OpDecorate %Buffer Block
OpMemberDecorate %Buffer 0 Offset 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 0
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%v3uint = OpTypeVector %uint 3
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%four = OpConstant %uint 4
%five = OpConstant %uint 5
%six = OpConstant %uint 6
%seven = OpConstant %uint 7
%eight = OpConstant %uint 8
%twelve = OpConstant %uint 12
%fortyTwo = OpConstant %uint 42
%hundred = OpConstant %uint 100
%subgroup = OpConstant %uint 3
%wgSemantics = OpConstant %uint 264
%bufferSemantics = OpConstant %uint 72
%ptrInput = OpTypePointer Input %uint
%ptrInput3 = OpTypePointer Input %v3uint
%Words = OpTypeRuntimeArray %uint
%Buffer = OpTypeStruct %Words
%ptrBuffer = OpTypePointer StorageBuffer %Buffer
%ptrOutput = OpTypePointer StorageBuffer %uint
%Shared = OpTypeArray %uint %twelve
%ptrShared = OpTypePointer Workgroup %Shared
%ptrSharedUint = OpTypePointer Workgroup %uint
%fn = OpTypeFunction %void
%helperFn = OpTypeFunction %uint
%sgSize = OpVariable %ptrInput Input
%sgLane = OpVariable %ptrInput Input
%sgId = OpVariable %ptrInput Input
%sgCount = OpVariable %ptrInput Input
%localId = OpVariable %ptrInput3 Input
%groupId = OpVariable %ptrInput3 Input
%output = OpVariable %ptrBuffer StorageBuffer
%shared = OpVariable %ptrShared Workgroup
%helper = OpFunction %uint DontInline %helperFn
%helperEntry = OpLabel
%loaded = OpLoad %uint %sgSize
OpReturnValue %loaded
OpFunctionEnd
%main = OpFunction %void None %fn
%entry = OpLabel
%localVector = OpLoad %v3uint %localId
%x = OpCompositeExtract %uint %localVector 0
%y = OpCompositeExtract %uint %localVector 1
%z = OpCompositeExtract %uint %localVector 2
%yStride = OpIMul %uint %y %two
%zStride = OpIMul %uint %z %six
%xy = OpIAdd %uint %x %yStride
%local = OpIAdd %uint %xy %zStride
%groupVector = OpLoad %v3uint %groupId
%groupX = OpCompositeExtract %uint %groupVector 0
%groupBase = OpIMul %uint %groupX %twelve
%global = OpIAdd %uint %groupBase %local
%base = OpIMul %uint %global %eight
%copied = OpCopyObject %ptrInput %sgSize
%copyRead = OpLoad %uint %copied
%helperRead = OpFunctionCall %uint %helper
%size = OpBitwiseOr %uint %copyRead %helperRead
%lane = OpLoad %uint %sgLane
%id = OpLoad %uint %sgId
%count = OpLoad %uint %sgCount
%elect = OpGroupNonUniformElect %bool %subgroup
%elected = OpSelect %uint %elect %one %zero
%slot = OpAccessChain %ptrSharedUint %shared %local
%ownValue = OpIAdd %uint %local %fortyTwo
OpStore %slot %ownValue
OpControlBarrier %subgroup %subgroup %wgSemantics
OpMemoryBarrier %subgroup %bufferSemantics
%ownRead = OpLoad %uint %slot
%field0 = OpAccessChain %ptrOutput %output %zero %base
OpStore %field0 %size
%i1 = OpIAdd %uint %base %one
%field1 = OpAccessChain %ptrOutput %output %zero %i1
OpStore %field1 %lane
%i2 = OpIAdd %uint %base %two
%field2 = OpAccessChain %ptrOutput %output %zero %i2
OpStore %field2 %id
%i3 = OpIAdd %uint %base %three
%field3 = OpAccessChain %ptrOutput %output %zero %i3
OpStore %field3 %count
%i4 = OpIAdd %uint %base %four
%field4 = OpAccessChain %ptrOutput %output %zero %i4
OpStore %field4 %elected
%i6 = OpIAdd %uint %base %six
%field6 = OpAccessChain %ptrOutput %output %zero %i6
%before = OpAtomicIAdd %uint %field6 %subgroup %zero %hundred
%i5 = OpIAdd %uint %base %five
%field5 = OpAccessChain %ptrOutput %output %zero %i5
OpStore %field5 %before
%i7 = OpIAdd %uint %base %seven
%field7 = OpAccessChain %ptrOutput %output %zero %i7
OpStore %field7 %ownRead
OpReturn
OpFunctionEnd
)";
    if (grouped) {
        const std::string decoration="OpDecorate %Buffer Block";
        text.replace(text.find(decoration),decoration.size(),
            "%blockGroup = OpDecorationGroup\nOpDecorate %blockGroup Block\nOpGroupDecorate %blockGroup %Buffer");
    }
    if (existing_index) {
        const std::string entry="%localId %groupId\n";
        text.insert(text.find(entry)+entry.size()-1," %existingIndex");
        text.insert(text.find("OpDecorate %Words"),"OpDecorate %existingIndex BuiltIn LocalInvocationIndex\n");
        text.insert(text.find("%output = OpVariable"),"%existingIndex = OpVariable %ptrInput Input\n");
        const std::string local="%local = OpIAdd %uint %xy %zStride";
        text.replace(text.find(local),local.size(),"%local = OpLoad %uint %existingIndex");
    }
    if (specialized) {
        const std::string mode="OpExecutionMode %main LocalSize 2 3 2";
        text.replace(text.find(mode),mode.size(),"OpExecutionMode %main LocalSize 1 1 1");
        text.insert(text.find("OpDecorate %Words"),R"(OpDecorate %workgroupSize BuiltIn WorkgroupSize
OpDecorate %sx SpecId 4
OpDecorate %sy SpecId 5
OpDecorate %sz SpecId 6
)");
        text.insert(text.find("%ptrInput = OpTypePointer"),R"(%sx = OpSpecConstant %uint 1
%sy = OpSpecConstant %uint 1
%sz = OpSpecConstant %uint 1
%workgroupSize = OpSpecConstantComposite %v3uint %sx %sy %sz
)");
    }
    if (divergent) {
        const std::string barrier="OpControlBarrier %subgroup %subgroup %wgSemantics";
        text.replace(text.find(barrier),barrier.size(),R"(%parity = OpBitwiseAnd %uint %local %one
%even = OpIEqual %bool %parity %zero
OpSelectionMerge %afterBarrier None
OpBranchConditional %even %withBarrier %afterBarrier
%withBarrier = OpLabel
OpControlBarrier %subgroup %subgroup %wgSemantics
OpBranch %afterBarrier
%afterBarrier = OpLabel)");
    }
    if (signed_types) {
        const std::string integer="%uint = OpTypeInt 32 0";
        text.replace(text.find(integer),integer.size(),"%uint = OpTypeInt 32 1");
    }
    return text;
}

// A shared module with two compute entry points and different local sizes.
// The second entry writes three records at the same per-workgroup stride 12.
inline std::string subgroup_basic_multi_entry(bool signed_types=false) {
    auto text=subgroup_basic_fixture(true,true,false,false,signed_types);
    const auto begin=text.find("%main = OpFunction");
    auto body=text.substr(begin);
    std::map<std::string,std::string> renamed;
    std::istringstream lines(body); std::string line;
    while (std::getline(lines,line)) if (!line.empty() && line[0]=='%') {
        const auto end=line.find(" =");
        if (end!=std::string::npos) {
            auto name=line.substr(0,end);
            renamed[name]=name=="%main"?"%second":name+"_second";
        }
    }
    std::string copy;
    for (size_t i=0;i<body.size();) {
        if (body[i]!='%') { copy+=body[i++]; continue; }
        size_t end=i+1;
        while (end<body.size() && (std::isalnum(static_cast<unsigned char>(body[end])) || body[end]=='_')) ++end;
        const auto id=body.substr(i,end-i);
        const auto found=renamed.find(id);
        copy+=found==renamed.end()?id:found->second; i=end;
    }
    text+=copy;
    const auto entry=text.find("OpEntryPoint GLCompute %main"), end=text.find('\n',entry);
    auto second=text.substr(entry,end-entry);
    second.replace(second.find("%main"),5,"%second");
    second.replace(second.find("\"main\""),6,"\"second\"");
    text.insert(end+1,second+"\n");
    const auto mode=text.find("OpExecutionMode %main LocalSize 2 3 2");
    text.insert(text.find('\n',mode)+1,"OpExecutionMode %second LocalSize 3 1 1\n");
    return text;
}

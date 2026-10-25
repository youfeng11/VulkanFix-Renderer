#pragma once
static const char *vertex_store_source = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %vertex %instance %position %outcolor %inpos %incolor
OpDecorate %vertex BuiltIn VertexIndex
OpDecorate %instance BuiltIn InstanceIndex
OpDecorate %position BuiltIn Position
OpDecorate %outcolor Location 0
OpDecorate %inpos Location 0
OpDecorate %incolor Location 1
OpDecorate %data DescriptorSet 0
OpDecorate %data Binding 0
OpDecorate %image DescriptorSet 0
OpDecorate %image Binding 1
OpDecorate %image NonReadable
OpDecorate %Block BufferBlock
OpMemberDecorate %Block 0 Offset 0
OpMemberDecorate %Block 1 Offset 4
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%bool = OpTypeBool
%b4 = OpTypeVector %bool 4
%v2 = OpTypeVector %float 2
%v4 = OpTypeVector %float 4
%u4 = OpTypeVector %uint 4
%i2 = OpTypeVector %int 2
%Block = OpTypeStruct %uint %uint
%Image = OpTypeImage %uint 2D 0 0 0 2 R32ui
%ptrimage = OpTypePointer UniformConstant %Image
%ptrblock = OpTypePointer Uniform %Block
%ptruint = OpTypePointer Uniform %uint
%ptrin = OpTypePointer Input %int
%ptrin2 = OpTypePointer Input %v2
%ptrin4 = OpTypePointer Input %v4
%ptrout = OpTypePointer Output %v4
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%hundred = OpConstant %uint 100
%magic = OpConstant %uint 123
%ione = OpConstant %int 1
%itwo = OpConstant %int 2
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%red = OpConstantComposite %v4 %fone %fzero %fzero %fone
%data = OpVariable %ptrblock Uniform
%image = OpVariable %ptrimage UniformConstant
%vertex = OpVariable %ptrin Input
%instance = OpVariable %ptrin Input
%inpos = OpVariable %ptrin2 Input
%incolor = OpVariable %ptrin4 Input
%position = OpVariable %ptrout Output
%outcolor = OpVariable %ptrout Output
%main = OpFunction %void None %fn
%label = OpLabel
%counter = OpAccessChain %ptruint %data %zero
%old0 = OpAtomicIAdd %uint %counter %one %zero %one
%constantcounter = OpAccessChain %ptruint %data %one
%old = OpAtomicIAdd %uint %constantcounter %one %zero %zero
%ok = OpIEqual %bool %old %magic
%color = OpLoad %v4 %incolor
%ok4 = OpCompositeConstruct %b4 %ok %ok %ok %ok
%finalcolor = OpSelect %v4 %ok4 %color %red
OpStore %outcolor %finalcolor
%pos = OpLoad %v2 %inpos
%x = OpCompositeExtract %float %pos 0
%y = OpCompositeExtract %float %pos 1
%clip = OpCompositeConstruct %v4 %x %y %fzero %fone
OpStore %position %clip
%v = OpLoad %int %vertex
%i = OpLoad %int %instance
%cx = OpISub %int %v %ione
%cy = OpISub %int %i %itwo
%coord = OpCompositeConstruct %i2 %cx %cy
%vu = OpBitcast %uint %v
%iu = OpBitcast %uint %i
%scaled = OpIMul %uint %iu %hundred
%value = OpIAdd %uint %vu %scaled
%texel = OpCompositeConstruct %u4 %value %zero %zero %one
%img = OpLoad %Image %image
OpImageWrite %img %coord %texel
OpReturn
OpFunctionEnd
)";
static const char *vertex_store_fragment = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %color %output
OpExecutionMode %main OriginUpperLeft
OpDecorate %color Location 0
OpDecorate %output Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%ptrin = OpTypePointer Input %v4
%ptrout = OpTypePointer Output %v4
%color = OpVariable %ptrin Input
%output = OpVariable %ptrout Output
%main = OpFunction %void None %fn
%label = OpLabel
%value = OpLoad %v4 %color
OpStore %output %value
OpReturn
OpFunctionEnd
)";

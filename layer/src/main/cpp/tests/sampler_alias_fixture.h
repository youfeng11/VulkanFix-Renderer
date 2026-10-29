#pragma once

// Independent compute fixture: mix regular and comparison samplers at one binding.
// All 128 descriptors are initialized by the GPU harness.
inline constexpr const char* kAliasedSamplerShader = R"spv(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %samplersA DescriptorSet 0
OpDecorate %samplersA Binding 0
OpDecorate %samplersB DescriptorSet 0
OpDecorate %samplersB Binding 0
OpDecorate %images DescriptorSet 0
OpDecorate %images Binding 1
OpDecorate %Out Block
OpMemberDecorate %Out 0 Offset 0
OpDecorate %out DescriptorSet 0
OpDecorate %out Binding 2
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%v2 = OpTypeVector %float 2
%v4 = OpTypeVector %float 4
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%count = OpConstant %uint 128
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%half = OpConstant %float 0.5
%uv = OpConstantComposite %v2 %half %half
%Sampler = OpTypeSampler
%Samplers = OpTypeArray %Sampler %count
%pSamplers = OpTypePointer UniformConstant %Samplers
%pSampler = OpTypePointer UniformConstant %Sampler
%samplersA = OpVariable %pSamplers UniformConstant
%samplersB = OpVariable %pSamplers UniformConstant
%Image = OpTypeImage %float 2D 0 0 0 1 Unknown
%ShadowImage = OpTypeImage %float 2D 1 0 0 1 Unknown
%Sampled = OpTypeSampledImage %Image
%SampledShadow = OpTypeSampledImage %ShadowImage
%Images = OpTypeArray %Image %two
%pImages = OpTypePointer UniformConstant %Images
%pImage = OpTypePointer UniformConstant %Image
%images = OpVariable %pImages UniformConstant
%Out = OpTypeStruct %v4
%pOut = OpTypePointer StorageBuffer %Out
%pV4 = OpTypePointer StorageBuffer %v4
%out = OpVariable %pOut StorageBuffer
%main = OpFunction %void None %fn
%entry = OpLabel
%ip0 = OpAccessChain %pImage %images %zero
%im0 = OpLoad %Image %ip0
%sp0 = OpAccessChain %pSampler %samplersB %zero
%s0 = OpLoad %Sampler %sp0
%si0 = OpSampledImage %Sampled %im0 %s0
%c0 = OpImageSampleExplicitLod %v4 %si0 %uv Lod %fzero
%a = OpCompositeExtract %float %c0 0
%ip1 = OpAccessChain %pImage %images %one
%im1 = OpLoad %Image %ip1
%sp2 = OpAccessChain %pSampler %samplersA %two
%s2 = OpLoad %Sampler %sp2
%siShadow = OpSampledImage %SampledShadow %im1 %s2
%d = OpImageSampleDrefExplicitLod %float %siShadow %uv %half Lod %fzero
%sp1 = OpAccessChain %pSampler %samplersB %one
%s1 = OpLoad %Sampler %sp1
%si1 = OpSampledImage %Sampled %im1 %s1
%c1 = OpImageSampleExplicitLod %v4 %si1 %uv Lod %fzero
%b = OpCompositeExtract %float %c1 0
%sum = OpFAdd %float %a %b
%mix = OpFMul %float %sum %half
%result = OpCompositeConstruct %v4 %a %d %b %mix
%dest = OpAccessChain %pV4 %out %zero
OpStore %dest %result
OpReturn
OpFunctionEnd
)spv";
inline constexpr const char* kCanonicalSamplerShader = R"spv(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %samplersA DescriptorSet 0
OpDecorate %samplersA Binding 0
OpDecorate %images DescriptorSet 0
OpDecorate %images Binding 1
OpDecorate %Out Block
OpMemberDecorate %Out 0 Offset 0
OpDecorate %out DescriptorSet 0
OpDecorate %out Binding 2
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%v2 = OpTypeVector %float 2
%v4 = OpTypeVector %float 4
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%count = OpConstant %uint 128
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%half = OpConstant %float 0.5
%uv = OpConstantComposite %v2 %half %half
%Sampler = OpTypeSampler
%Samplers = OpTypeArray %Sampler %count
%pSamplers = OpTypePointer UniformConstant %Samplers
%pSampler = OpTypePointer UniformConstant %Sampler
%samplersA = OpVariable %pSamplers UniformConstant
%Image = OpTypeImage %float 2D 0 0 0 1 Unknown
%ShadowImage = OpTypeImage %float 2D 1 0 0 1 Unknown
%Sampled = OpTypeSampledImage %Image
%SampledShadow = OpTypeSampledImage %ShadowImage
%Images = OpTypeArray %Image %two
%pImages = OpTypePointer UniformConstant %Images
%pImage = OpTypePointer UniformConstant %Image
%images = OpVariable %pImages UniformConstant
%Out = OpTypeStruct %v4
%pOut = OpTypePointer StorageBuffer %Out
%pV4 = OpTypePointer StorageBuffer %v4
%out = OpVariable %pOut StorageBuffer
%main = OpFunction %void None %fn
%entry = OpLabel
%ip0 = OpAccessChain %pImage %images %zero
%im0 = OpLoad %Image %ip0
%sp0 = OpAccessChain %pSampler %samplersA %zero
%s0 = OpLoad %Sampler %sp0
%si0 = OpSampledImage %Sampled %im0 %s0
%c0 = OpImageSampleExplicitLod %v4 %si0 %uv Lod %fzero
%a = OpCompositeExtract %float %c0 0
%ip1 = OpAccessChain %pImage %images %one
%im1 = OpLoad %Image %ip1
%sp2 = OpAccessChain %pSampler %samplersA %two
%s2 = OpLoad %Sampler %sp2
%siShadow = OpSampledImage %SampledShadow %im1 %s2
%d = OpImageSampleDrefExplicitLod %float %siShadow %uv %half Lod %fzero
%sp1 = OpAccessChain %pSampler %samplersA %one
%s1 = OpLoad %Sampler %sp1
%si1 = OpSampledImage %Sampled %im1 %s1
%c1 = OpImageSampleExplicitLod %v4 %si1 %uv Lod %fzero
%b = OpCompositeExtract %float %c1 0
%sum = OpFAdd %float %a %b
%mix = OpFMul %float %sum %half
%result = OpCompositeConstruct %v4 %a %d %b %mix
%dest = OpAccessChain %pV4 %out %zero
OpStore %dest %result
OpReturn
OpFunctionEnd
)spv";

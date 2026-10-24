#ifndef INDIRECT_COUNT_SHADER_H
#define INDIRECT_COUNT_SHADER_H

// Compute shader used to expand vkCmdDrawIndirectCount on drivers without
// VK_KHR_draw_indirect_count.
//
// Inputs:  src = the application's VkDrawIndirectCommand array (4 u32 per draw)
//          cnt = the count buffer (one u32)
//          dst = the layer's scratch buffer, later used with vkCmdDrawIndirect
// Push:    srcOffset/cntOffset/dstOffset in words, maxCount.
//
// For i < maxCount: copy src[i] when i < min(count, maxCount), otherwise write a
// zeroed command (vertexCount = instanceCount = 0), so vkCmdDrawIndirect can be
// issued with drawCount = maxCount.
//
// This is SPIR-V 1.0 assembly (BufferBlock + Uniform storage) so it validates
// for SPV_ENV_VULKAN_1_0 and runs on a Vulkan 1.0 driver.
static constexpr const char* kIndirectCountDrawShader = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gid
               OpExecutionMode %main LocalSize 64 1 1
               OpDecorate %gid BuiltIn GlobalInvocationId
               OpDecorate %src DescriptorSet 0
               OpDecorate %src Binding 0
               OpDecorate %dst DescriptorSet 0
               OpDecorate %dst Binding 1
               OpDecorate %cnt DescriptorSet 0
               OpDecorate %cnt Binding 2
               OpDecorate %src_struct BufferBlock
               OpMemberDecorate %src_struct 0 Offset 0
               OpDecorate %dst_struct BufferBlock
               OpMemberDecorate %dst_struct 0 Offset 0
               OpDecorate %cnt_struct BufferBlock
               OpMemberDecorate %cnt_struct 0 Offset 0
               OpDecorate %rt ArrayStride 4
               OpDecorate %pc_block Block
               OpMemberDecorate %pc_block 0 Offset 0
               OpMemberDecorate %pc_block 1 Offset 4
               OpMemberDecorate %pc_block 2 Offset 8
               OpMemberDecorate %pc_block 3 Offset 12
               OpMemberDecorate %pc_block 4 Offset 16
       %void = OpTypeVoid
       %func = OpTypeFunction %void
       %uint = OpTypeInt 32 0
       %bool = OpTypeBool
        %v3u = OpTypeVector %uint 3
   %ptr_in_v3 = OpTypePointer Input %v3u
        %gid = OpVariable %ptr_in_v3 Input
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
     %uint_4 = OpConstant %uint 4
         %rt = OpTypeRuntimeArray %uint
  %src_struct = OpTypeStruct %rt
  %dst_struct = OpTypeStruct %rt
  %cnt_struct = OpTypeStruct %rt
 %ptr_src_struct = OpTypePointer Uniform %src_struct
 %ptr_dst_struct = OpTypePointer Uniform %dst_struct
 %ptr_cnt_struct = OpTypePointer Uniform %cnt_struct
        %src = OpVariable %ptr_src_struct Uniform
        %dst = OpVariable %ptr_dst_struct Uniform
        %cnt = OpVariable %ptr_cnt_struct Uniform
   %ptr_uint = OpTypePointer Uniform %uint
   %pc_block = OpTypeStruct %uint %uint %uint %uint %uint
%ptr_pc_block = OpTypePointer PushConstant %pc_block
         %pc = OpVariable %ptr_pc_block PushConstant
%ptr_pc_uint = OpTypePointer PushConstant %uint
       %main = OpFunction %void None %func
      %entry = OpLabel
      %gid_v = OpLoad %v3u %gid
        %idx = OpCompositeExtract %uint %gid_v 0
     %pc_src = OpAccessChain %ptr_pc_uint %pc %uint_0
    %src_off = OpLoad %uint %pc_src
     %pc_cnt = OpAccessChain %ptr_pc_uint %pc %uint_1
    %cnt_off = OpLoad %uint %pc_cnt
     %pc_dst = OpAccessChain %ptr_pc_uint %pc %uint_2
    %dst_off = OpLoad %uint %pc_dst
     %pc_max = OpAccessChain %ptr_pc_uint %pc %uint_3
      %max_v = OpLoad %uint %pc_max
   %pc_stride = OpAccessChain %ptr_pc_uint %pc %uint_4
  %stride_v = OpLoad %uint %pc_stride
        %ge = OpUGreaterThanEqual %bool %idx %max_v
               OpSelectionMerge %merge None
               OpBranchConditional %ge %merge %body
       %body = OpLabel
    %cnt_idx = OpAccessChain %ptr_uint %cnt %uint_0 %cnt_off
      %cnt_v = OpLoad %uint %cnt_idx
     %is_lt = OpULessThan %bool %cnt_v %max_v
      %count = OpSelect %uint %is_lt %cnt_v %max_v
         %i4 = OpIMul %uint %idx %stride_v
     %s_base = OpIAdd %uint %src_off %i4
     %d_base = OpIAdd %uint %dst_off %i4
       %keep = OpULessThan %bool %idx %count
         %s0 = OpAccessChain %ptr_uint %src %uint_0 %s_base
         %v0 = OpLoad %uint %s0
         %r0 = OpSelect %uint %keep %v0 %uint_0
         %d0 = OpAccessChain %ptr_uint %dst %uint_0 %d_base
               OpStore %d0 %r0
       %s1_i = OpIAdd %uint %s_base %uint_1
         %s1 = OpAccessChain %ptr_uint %src %uint_0 %s1_i
         %v1 = OpLoad %uint %s1
         %r1 = OpSelect %uint %keep %v1 %uint_0
       %d1_i = OpIAdd %uint %d_base %uint_1
         %d1 = OpAccessChain %ptr_uint %dst %uint_0 %d1_i
               OpStore %d1 %r1
       %s2_i = OpIAdd %uint %s_base %uint_2
         %s2 = OpAccessChain %ptr_uint %src %uint_0 %s2_i
         %v2 = OpLoad %uint %s2
         %r2 = OpSelect %uint %keep %v2 %uint_0
       %d2_i = OpIAdd %uint %d_base %uint_2
         %d2 = OpAccessChain %ptr_uint %dst %uint_0 %d2_i
               OpStore %d2 %r2
       %s3_i = OpIAdd %uint %s_base %uint_3
         %s3 = OpAccessChain %ptr_uint %src %uint_0 %s3_i
         %v3 = OpLoad %uint %s3
         %r3 = OpSelect %uint %keep %v3 %uint_0
       %d3_i = OpIAdd %uint %d_base %uint_3
         %d3 = OpAccessChain %ptr_uint %dst %uint_0 %d3_i
               OpStore %d3 %r3
               OpBranch %merge
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

// Indexed variant: VkDrawIndexedIndirectCommand has five u32 words
// (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance).
static constexpr const char* kIndirectCountIndexedShader = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gid
               OpExecutionMode %main LocalSize 64 1 1
               OpDecorate %gid BuiltIn GlobalInvocationId
               OpDecorate %src DescriptorSet 0
               OpDecorate %src Binding 0
               OpDecorate %dst DescriptorSet 0
               OpDecorate %dst Binding 1
               OpDecorate %cnt DescriptorSet 0
               OpDecorate %cnt Binding 2
               OpDecorate %src_struct BufferBlock
               OpMemberDecorate %src_struct 0 Offset 0
               OpDecorate %dst_struct BufferBlock
               OpMemberDecorate %dst_struct 0 Offset 0
               OpDecorate %cnt_struct BufferBlock
               OpMemberDecorate %cnt_struct 0 Offset 0
               OpDecorate %rt ArrayStride 4
               OpDecorate %pc_block Block
               OpMemberDecorate %pc_block 0 Offset 0
               OpMemberDecorate %pc_block 1 Offset 4
               OpMemberDecorate %pc_block 2 Offset 8
               OpMemberDecorate %pc_block 3 Offset 12
               OpMemberDecorate %pc_block 4 Offset 16
       %void = OpTypeVoid
       %func = OpTypeFunction %void
       %uint = OpTypeInt 32 0
       %bool = OpTypeBool
        %v3u = OpTypeVector %uint 3
   %ptr_in_v3 = OpTypePointer Input %v3u
        %gid = OpVariable %ptr_in_v3 Input
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
     %uint_4 = OpConstant %uint 4
     %uint_5 = OpConstant %uint 5
         %rt = OpTypeRuntimeArray %uint
  %src_struct = OpTypeStruct %rt
  %dst_struct = OpTypeStruct %rt
  %cnt_struct = OpTypeStruct %rt
 %ptr_src_struct = OpTypePointer Uniform %src_struct
 %ptr_dst_struct = OpTypePointer Uniform %dst_struct
 %ptr_cnt_struct = OpTypePointer Uniform %cnt_struct
        %src = OpVariable %ptr_src_struct Uniform
        %dst = OpVariable %ptr_dst_struct Uniform
        %cnt = OpVariable %ptr_cnt_struct Uniform
   %ptr_uint = OpTypePointer Uniform %uint
   %pc_block = OpTypeStruct %uint %uint %uint %uint %uint
%ptr_pc_block = OpTypePointer PushConstant %pc_block
         %pc = OpVariable %ptr_pc_block PushConstant
%ptr_pc_uint = OpTypePointer PushConstant %uint
       %main = OpFunction %void None %func
      %entry = OpLabel
      %gid_v = OpLoad %v3u %gid
        %idx = OpCompositeExtract %uint %gid_v 0
     %pc_src = OpAccessChain %ptr_pc_uint %pc %uint_0
    %src_off = OpLoad %uint %pc_src
     %pc_cnt = OpAccessChain %ptr_pc_uint %pc %uint_1
    %cnt_off = OpLoad %uint %pc_cnt
     %pc_dst = OpAccessChain %ptr_pc_uint %pc %uint_2
    %dst_off = OpLoad %uint %pc_dst
     %pc_max = OpAccessChain %ptr_pc_uint %pc %uint_3
      %max_v = OpLoad %uint %pc_max
   %pc_stride = OpAccessChain %ptr_pc_uint %pc %uint_4
  %stride_v = OpLoad %uint %pc_stride
        %ge = OpUGreaterThanEqual %bool %idx %max_v
               OpSelectionMerge %merge None
               OpBranchConditional %ge %merge %body
       %body = OpLabel
    %cnt_idx = OpAccessChain %ptr_uint %cnt %uint_0 %cnt_off
      %cnt_v = OpLoad %uint %cnt_idx
     %is_lt = OpULessThan %bool %cnt_v %max_v
      %count = OpSelect %uint %is_lt %cnt_v %max_v
         %i5 = OpIMul %uint %idx %stride_v
     %s_base = OpIAdd %uint %src_off %i5
     %d_base = OpIAdd %uint %dst_off %i5
       %keep = OpULessThan %bool %idx %count
         %s0 = OpAccessChain %ptr_uint %src %uint_0 %s_base
         %v0 = OpLoad %uint %s0
         %r0 = OpSelect %uint %keep %v0 %uint_0
         %d0 = OpAccessChain %ptr_uint %dst %uint_0 %d_base
               OpStore %d0 %r0
       %s1_i = OpIAdd %uint %s_base %uint_1
         %s1 = OpAccessChain %ptr_uint %src %uint_0 %s1_i
         %v1 = OpLoad %uint %s1
         %r1 = OpSelect %uint %keep %v1 %uint_0
       %d1_i = OpIAdd %uint %d_base %uint_1
         %d1 = OpAccessChain %ptr_uint %dst %uint_0 %d1_i
               OpStore %d1 %r1
       %s2_i = OpIAdd %uint %s_base %uint_2
         %s2 = OpAccessChain %ptr_uint %src %uint_0 %s2_i
         %v2 = OpLoad %uint %s2
         %r2 = OpSelect %uint %keep %v2 %uint_0
       %d2_i = OpIAdd %uint %d_base %uint_2
         %d2 = OpAccessChain %ptr_uint %dst %uint_0 %d2_i
               OpStore %d2 %r2
       %s3_i = OpIAdd %uint %s_base %uint_3
         %s3 = OpAccessChain %ptr_uint %src %uint_0 %s3_i
         %v3 = OpLoad %uint %s3
         %r3 = OpSelect %uint %keep %v3 %uint_0
       %d3_i = OpIAdd %uint %d_base %uint_3
         %d3 = OpAccessChain %ptr_uint %dst %uint_0 %d3_i
               OpStore %d3 %r3
       %s4_i = OpIAdd %uint %s_base %uint_4
         %s4 = OpAccessChain %ptr_uint %src %uint_0 %s4_i
         %v4 = OpLoad %uint %s4
         %r4 = OpSelect %uint %keep %v4 %uint_0
       %d4_i = OpIAdd %uint %d_base %uint_4
         %d4 = OpAccessChain %ptr_uint %dst %uint_0 %d4_i
               OpStore %d4 %r4
               OpBranch %merge
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

#endif  // INDIRECT_COUNT_SHADER_H



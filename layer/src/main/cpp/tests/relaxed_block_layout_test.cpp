#include "shader/relaxed_block_layout.h"
#include "shader/shader_rewriter.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <spirv-tools/libspirv.hpp>

namespace {
const char* fixture = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %invocation
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %invocation BuiltIn LocalInvocationIndex
OpDecorate %block BufferBlock
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 0
OpMemberDecorate %block 0 Offset 0
OpMemberDecorate %block 1 Offset 4
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%vec = OpTypeVector %uint 3
%block = OpTypeStruct %uint %vec
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%ptr_block = OpTypePointer Uniform %block
%ptr_vec = OpTypePointer Uniform %vec
%ptr_uint = OpTypePointer Uniform %uint
%ptr_input = OpTypePointer Input %uint
%buffer = OpVariable %ptr_block Uniform
%invocation = OpVariable %ptr_input Input
%main = OpFunction %void None %fn
%entry = OpLabel
%index = OpLoad %uint %invocation
%vp = OpAccessChain %ptr_vec %buffer %one
%v = OpLoad %vec %vp Volatile|Aligned 4
OpStore %vp %v Volatile|Aligned 4
%sp = OpAccessChain %ptr_uint %vp %index
%s = OpLoad %uint %sp Volatile
OpStore %sp %s Volatile
%old = OpAtomicIAdd %uint %sp %one %zero %one
OpReturn
OpFunctionEnd
)";
const char* aggregate = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %array ArrayStride 16
OpDecorate %runtime ArrayStride 4
OpMemberDecorate %item 0 Offset 0
OpMemberDecorate %item 1 Offset 4
OpMemberDecorate %block 0 Offset 0
OpMemberDecorate %block 1 Offset 64
OpDecorate %block BufferBlock
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 0
OpDecorate %size SpecId 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%vec = OpTypeVector %uint 3
%zero = OpConstant %uint 0
%size = OpSpecConstant %uint 2
%item = OpTypeStruct %uint %vec
%array = OpTypeArray %item %size
%runtime = OpTypeRuntimeArray %uint
%block = OpTypeStruct %array %runtime
%ptr_block = OpTypePointer Uniform %block
%ptr_array = OpTypePointer Uniform %array
%ptr_local = OpTypePointer Function %array
%buffer = OpVariable %ptr_block Uniform
%main = OpFunction %void None %fn
%entry = OpLabel
%local = OpVariable %ptr_local Function
%p = OpAccessChain %ptr_array %buffer %zero
%value = OpLoad %array %p Aligned 16
OpStore %p %value Aligned 16
OpCopyMemory %local %p Aligned 4
OpCopyMemory %p %local Aligned 4
%length = OpArrayLength %uint %buffer 1
OpReturn
OpFunctionEnd
)";
}
int main(int argc, char** argv) {
    setenv("VULKAN_FIX_SHADER_CACHE_PATH", "/dev/null", 1);
    unsigned failures = 0;
    auto check = [&](bool okay, const char* text) { printf("%s: %s\n", okay ? "PASS" : "FAIL", text); failures += !okay; };
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_0);
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& p, const char* message) {
        if (message) printf("SPIR-V %zu: %s\n", p.index, message);
    });
    spvtools::ValidatorOptions relaxed; relaxed.SetRelaxBlockLayout(true);
    for (const auto* source : {fixture, aggregate}) {
        std::vector<uint32_t> code;
        check(tools.Assemble(source, &code) && tools.Validate(code.data(), code.size(), relaxed), "source exercises a legal relaxed memory layout");
        std::string error;
        const auto original = code;
        check(vkfix::lower_relaxed_block_layout(code, error) == vkfix::BlockLayoutLowering::Changed,
            "buffer memory types and accesses are scalarized");
        if (!error.empty()) printf("Lowering: %s\n", error.c_str());
        check(tools.Validate(code), "lowered module validates without relaxed block layout");
        check(!vkfix::has_block_vectors(code.data(), code.size()), "the native memory interface no longer needs vector member alignment");
        const auto once = code;
        check(vkfix::lower_relaxed_block_layout(code, error) == vkfix::BlockLayoutLowering::Unchanged && code == once,
            "repeated processing preserves the lowered module");
        code = original;
        check(ShaderRewriter::get().rewrite(code, 0x10000) && tools.Validate(code),
            "the same-version Vulkan 1.0 shader takes the full layout conversion path");
        code = original;
        check(!ShaderRewriter::get().rewrite(code, 0x10000, vkfix::NativeRelaxedBlockLayout) && code == original,
            "an enabled native layout route retains the source byte-for-byte");
    }
    for (unsigned bits : {8u, 16u}) {
        std::string source = fixture;
        const auto width = std::to_string(bits);
        const std::string declaration = bits == 8 ?
            "OpCapability UniformAndStorageBuffer8BitAccess\nOpExtension \"SPV_KHR_8bit_storage\"\n" :
            "OpCapability UniformAndStorageBuffer16BitAccess\nOpExtension \"SPV_KHR_16bit_storage\"\n";
        source.insert(source.find("OpMemoryModel"), declaration);
        source.insert(source.find("%vec ="), "%small = OpTypeInt " + width + " 0\n");
        auto replace = [&](const char* from, const char* to) {
            const auto at = source.find(from); if (at != std::string::npos) source.replace(at, std::strlen(from), to);
        };
        replace("%vec = OpTypeVector %uint", "%vec = OpTypeVector %small");
        replace("%ptr_uint = OpTypePointer Uniform %uint", "%ptr_uint = OpTypePointer Uniform %small");
        replace("%s = OpLoad %uint", "%s = OpLoad %small");
        replace("%old = OpAtomicIAdd %uint %sp %one %zero %one", "");
        std::vector<uint32_t> code;
        check(tools.Assemble(source, &code) && tools.Validate(code.data(), code.size(), relaxed), "storage-only narrow vector source is legal");
        std::string error;
        check(vkfix::lower_relaxed_block_layout(code, error) == vkfix::BlockLayoutLowering::Changed && tools.Validate(code),
            "storage-only narrow vectors reconstruct through supported width conversions");
        if (!error.empty()) printf("Narrow lowering: %s\n", error.c_str());
    }
    for (bool push : {false, true}) {
        std::string source = fixture;
        auto replace = [&](const std::string& from, const std::string& to) {
            size_t at = 0;
            while ((at = source.find(from, at)) != std::string::npos) { source.replace(at, from.size(), to); at += to.size(); }
        };
        replace("BufferBlock", "Block");
        replace("OpStore %vp %v Volatile|Aligned 4", "");
        replace("OpStore %sp %s Volatile", "");
        replace("%old = OpAtomicIAdd %uint %sp %one %zero %one", "");
        if (push) {
            replace("Uniform", "PushConstant");
            replace("OpDecorate %buffer DescriptorSet 0", "");
            replace("OpDecorate %buffer Binding 0", "");
        }
        std::vector<uint32_t> code;
        check(tools.Assemble(source, &code) && tools.Validate(code.data(), code.size(), relaxed), "uniform/push source uses valid relaxed offsets");
        std::string error;
        check(vkfix::lower_relaxed_block_layout(code, error) == vkfix::BlockLayoutLowering::Changed && tools.Validate(code),
            "uniform/push storage retains byte offsets with standard native layout");
        if (!error.empty()) printf("Uniform/push lowering: %s\n", error.c_str());
    }
    if (argc == 2) {
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (!file || size <= 0 || size % 4) return 2;
        std::vector<uint32_t> code(size / 4); file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), size);
        std::string error;
        check(vkfix::lower_relaxed_block_layout(code, error) != vkfix::BlockLayoutLowering::Invalid && tools.Validate(code), "captured CTS shader converts to standard Vulkan 1.0 layout");
        if (!error.empty()) printf("Capture lowering: %s\n", error.c_str());
    }
    printf("Relaxed block layout failures: %u\n", failures);
    return failures ? 1 : 0;
}

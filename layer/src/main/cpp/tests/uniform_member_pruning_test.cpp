#include "shader/uniform_member_pruning.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>

static unsigned failures;
static void check(bool condition, const char* message) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

static std::vector<uint32_t> fixture(unsigned count, const char* storage = "Uniform",
                                     const char* kind = "float", bool all_used = false,
                                     bool multiple_entries = false) {
    const unsigned last = count - 1;
    const bool matrix = std::string(kind) == "matrix";
    const bool array = std::string(kind) == "array";
    std::ostringstream s;
    s << "OpCapability Shader\nOpMemoryModel Logical GLSL450\n"
         "OpEntryPoint Fragment %main \"main\" %out\n";
    if (multiple_entries)
        s << "OpEntryPoint Fragment %other \"other\" %out\n";
    s << "OpExecutionMode %main OriginUpperLeft\n";
    if (multiple_entries) s << "OpExecutionMode %other OriginUpperLeft\n";
    s << "OpMemberName %Block " << last << " \"last\"\n"
         "OpDecorate %Block Block\nOpDecorate %out Location 0\n";
    if (std::string(storage) != "PushConstant")
        s << "OpDecorate %ubo DescriptorSet 2\nOpDecorate %ubo Binding 0\n";
    for (unsigned i = 0; i < count; ++i) {
        s << "OpMemberDecorate %Block " << i << " Offset " << i * (matrix ? 64 : array ? 32 : 16) << "\n";
        if (matrix)
            s << "OpMemberDecorate %Block " << i << " ColMajor\n"
              << "OpMemberDecorate %Block " << i << " MatrixStride 16\n";
    }
    if (array) s << "OpDecorate %array ArrayStride 16\n";
    s << "%void = OpTypeVoid\n%fn = OpTypeFunction %void\n"
         "%float = OpTypeFloat 32\n%uint = OpTypeInt 32 0\n"
         "%v3 = OpTypeVector %float 3\n%v4 = OpTypeVector %float 4\n"
         "%mat = OpTypeMatrix %v4 4\n%two = OpConstant %uint 2\n"
         "%array = OpTypeArray %v3 %two\n%Block = OpTypeStruct";
    for (unsigned i = 0; i < count; ++i)
        s << (matrix ? " %mat" : array ? " %array" : " %float");
    s << "\n%blockptr = OpTypePointer " << storage << " %Block\n"
      << "%floatptr = OpTypePointer " << storage << " %float\n"
      << "%outptr = OpTypePointer Output %float\n"
         "%zero = OpConstant %uint 0\n";
    for (unsigned i = 0; i < count; ++i)
        s << "%c" << i << " = OpConstant %uint " << i << "\n";
    s << "%ubo = OpVariable %blockptr " << storage << "\n"
         "%out = OpVariable %outptr Output\n%main = OpFunction %void None %fn\n"
         "%entry = OpLabel\n";
    for (unsigned i = all_used ? 0 : last; i < count; ++i)
        s << "%member_ptr_" << i << " = OpAccessChain %floatptr %ubo %c" << i
          << (matrix || array ? " %zero %zero" : "") << "\n"
          << "%loaded_" << i << " = OpLoad %float %member_ptr_" << i << "\n";
    s << "OpStore %out %loaded_" << last << "\nOpReturn\nOpFunctionEnd\n";
    if (multiple_entries)
        s << "%other = OpFunction %void None %fn\n%otherentry = OpLabel\n"
             "%op = OpAccessChain %floatptr %ubo %zero\n%ov = OpLoad %float %op\n"
             "OpStore %out %ov\nOpReturn\nOpFunctionEnd\n";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* e) {
        printf("Fixture validation: %s\n", e);
    });
    std::vector<uint32_t> words;
    if (!tools.Assemble(s.str(), &words) || !tools.Validate(words)) {
        check(false, "assemble valid fixture");
        return {};
    }
    return words;
}

static std::vector<uint32_t> offsets(const std::vector<uint32_t>& words) {
    std::vector<uint32_t> result;
    for (size_t i = 5; i < words.size(); i += words[i] >> 16)
        if ((words[i] & 0xffff) == spv::OpMemberDecorate &&
            (words[i] >> 16) == 5 && words[i + 3] == spv::DecorationOffset)
            result.push_back(words[i + 4]);
    return result;
}

static bool has_stride(const std::vector<uint32_t>& words, spv::Decoration decoration, uint32_t stride) {
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        if ((words[i] & 0xffff) == spv::OpMemberDecorate && (words[i] >> 16) == 5 &&
            words[i + 3] == decoration && words[i + 4] == stride) return true;
        if ((words[i] & 0xffff) == spv::OpDecorate && (words[i] >> 16) == 4 &&
            words[i + 2] == decoration && words[i + 3] == stride) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    using namespace vkfix;
    check(uniform_member_pruning_enabled(0x13b5, 0x4010b1), "legacy Mali defaults on");
    check(!uniform_member_pruning_enabled(0x13b5, 0x402000), "native Vulkan 1.2 defaults off");
    check(!uniform_member_pruning_enabled(0x5143, 0x401000), "other vendors default off");
    check(!uniform_member_pruning_enabled(0x13b5, 0x401000, "0"), "explicit opt out");
    check(uniform_member_pruning_enabled(0x5143, 0x403000, "1"), "explicit diagnostic opt in");
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
    std::string error;
    for (const char* kind : {"float", "matrix", "array"}) {
        auto original = fixture(305, "Uniform", kind);
        if (original.empty()) continue;
        auto code = original;
        check(has_large_uniform_block(code.data(), code.size()), "large UBO preflight");
        auto result = prune_unused_uniform_members(code, 0x10300, error);
        check(result == UniformPruneResult::Changed && validator.Validate(code), "pruned module validates");
        const auto expected = 304u * (std::string(kind) == "matrix" ? 64 : std::string(kind) == "array" ? 32 : 16);
        check(offsets(code) == std::vector<uint32_t>{expected}, "retained member keeps original byte offset");
        check(code.size() < original.size(), "unused members actually removed");
        if (std::string(kind) == "matrix")
            check(has_stride(code, spv::DecorationMatrixStride, 16), "matrix stride preserved");
        if (std::string(kind) == "array")
            check(has_stride(code, spv::DecorationArrayStride, 16), "array stride preserved");
        auto before = code;
        check(prune_unused_uniform_members(code, 0x10300, error) == UniformPruneResult::Unchanged &&
              code == before, "pruning is idempotent");
    }
    for (const char* storage : {"StorageBuffer", "PushConstant"}) {
        auto code = fixture(305, storage);
        if (code.empty()) continue;
        auto original = code;
        check(!has_large_uniform_block(code.data(), code.size()) &&
              prune_unused_uniform_members(code, 0x10300, error) == UniformPruneResult::Unchanged &&
              code == original, "SSBO and push constant layouts do not trigger pruning");
    }
    auto small = fixture(256);
    auto small_before = small;
    check(prune_unused_uniform_members(small, 0x10300, error) == UniformPruneResult::Unchanged &&
          small == small_before, "small block uses unchanged fast path");
    auto all = fixture(305, "Uniform", "float", true);
    auto all_before = all;
    check(prune_unused_uniform_members(all, 0x10300, error) == UniformPruneResult::Unchanged &&
          all == all_before, "all accessed members remain intact");
    auto entries = fixture(305, "Uniform", "float", false, true);
    check(!entries.empty() && prune_unused_uniform_members(entries, 0x10300, error) == UniformPruneResult::Changed &&
          validator.Validate(entries) && offsets(entries) == std::vector<uint32_t>({0, 304 * 16}),
          "members accessed by every entry point are retained");
    auto bad = fixture(305);
    if (!bad.empty()) {
        bad.pop_back();
        auto original = bad;
        check(prune_unused_uniform_members(bad, 0x10300, error) == UniformPruneResult::Invalid &&
              bad == original && !error.empty(), "validation failure preserves original bytes");
    }
    std::vector<uint32_t> truncated{spv::MagicNumber, 0x10300, 0, 1, 0, 0xffff001e};
    auto truncated_before = truncated;
    check(prune_unused_uniform_members(truncated, 0x10300, error) == UniformPruneResult::Invalid &&
          truncated == truncated_before, "truncated instruction safely rejected");

    // Export the implementation's result for real captured shader replay.
    if ((argc - 1) % 2) return 2;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::ifstream input(argv[i], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        if (bytes.empty() || bytes.size() % 4) return 2;
        std::vector<uint32_t> code(bytes.size() / 4);
        std::copy(bytes.begin(), bytes.end(), reinterpret_cast<char*>(code.data()));
        auto result = prune_unused_uniform_members(code, 0x10300, error);
        check(result == UniformPruneResult::Changed && validator.Validate(code), argv[i]);
        std::ofstream output(argv[i + 1], std::ios::binary);
        output.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
        if (!output) return 2;
    }
    printf("Uniform pruning regression: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}

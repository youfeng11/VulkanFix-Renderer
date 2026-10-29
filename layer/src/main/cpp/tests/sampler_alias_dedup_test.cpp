#include "shader/sampler_alias_dedup.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>

static unsigned failures;
static void check(bool condition, const char* message) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
static std::vector<uint32_t> fixture(bool array = true, bool entries = false,
                                      unsigned binding = 0, bool different_type = false,
                                      const char* decorations = "", unsigned shape = 0) {
    std::ostringstream s;
    s << "OpCapability Shader\nOpMemoryModel Logical GLSL450\n"
         "OpEntryPoint GLCompute %main \"main\"";
    if (entries) s << " %a %200";
    s << "\nOpExecutionMode %main LocalSize 1 1 1\n";
    if (entries) s << "OpEntryPoint GLCompute %other \"other\" %a %200\n"
                      "OpExecutionMode %other LocalSize 1 1 1\n";
    // Entry points must precede all execution modes.
    std::string prefix = s.str();
    if (entries) {
        const std::string mode = "OpExecutionMode %main LocalSize 1 1 1\n";
        prefix.erase(prefix.find(mode), mode.size());
        prefix += mode;
    }
    s.str(""); s.clear(); s << prefix;
    s << "OpName %200 \"alias\"\n"
         "OpDecorate %a DescriptorSet 0\nOpDecorate %a Binding 0\n"
         "OpDecorate %200 DescriptorSet 0\nOpDecorate %200 Binding " << binding << "\n"
      << decorations <<
         "%void = OpTypeVoid\n%fn = OpTypeFunction %void\n"
         "%uint = OpTypeInt 32 0\n%zero = OpConstant %uint 0\n"
         "%length = OpConstant %uint 128\n"
         "%collision = OpConstant %uint 200\n"
         "%sampler = OpTypeSampler\n%array = OpTypeArray %sampler %length\n"
         "%pointer = OpTypePointer UniformConstant " << (array ? "%array" : "%sampler") << "\n";
    // Different array lengths give genuinely distinct, valid pointer types.
    if (different_type)
        s << "%length2 = OpConstant %uint 64\n%array2 = OpTypeArray %sampler %length2\n"
             "%pointer2 = OpTypePointer UniformConstant %array2\n";
    if (array) s << "%element = OpTypePointer UniformConstant %sampler\n";
    s << "%a = OpVariable %pointer UniformConstant\n"
         "%200 = OpVariable " << (different_type ? "%pointer2" : "%pointer") << " UniformConstant\n"
         "%main = OpFunction %void None %fn\n%entry = OpLabel\n";
    if (array) s << "%ap = OpAccessChain %element %a %zero\n"
                    "%bp = OpAccessChain %element %200 %zero\n"
                    "%av = OpLoad %sampler %ap\n%bv = OpLoad %sampler %bp\n";
    else s << "%av = OpLoad %sampler %a\n%bv = OpLoad %sampler %200\n";
    s << "OpReturn\nOpFunctionEnd\n";
    if (entries) s << "%other = OpFunction %void None %fn\n%entry2 = OpLabel\n"
                      "%op = OpAccessChain %element %200 %zero\n"
                      "%ov = OpLoad %sampler %op\nOpReturn\nOpFunctionEnd\n";
    spvtools::SpirvTools tools(entries ? SPV_ENV_VULKAN_1_2 : SPV_ENV_VULKAN_1_1);
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* text) {
        printf("SPIR-V: %s\n", text);
    });
    std::vector<uint32_t> words;
    std::string assembly = s.str();
    auto replace = [&](const std::string& from, const std::string& to) {
        size_t i = 0;
        while ((i = assembly.find(from, i)) != std::string::npos) {
            assembly.replace(i, from.size(), to);
            i += to.size();
        }
    };
    if (shape == 1) {
        replace("OpMemoryModel", "OpCapability RuntimeDescriptorArray\nOpExtension \"SPV_EXT_descriptor_indexing\"\nOpMemoryModel");
        replace("%array = OpTypeArray %sampler %length", "%array = OpTypeRuntimeArray %sampler");
    } else if (shape == 2) {
        replace("%array = OpTypeArray %sampler %length",
                "%inner = OpTypeArray %sampler %length\n%array = OpTypeArray %inner %length");
        replace("%element %a %zero", "%element %a %zero %zero");
        replace("%element %200 %zero", "%element %200 %zero %zero");
    } else if (shape == 3) {
        replace("%sampler = OpTypeSampler", "%float = OpTypeFloat 32\n%sampler = OpTypeImage %float 2D 0 0 0 1 Unknown");
    }
    check(tools.Assemble(assembly, &words, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS) &&
          tools.Validate(words) == (shape != 2), "fixture validation matches expectation");
    return words;
}
static bool literal_retained(const std::vector<uint32_t>& code) {
    for (size_t i = 5; i < code.size(); i += code[i] >> 16)
        if ((code[i] & 0xffff) == spv::OpConstant && (code[i] >> 16) == 4 &&
            code[i + 3] == 200) return true;
    return false;
}
int main(int argc, char** argv) {
    using namespace vkfix;
    check(sampler_alias_dedup_enabled(0x13b5, 0xa8670000, 134221824), "validated device defaults on");
    check(!sampler_alias_dedup_enabled(0x13b5, 0xa8670001, 134221824), "other device defaults off");
    check(!sampler_alias_dedup_enabled(0x13b5, 0xa8670000, 134221825), "other driver defaults off");
    check(!sampler_alias_dedup_enabled(0x5143, 0xa8670000, 134221824), "other vendor defaults off");
    check(!sampler_alias_dedup_enabled(0x13b5, 0xa8670000, 134221824, "0"), "explicit opt out");
    check(sampler_alias_dedup_enabled(0, 0, 0, "1"), "explicit diagnostic opt in");
    check(!sampler_alias_dedup_enabled(0, 0, 0, "unknown"), "unknown override uses auto policy");
    std::string error;
    for (bool array : {false, true}) for (bool entries : {false, true}) {
        if (!array && entries) continue;
        auto code = fixture(array, entries);
        auto original = code;
        const auto version = entries ? 0x10500u : 0x10300u;
        check(has_sampler_aliases(code.data(), code.size()), "preflight finds aliases");
        check(dedup_sampler_aliases(code, version, error) == SamplerAliasResult::Changed &&
              code.size() < original.size() && literal_retained(code), "merge aliases, retain colliding literal");
        spvtools::SpirvTools validator(entries ? SPV_ENV_VULKAN_1_2 : SPV_ENV_VULKAN_1_1);
        check(validator.Validate(code), "rewritten references and entry interfaces validate");
        auto once = code;
        check(!has_sampler_aliases(code.data(), code.size()) &&
              dedup_sampler_aliases(code, version, error) == SamplerAliasResult::Unchanged &&
              once == code, "idempotent");
    }
    for (auto code : {fixture(true, false, 1), fixture(true, false, 0, true),
                      fixture(true, false, 0, false, "OpDecorate %200 RelaxedPrecision\n"),
                      fixture(true, false, 0, false, "%group = OpDecorationGroup\n"),
                      fixture(true, false, 0, false, "", 1),
                      fixture(true, false, 0, false, "", 2),
                      fixture(true, false, 0, false, "", 3)}) {
        const auto before = code;
        check(!has_sampler_aliases(code.data(), code.size()) &&
              dedup_sampler_aliases(code, 0x10300, error) == SamplerAliasResult::Unchanged &&
              code == before, "conservative skip preserves bytes");
    }
    auto original = fixture();
    bool truncations_safe = true;
    for (size_t size = 0; size < original.size(); ++size) {
        auto code = std::vector<uint32_t>(original.begin(), original.begin() + size);
        auto before = code;
        const auto result = dedup_sampler_aliases(code, 0x10300, error);
        truncations_safe &= result != SamplerAliasResult::Changed && code == before;
    }
    check(truncations_safe, "every truncation boundary fails open");
    auto bad = original;
    bad[5] &= 0xffff;
    auto before = bad;
    check(dedup_sampler_aliases(bad, 0x10300, error) == SamplerAliasResult::Invalid && bad == before,
          "zero word count safely rejected");
    auto huge = fixture(true, false, 1);
    huge[3] = 0xffffffffu;
    before = huge;
    check(dedup_sampler_aliases(huge, 0x10300, error) == SamplerAliasResult::Unchanged && huge == before,
          "no allocation from untrusted ID bound");

    if ((argc - 1) % 2) return 2;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::ifstream input(argv[i], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        if (bytes.empty() || bytes.size() % 4) return 2;
        std::vector<uint32_t> code(bytes.size() / 4);
        std::memcpy(code.data(), bytes.data(), bytes.size());
        check(dedup_sampler_aliases(code, 0x10300, error) == SamplerAliasResult::Changed, argv[i]);
        std::ofstream output(argv[i + 1], std::ios::binary);
        output.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
        if (!output) return 2;
    }
    printf("Sampler alias regression: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}

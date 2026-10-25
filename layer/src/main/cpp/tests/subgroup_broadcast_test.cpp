#include "shader/shader_rewriter.h"
#include "shader/subgroup_broadcast_lowering.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <string>
#include <thread>
static unsigned failures = 0;
static void check(bool ok, const char *name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
static std::string fixture(const std::string &type, const std::string &declaration,
                           const std::string &caps = "") {
    return "OpCapability Shader\nOpCapability GroupNonUniform\nOpCapability "
           "GroupNonUniformBallot\nOpCapability GroupNonUniformQuad\n" +
           caps + R"(
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %push %value
OpExecutionMode %main LocalSize 32 1 1
OpDecorate %Push Block
OpMemberDecorate %Push 0 Offset 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%bool = OpTypeBool
%bool4 = OpTypeVector %bool 4
)" + declaration +
           R"(
%Push = OpTypeStruct %uint
%ptrpush = OpTypePointer PushConstant %Push
%ptrindex = OpTypePointer PushConstant %uint
%scope = OpConstant %uint 3
%zero = OpConstant %uint 0
%push = OpVariable %ptrpush PushConstant
)" + "%ptrvalue = OpTypePointer Private " +
           type + "\n%value = OpVariable %ptrvalue Private\n" + R"(
%main = OpFunction %void None %fn
%label = OpLabel
%indexptr = OpAccessChain %ptrindex %push %zero
%index = OpLoad %uint %indexptr
)" + "%loaded = OpLoad " +
           type + " %value\n" + "%broadcast = OpGroupNonUniformBroadcast " + type +
           " %scope %loaded %index\n" + "%quad = OpGroupNonUniformQuadBroadcast " + type +
           " %scope %loaded %index\nOpReturn\nOpFunctionEnd\n";
}
int main() {
    setenv("VULKAN_FIX_SHADER_CACHE_PATH", "/dev/null", 1);
    setenv("VULKAN_FIX_LOG_PATH", "/dev/null", 1);
    spvtools::SpirvTools source(SPV_ENV_VULKAN_1_2), target(SPV_ENV_VULKAN_1_1);
    auto messages = [](spv_message_level_t, const char *, const spv_position_t &,
                       const char *error) { printf("Validation: %s\n", error); };
    source.SetMessageConsumer(messages);
    target.SetMessageConsumer(messages);
    struct Type {
        const char *type;
        const char *decl;
        const char *caps;
    };
    const Type types[] = {
        {"%uint", "", ""},
        {"%bool", "", ""},
        {"%bool4", "", ""},
        {"%v4uint", "%v4uint = OpTypeVector %uint 4\n", ""},
        {"%float", "%float = OpTypeFloat 32\n", ""},
        {"%v4float", "%float = OpTypeFloat 32\n%v4float = OpTypeVector %float 4\n", ""},
        {"%i8", "%i8 = OpTypeInt 8 1\n", "OpCapability Int8\n"},
        {"%i16", "%i16 = OpTypeInt 16 1\n", "OpCapability Int16\n"},
        {"%u64", "%u64 = OpTypeInt 64 0\n", "OpCapability Int64\n"},
        {"%v2u64", "%u64 = OpTypeInt 64 0\n%v2u64 = OpTypeVector %u64 2\n", "OpCapability Int64\n"},
        {"%f16", "%f16 = OpTypeFloat 16\n", "OpCapability Float16\n"},
        {"%f64", "%f64 = OpTypeFloat 64\n", "OpCapability Float64\n"},
    };
    std::vector<uint32_t> base;
    for (auto &type : types) {
        std::vector<uint32_t> original;
        bool assembled = source.Assemble(fixture(type.type, type.decl, type.caps), &original);
        check(assembled && source.Validate(original),
              (std::string("valid dynamic input: ") + type.type).c_str());
        if (!assembled)
            continue;
        if (base.empty())
            base = original;
        for (bool shuffle : {false, true}) {
            auto words = original;
            std::string error;
            auto result = vkfix::lower_subgroup_broadcasts(words, 32, shuffle, error);
            bool downgraded = result == vkfix::BroadcastLowering::Changed &&
                              ShaderRewriter::get().rewrite(words, 0x10300);
            check(downgraded && target.Validate(words),
                  (std::string(shuffle ? "shuffle + quad lowering: "
                                       : "constant broadcast + quad lowering: ") +
                   type.type)
                      .c_str());
            if (!error.empty())
                printf("Lowering error: %s\n", error.c_str());
        }
    }
    std::string error;
    auto constant_text = fixture("%uint", "");
    auto replace = [&](const char *from, const char *to) {
        auto position = constant_text.find(from);
        constant_text.replace(position, strlen(from), to);
    };
    replace("%scope %loaded %index", "%scope %loaded %zero");
    replace("%scope %loaded %index", "%scope %loaded %zero");
    std::vector<uint32_t> constant;
    source.Assemble(constant_text, &constant);
    auto saved = constant;
    check(vkfix::lower_subgroup_broadcasts(constant, 32, true, error) ==
                  vkfix::BroadcastLowering::Unchanged &&
              constant == saved,
          "constant broadcasts remain byte-identical");
    auto invalid = base;
    invalid.pop_back();
    invalid.push_back(0);
    saved = invalid;
    check(vkfix::lower_subgroup_broadcasts(invalid, 32, false, error) ==
                  vkfix::BroadcastLowering::Invalid &&
              invalid == saved,
          "malformed input is rejected without partial mutation");
    auto one = base;
    check(vkfix::lower_subgroup_broadcasts(one, 1, false, error) ==
                  vkfix::BroadcastLowering::Changed &&
              ShaderRewriter::get().rewrite(one, 0x10300) && target.Validate(one),
          "single-invocation subgroup fallback is valid");
    auto large = base;
    check(vkfix::lower_subgroup_broadcasts(large, 128, false, error) ==
                  vkfix::BroadcastLowering::Changed &&
              ShaderRewriter::get().rewrite(large, 0x10300) && target.Validate(large),
          "128-invocation subgroup fallback is valid");
    std::atomic<unsigned> bad{0};
    auto run = [&] {
        for (unsigned i = 0; i < 10; ++i) {
            auto code = base;
            std::string message;
            if (vkfix::lower_subgroup_broadcasts(code, 32, i % 2, message) !=
                vkfix::BroadcastLowering::Changed)
                ++bad;
        }
    };
    std::thread a(run), b(run);
    a.join();
    b.join();
    check(bad == 0, "concurrent lowerings have no shared IDs or temporary state");
    printf("Subgroup lowering failures: %u\n", failures);
    return failures ? 1 : 0;
}

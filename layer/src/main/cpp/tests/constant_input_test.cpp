#include "shader/constant_input_lowering.h"
#include "shader/shader_rewriter.h"
#include <cstdio>
#include <string>
#include <map>
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>

namespace {
std::string fixture(bool member, bool grouped, bool signed_value, bool copy, bool device) {
    std::string text = "OpCapability Shader\nOpCapability MultiView\n";
    if (device) text += "OpCapability DeviceGroup\n";
    text += "OpExtension \"SPV_KHR_multiview\"\n";
    if (device) text += "OpExtension \"SPV_KHR_device_group\"\n";
    text += "OpMemoryModel Logical GLSL450\nOpEntryPoint Vertex %main \"main\" %input %position";
    if (device) text += " %device";
    text += "\nOpName %pointerCopy \"pointerCopy\"\nOpDecorate %position BuiltIn Position\n";
    if (device) text += "OpDecorate %device BuiltIn DeviceIndex\n";
    if (grouped) text += "%group = OpDecorationGroup\nOpDecorate %group RelaxedPrecision\nOpGroupDecorate %group %selected\n";
    if (member) {
        text += "OpDecorate %block Block\n";
        text += "OpMemberDecorate %block 0 BuiltIn ViewIndex\n";
        text += "OpMemberDecorate %block 1 BuiltIn VertexIndex\n";
    } else text += "OpDecorate %input BuiltIn ViewIndex\n";
    text += "%void = OpTypeVoid\n%fn = OpTypeFunction %void\n%uint = OpTypeInt 32 0\n";
    if (signed_value) text += "%valueType = OpTypeInt 32 1\n";
    text += "%float = OpTypeFloat 32\n%v4 = OpTypeVector %float 4\n%outptr = OpTypePointer Output %v4\n%position = OpVariable %outptr Output\n%zero = OpConstant %uint 0\n%one = OpConstant %float 1\n";
    if (member) text += "%block = OpTypeStruct %valueType %uint\n%inputptr = OpTypePointer Input %block\n";
    else text += "%inputptr = OpTypePointer Input %valueType\n";
    if (copy) text += "%localptr = OpTypePointer Function %valueType\n";
    if (member && copy) text += "%leafptr = OpTypePointer Input %valueType\n";
    text += "%input = OpVariable %inputptr Input\n";
    if (device) text += "%deviceptr = OpTypePointer Input %uint\n%device = OpVariable %deviceptr Input\n";
    text += "%main = OpFunction %void None %fn\n%entry = OpLabel\n";
    if (copy) text += "%local = OpVariable %localptr Function\n";
    text += "%pointerCopy = OpCopyObject %inputptr %input\n";
    const std::string value_type = member ? "%block" : "%valueType";
    if (copy) {
        if (member) text += "%leaf = OpAccessChain %leafptr %pointerCopy %zero\n";
        text += std::string("OpCopyMemory %local ") + (member ? "%leaf" : "%pointerCopy") + " Volatile|Aligned|Nontemporal 4\n%value = OpLoad %valueType %local\n";
    }
    else text += "%value = OpLoad " + value_type + " %pointerCopy Volatile|Aligned|Nontemporal 4\n";
    if (member && !copy) text += "%selected = OpCompositeExtract %valueType %value 0\n";
    else text += "%selected = OpCopyObject %valueType %value\n";
    text += std::string("%x = ") + (signed_value ? "OpConvertSToF" : "OpConvertUToF") + " %float %selected\n";
    if (device) text += "%deviceValue = OpLoad %uint %device\n%y = OpConvertUToF %float %deviceValue\n";
    else text += "%y = OpCopyObject %float %one\n";
    text += "%positionValue = OpCompositeConstruct %v4 %x %y %one %one\nOpStore %position %positionValue\nOpReturn\nOpFunctionEnd\n";
    if (!signed_value) {
        size_t at = 0;
        while ((at = text.find("%valueType", at)) != std::string::npos) { text.replace(at, 10, "%uint"); at += 5; }
    }
    return text;
}
unsigned builtin_count(const std::vector<uint32_t>& code, uint32_t builtin) {
    unsigned result = 0;
    for (size_t at = 5; at < code.size(); at += code[at] >> 16) {
        const auto* w = code.data() + at;
        result += (w[0] & 0xffff) == spv::OpDecorate && (w[0] >> 16) == 4 && w[2] == spv::DecorationBuiltIn && w[3] == builtin;
        result += (w[0] & 0xffff) == spv::OpMemberDecorate && (w[0] >> 16) == 5 && w[3] == spv::DecorationBuiltIn && w[4] == builtin;
    }
    return result;
}
// Check the selected value reaches the observable Position.x output. Merely
// validating the transformed module would also accept a mistaken constant 0.
bool selected_x_is(const std::vector<uint32_t>& code, uint32_t view) {
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
    optimizer.RegisterPerformancePasses();
    std::vector<uint32_t> optimized;
    if (!optimizer.Run(code.data(), code.size(), &optimized)) return false;
    std::map<uint32_t, const uint32_t*> definitions;
    uint32_t position = 0, stored = 0;
    for (size_t at = 5; at < optimized.size(); at += optimized[at] >> 16) {
        const auto* w = optimized.data() + at;
        const auto op = w[0] & 0xffff;
        if (op == spv::OpDecorate && w[2] == spv::DecorationBuiltIn && w[3] == spv::BuiltInPosition) position = w[1];
        if (op == spv::OpConstant || op == spv::OpConstantComposite || op == spv::OpCompositeConstruct || op == spv::OpCopyObject)
            definitions[w[2]] = w;
        if (op == spv::OpStore && w[1] == position) stored = w[2];
    }
    auto resolve = [&](uint32_t id) {
        const uint32_t* w = nullptr;
        for (unsigned i = 0; i < 32; ++i) {
            const auto found = definitions.find(id);
            if (found == definitions.end()) return static_cast<const uint32_t*>(nullptr);
            w = found->second;
            if ((w[0] & 0xffff) != spv::OpCopyObject) return w;
            id = w[3];
        }
        return static_cast<const uint32_t*>(nullptr);
    };
    const auto* composite = resolve(stored);
    if (!composite || ((composite[0] & 0xffff) != spv::OpConstantComposite &&
        (composite[0] & 0xffff) != spv::OpCompositeConstruct)) return false;
    const auto* scalar = resolve(composite[3]);
    const float expected = float(view); uint32_t bits;
    std::memcpy(&bits, &expected, sizeof(bits));
    return scalar && (scalar[0] & 0xffff) == spv::OpConstant && scalar[3] == bits;
}
}
int main() {
    setenv("VULKAN_FIX_SHADER_CACHE_PATH", "/dev/null", 1);
    unsigned failures = 0;
    auto check = [&](bool okay, const char* label) { printf("%s: %s\n", okay ? "PASS" : "FAIL", label); failures += !okay; };
    spvtools::SpirvTools source(SPV_ENV_VULKAN_1_1), legacy(SPV_ENV_VULKAN_1_0);
    auto message = [](spv_message_level_t, const char*, const spv_position_t& at, const char* text) { printf("word %zu: %s\n", at.index, text); };
    source.SetMessageConsumer(message); legacy.SetMessageConsumer(message);
    for (bool member : {false, true}) for (bool grouped : {false, true}) for (bool sign : {false, true})
    for (bool copy : {false, true}) for (bool device : {false, true}) {
        std::vector<uint32_t> original;
        const bool legal = source.Assemble(fixture(member, grouped, sign, copy, device), &original) && source.Validate(original);
        check(legal, "scalar/member builtin, grouped decoration, signedness and memory-copy fixture is valid");
        if (!legal) continue;
        for (uint32_t view : {0u, 4u, 31u}) {
            auto code = original; std::string error;
            auto result = vkfix::lower_view_index(code, view, error);
            if (!error.empty()) printf("lower_view_index: %s\n", error.c_str());
            check(result == vkfix::ConstantInputLowering::Changed && !builtin_count(code, spv::BuiltInViewIndex) && source.Validate(code),
                "selected ViewIndex preserves valid Vulkan interfaces and memory operands");
            if (result != vkfix::ConstantInputLowering::Changed) continue;
            if (device) check(vkfix::lower_singleton_device_index(code, error) == vkfix::ConstantInputLowering::Changed && source.Validate(code),
                "ViewIndex selection composes with singleton DeviceIndex");
            check(selected_x_is(code, view), "observable Position.x contains the requested value, including nonzero view 31");
            check(ShaderRewriter::get().rewrite(code, 0x10000) && legacy.Validate(code), "lowered interfaces validate on Vulkan 1.0");
            const auto once = code;
            check(vkfix::lower_view_index(code, view, error) == vkfix::ConstantInputLowering::Unchanged && code == once, "ViewIndex selection is idempotent");
        }
        auto native = original; std::string error;
        const auto result = vkfix::lower_view_index_from_device(native, error);
        if (!error.empty()) printf("device bridge: %s\n", error.c_str());
        check(result == vkfix::ConstantInputLowering::Changed && !builtin_count(native, spv::BuiltInViewIndex) &&
            builtin_count(native, spv::BuiltInDeviceIndex) == 1 && source.Validate(native), "device-index pipeline mode coalesces both builtins into one native input");
    }
    std::vector<uint32_t> code; source.Assemble(fixture(true, false, true, false, true), &code);
    const auto saved = code; std::string error;
    check(vkfix::lower_view_index(code, 32, error) == vkfix::ConstantInputLowering::Invalid && code == saved, "invalid view is rejected transactionally");
    code.pop_back(); const auto broken = code;
    check(vkfix::lower_view_index(code, 4, error) == vkfix::ConstantInputLowering::Invalid && code == broken, "malformed module is rejected transactionally");
    printf("Constant-input failures: %u\n", failures); return failures ? 1 : 0;
}

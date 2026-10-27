#include "shader/vertex_store_lowering.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <tuple>

namespace {
using Value = std::vector<uint32_t>;
using Instruction = std::vector<uint32_t>;
unsigned failures;
void check(bool ok, const char* message) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
// Minimal vertex fixture: uint input at location 0; constant position output.
// Assembled offline for Vulkan 1.0; runtime assembly is outside this test.
const uint32_t fixture[] = {
    0x07230203, 0x00010000, 0x00070000, 0x0000000e, 0x00000000, 0x00020011, 0x00000001, 0x0003000e,
    0x00000000, 0x00000001, 0x0007000f, 0x00000000, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002,
    0x00000003, 0x00040047, 0x00000002, 0x0000001e, 0x00000000, 0x00040047, 0x00000003, 0x0000000b,
    0x00000000, 0x00020013, 0x00000004, 0x00030021, 0x00000005, 0x00000004, 0x00040015, 0x00000006,
    0x00000020, 0x00000000, 0x00030016, 0x00000007, 0x00000020, 0x00040017, 0x00000008, 0x00000007,
    0x00000004, 0x00040020, 0x00000009, 0x00000001, 0x00000006, 0x00040020, 0x0000000a, 0x00000003,
    0x00000008, 0x0004002b, 0x00000007, 0x0000000b, 0x00000000, 0x0007002c, 0x00000008, 0x0000000c,
    0x0000000b, 0x0000000b, 0x0000000b, 0x0000000b, 0x0004003b, 0x00000009, 0x00000002, 0x00000001,
    0x0004003b, 0x0000000a, 0x00000003, 0x00000003, 0x00050036, 0x00000004, 0x00000001, 0x00000000,
    0x00000005, 0x000200f8, 0x0000000d, 0x0003003e, 0x00000003, 0x0000000c, 0x000100fd, 0x00010038,
};

// Executes the generated compute entry up to its call into the original vertex
// shader. Unsupported instructions and aggregate-buffer OOB loads fail loudly.
// This checks the actual generated instructions, including branch/Phi behavior.
uint32_t evaluate(const Value& program, const Value& memory, uint32_t input_id,
                  uint32_t invocation = 0, uint32_t instance = 0) {
    std::vector<Instruction> code;
    std::map<uint32_t, size_t> labels, functions;
    std::map<uint32_t, Value> values;
    struct Pointer { uint32_t root, word; };
    std::map<uint32_t, Pointer> pointers;
    uint32_t entry = 0, gid = 0, input_buffer = 0;
    for (size_t p = 5; p < program.size();) {
        const auto size = program[p] >> 16;
        if (!size || p + size > program.size()) throw std::runtime_error("invalid instruction");
        Instruction w(program.begin() + p, program.begin() + p + size);
        const auto op = w[0] & 65535;
        if (op == spv::OpEntryPoint) entry = w[2];
        if (op == spv::OpDecorate && w[2] == spv::DecorationBuiltIn &&
            w[3] == spv::BuiltInGlobalInvocationId) gid = w[1];
        if (op == spv::OpDecorate && w[2] == spv::DecorationBinding && w[3] == 1)
            input_buffer = w[1];
        if (op == spv::OpConstant) values[w[2]] = {w[3]};
        if (op == spv::OpConstantComposite) {
            Value composite;
            for (size_t i = 3; i < w.size(); ++i) composite.push_back(values.at(w[i]).at(0));
            values[w[2]] = composite;
        }
        if (op == spv::OpVariable) pointers[w[2]] = {w[2], 0};
        if (op == spv::OpFunction) functions[w[2]] = code.size();
        if (op == spv::OpLabel) labels[w[1]] = code.size();
        code.push_back(std::move(w));
        p += size;
    }
    auto scalar = [&](uint32_t id) { return values.at(id).at(0); };
    uint32_t current_label = 0, predecessor = 0;
    size_t pc = functions.at(entry) + 1;
    for (unsigned steps = 0; steps < 10000; ++steps) {
        const auto& w = code.at(pc++);
        const auto op = w[0] & 65535;
        uint32_t result = 0;
        switch (op) {
            case spv::OpLabel: current_label = w[1]; continue;
            case spv::OpSelectionMerge: continue;
            case spv::OpBranch:
                predecessor = current_label; pc = labels.at(w[1]); continue;
            case spv::OpBranchConditional:
                predecessor = current_label; pc = labels.at(scalar(w[1]) ? w[2] : w[3]); continue;
            case spv::OpPhi:
                for (size_t i = 3; i + 1 < w.size(); i += 2)
                    if (w[i + 1] == predecessor) values[w[2]] = values.at(w[i]);
                continue;
            case spv::OpAccessChain:
                pointers[w[2]] = {pointers.at(w[3]).root, scalar(w.back())}; continue;
            case spv::OpLoad: {
                const auto pointer = pointers.at(w[3]);
                if (pointer.root == gid) values[w[2]] = {invocation, instance, 0};
                else if (pointer.root == input_buffer) values[w[2]] = {memory.at(pointer.word)};
                else throw std::runtime_error("unexpected generated load");
                continue;
            }
            case spv::OpStore: values[w[1]] = values.at(w[2]); continue;
            case spv::OpCompositeExtract: result = values.at(w[3]).at(w[4]); break;
            case spv::OpUMulExtended: {
                const uint64_t product = uint64_t(scalar(w[3])) * scalar(w[4]);
                values[w[2]] = {uint32_t(product), uint32_t(product >> 32)}; continue;
            }
            case spv::OpIAdd: result = scalar(w[3]) + scalar(w[4]); break;
            case spv::OpISub: result = scalar(w[3]) - scalar(w[4]); break;
            case spv::OpIMul: result = scalar(w[3]) * scalar(w[4]); break;
            case spv::OpShiftRightLogical: result = scalar(w[3]) >> scalar(w[4]); break;
            case spv::OpBitwiseAnd: result = scalar(w[3]) & scalar(w[4]); break;
            case spv::OpIEqual: result = scalar(w[3]) == scalar(w[4]); break;
            case spv::OpINotEqual: result = scalar(w[3]) != scalar(w[4]); break;
            case spv::OpULessThan: result = scalar(w[3]) < scalar(w[4]); break;
            case spv::OpSLessThan: result = int32_t(scalar(w[3])) < int32_t(scalar(w[4])); break;
            case spv::OpULessThanEqual: result = scalar(w[3]) <= scalar(w[4]); break;
            case spv::OpUGreaterThanEqual: result = scalar(w[3]) >= scalar(w[4]); break;
            case spv::OpLogicalAnd: result = scalar(w[3]) && scalar(w[4]); break;
            case spv::OpSelect: values[w[2]] = values.at(scalar(w[3]) ? w[4] : w[5]); continue;
            case spv::OpBitcast: values[w[2]] = values.at(w[3]); continue;
            case spv::OpFunctionCall: return values.at(input_id).at(0);
            default: throw std::runtime_error("unhandled opcode " + std::to_string(op));
        }
        values[w[2]] = {result};
    }
    throw std::runtime_error("generated entry did not reach vertex function");
}
}

int main() {
    try {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_0);
        Value original(std::begin(fixture), std::end(fixture));
        uint32_t input = 0;
        for (size_t p = 5; p < original.size(); p += original[p] >> 16)
            if ((original[p] & 65535) == spv::OpDecorate && original[p + 2] == spv::DecorationLocation)
                input = original[p + 1];
        std::map<std::tuple<VkFormat, VkVertexInputRate, uint32_t>, Value> programs;
        auto run = [&](uint32_t first, uint32_t stride, uint32_t length, uint32_t attr_offset,
                       VkFormat format, VkVertexInputRate rate = VK_VERTEX_INPUT_RATE_VERTEX,
                       uint32_t index_bits = 0, uint32_t index_length = UINT32_MAX,
                       uint32_t first_index = 0, uint32_t invocation = 0) {
            const auto key = std::make_tuple(format, rate, attr_offset);
            auto found = programs.find(key);
            if (found == programs.end()) {
                vkfix::VertexStoreProgram lowered;
                if (!vkfix::lower_vertex_stores(original, "main", 0, {{0, 0, attr_offset, format, rate}}, lowered))
                    throw std::runtime_error(lowered.error);
                if (!tools.Validate(lowered.compute) || !tools.Validate(lowered.replay))
                    throw std::runtime_error("generated stages fail validation");
                found = programs.emplace(key, std::move(lowered.compute)).first;
            }
            Value memory(96, 0xa5a5a5a5);
            for (unsigned i = 0; i < 56; ++i) memory[i] = 0;
            memory[0] = invocation + 1; memory[1] = 1; memory[2] = first; memory[3] = first;
            memory[4] = index_bits; memory[5] = first_index; memory[6] = 80 * 4;
            memory[7] = index_length == UINT32_MAX ? index_bits / 8 : index_length;
            memory[8] = 64 * 4; memory[9] = stride; memory[vkfix::kVertexStoreBindingLengths] = length;
            memory[64] = 0x12345678;
            memory[80] = 1;
            return evaluate(found->second, memory, input, invocation);
        };
        check(run(0, 4, 4, 0, VK_FORMAT_R32_UINT) == 0x12345678, "in-range vertex load retains its value");
        check(run(1, 4, 4, 0, VK_FORMAT_R32_UINT) == 0, "out-of-range vertex cannot read a neighboring packed buffer");
        check(run(0x40000000, 4, 4, 0, VK_FORMAT_R32_UINT) == 0, "32-bit stride multiplication cannot wrap into valid data");
        check(run(1, 4, 4, 0xfffffffc, VK_FORMAT_R32_UINT) == 0, "attribute offset addition cannot wrap into valid data");
        check(run(0, 4, 3, 0, VK_FORMAT_R32_UINT) == 0, "partly out-of-range 32-bit component returns zero");
        check(run(0, 1, 1, 0, VK_FORMAT_R8_UINT) == 0x78, "last valid 8-bit component is preserved");
        check(run(0, 2, 2, 0, VK_FORMAT_R16_UINT) == 0x5678, "last valid 16-bit component is preserved");
        check(run(UINT32_MAX, 0, 4, 0, VK_FORMAT_R32_UINT) == 0x12345678, "zero-stride binding remains valid for any invocation");
        check(run(1, 4, 4, 0, VK_FORMAT_R32_UINT, VK_VERTEX_INPUT_RATE_INSTANCE) == 0,
              "instance-rate fetch obeys its own binding range");
        check(run(0, 4, 4, 0, VK_FORMAT_R32_UINT, VK_VERTEX_INPUT_RATE_VERTEX, 16) == 0,
              "16-bit index selecting an out-of-range vertex is bounded");
        check(run(0, 4, 4, 0, VK_FORMAT_R32_UINT, VK_VERTEX_INPUT_RATE_VERTEX, 32) == 0,
              "32-bit index selecting an out-of-range vertex is bounded");
        check(run(0, 4, 4, 0, VK_FORMAT_R32_UINT, VK_VERTEX_INPUT_RATE_VERTEX, 16, 1) == 0x12345678,
              "partly out-of-range 16-bit index is safely read as zero");
        check(run(0, 4, 4, 0, VK_FORMAT_R32_UINT, VK_VERTEX_INPUT_RATE_VERTEX, 32, 0) == 0x12345678,
              "empty index binding cannot read neighboring data");
        check(run(0, 4, 4, 0, VK_FORMAT_R32_UINT, VK_VERTEX_INPUT_RATE_VERTEX, 32, 4,
                  UINT32_MAX, 1) == 0x12345678, "first-index addition overflow cannot wrap to an index-buffer read");
        check(run(UINT32_MAX, 4, 4, 0, VK_FORMAT_R32_UINT, VK_VERTEX_INPUT_RATE_VERTEX, 16) == 0x12345678,
              "valid negative base-vertex offset preserves the indexed vertex");
        check(run(UINT32_MAX - 1, 4, 4, 0, VK_FORMAT_R32_UINT, VK_VERTEX_INPUT_RATE_VERTEX, 16) == 0,
              "negative final vertex cannot fetch from another packed range");
        uint32_t seed = 0xcafe;
        auto random = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
        bool correct = true;
        for (unsigned i = 0; i < 2000; ++i) {
            const uint32_t first = i % 3 ? random() % 20 : random();
            const uint32_t stride = (random() % 9) * 4, length = random() % 64;
            const uint64_t relative = uint64_t(first) * stride;
            const uint32_t expected = relative + 4 > length ? 0 :
                                      relative == 0 ? 0x12345678 : 0xa5a5a5a5;
            correct &= run(first, stride, length, 0, VK_FORMAT_R32_UINT) == expected;
        }
        check(correct, "2000 generated-SPIR-V fetches match an independent 64-bit bounds reference");
    } catch (const std::exception& error) {
        printf("FAIL: %s\n", error.what()); ++failures;
    }
    printf("Vertex input bounds regressions: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}

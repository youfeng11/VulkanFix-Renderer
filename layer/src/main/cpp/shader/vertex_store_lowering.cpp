#include "vertex_store_lowering.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <stdexcept>
#include <unordered_map>

namespace vkfix {
namespace {
using Words = std::vector<uint32_t>;
struct Inst {
    Words w;
    uint32_t result = 0, type = 0;
    Words references;
    uint32_t op() const {
        return w[0] & 65535;
    }
};
void put(Words &out, uint32_t op, const Words &args) {
    out.push_back((uint32_t(args.size() + 1) << 16) | op);
    out.insert(out.end(), args.begin(), args.end());
}
Words string_words(const char *text) {
    Words result((strlen(text) + 4) / 4, 0);
    memcpy(result.data(), text, strlen(text));
    return result;
}
bool debug_op(uint32_t op) {
    return op == spv::OpName || op == spv::OpMemberName || op == spv::OpString ||
           op == spv::OpSource || op == spv::OpSourceExtension || op == spv::OpSourceContinued ||
           op == spv::OpLine || op == spv::OpNoLine || op == spv::OpModuleProcessed;
}
struct Format {
    uint32_t bits = 0, components = 0;
    enum Kind { Float, UInt, SInt, UNorm, SNorm } kind = Float;
};
Format format_info(VkFormat f) {
    // Vulkan's contiguous core format groups use UNORM,SNORM,USCALED,SSCALED,
    // UINT,SINT,SRGB for 8-bit, and add SFLOAT as the seventh 16-bit format.
    for (auto base : {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM,
                      VK_FORMAT_R8G8B8A8_UNORM}) {
        if (f >= base && f <= base + 5) {
            const uint32_t n = base == VK_FORMAT_R8_UNORM       ? 1
                               : base == VK_FORMAT_R8G8_UNORM   ? 2
                               : base == VK_FORMAT_R8G8B8_UNORM ? 3
                                                                : 4;
            const int k = int(f) - int(base);
            if (k == 2 || k == 3)
                break;
            return {8, n,
                    k == 0   ? Format::UNorm
                    : k == 1 ? Format::SNorm
                    : k == 4 ? Format::UInt
                             : Format::SInt};
        }
    }
    for (auto base : {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM,
                      VK_FORMAT_R16G16B16A16_UNORM}) {
        if (f >= base && f <= base + 5) {
            const uint32_t n = base == VK_FORMAT_R16_UNORM         ? 1
                               : base == VK_FORMAT_R16G16_UNORM    ? 2
                               : base == VK_FORMAT_R16G16B16_UNORM ? 3
                                                                   : 4;
            const int k = int(f) - int(base);
            if (k == 2 || k == 3)
                break;
            return {16, n,
                    k == 0   ? Format::UNorm
                    : k == 1 ? Format::SNorm
                    : k == 4 ? Format::UInt
                             : Format::SInt};
        }
    }
    for (auto base : {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                      VK_FORMAT_R32G32B32A32_UINT}) {
        if (f >= base && f <= base + 2) {
            const uint32_t n = base == VK_FORMAT_R32_UINT         ? 1
                               : base == VK_FORMAT_R32G32_UINT    ? 2
                               : base == VK_FORMAT_R32G32B32_UINT ? 3
                                                                  : 4;
            return {32, n, f == base ? Format::UInt : f == base + 1 ? Format::SInt : Format::Float};
        }
    }
    throw std::runtime_error("unsupported vertex attribute format");
}
struct Builder {
    uint32_t next;
    Words annotations, declarations, body;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> pointers;
    std::map<uint32_t, uint32_t> constants;
    uint32_t u32, i32, f32, boolean, void_type, fn_type, v3uint;
    uint32_t scratch, inputs, storage;
    explicit Builder(uint32_t bound, uint32_t storage_class) : next(bound), storage(storage_class) {
        void_type = type(spv::OpTypeVoid, {});
        boolean = type(spv::OpTypeBool, {});
        u32 = type(spv::OpTypeInt, {32, 0});
        i32 = type(spv::OpTypeInt, {32, 1});
        f32 = type(spv::OpTypeFloat, {32});
        v3uint = type(spv::OpTypeVector, {u32, 3});
        fn_type = type(spv::OpTypeFunction, {void_type});
    }
    uint32_t id() {
        return next++;
    }
    uint32_t type(uint32_t op, Words args) {
        auto n = id();
        args.insert(args.begin(), n);
        put(declarations, op, args);
        return n;
    }
    uint32_t pointer(uint32_t sc, uint32_t t) {
        auto key = std::make_pair(sc, t);
        auto it = pointers.find(key);
        if (it != pointers.end())
            return it->second;
        return pointers[key] = type(spv::OpTypePointer, {sc, t});
    }
    uint32_t constant(uint32_t v) {
        auto it = constants.find(v);
        if (it != constants.end())
            return it->second;
        const auto n = id();
        put(declarations, spv::OpConstant, {u32, n, v});
        return constants[v] = n;
    }
    uint32_t op(uint32_t code, uint32_t t, Words args) {
        const auto n = id();
        args.insert(args.begin(), {t, n});
        put(body, code, args);
        return n;
    }
    uint32_t unary(uint32_t code, uint32_t t, uint32_t a) {
        return op(code, t, {a});
    }
    uint32_t binary(uint32_t code, uint32_t t, uint32_t a, uint32_t b) {
        return op(code, t, {a, b});
    }
    uint32_t add(uint32_t a, uint32_t b) {
        return binary(spv::OpIAdd, u32, a, b);
    }
    uint32_t mul(uint32_t a, uint32_t b) {
        return binary(spv::OpIMul, u32, a, b);
    }
    uint32_t load(uint32_t t, uint32_t ptr) {
        return unary(spv::OpLoad, t, ptr);
    }
    void store(uint32_t ptr, uint32_t value) {
        put(body, spv::OpStore, {ptr, value});
    }
    uint32_t access(uint32_t sc, uint32_t t, uint32_t base, Words indices) {
        indices.insert(indices.begin(), base);
        return op(spv::OpAccessChain, pointer(sc, t), indices);
    }
    uint32_t word(uint32_t index) {
        return load(u32, access(storage, u32, inputs, {constant(0), index}));
    }
    uint32_t param(uint32_t index) {
        return word(constant(index));
    }
    uint32_t raw(uint32_t byte_offset, uint32_t bits) {
        const auto index = binary(spv::OpShiftRightLogical, u32, byte_offset, constant(2));
        auto value = word(index);
        const auto shift =
            mul(binary(spv::OpBitwiseAnd, u32, byte_offset, constant(3)), constant(8));
        value = binary(spv::OpShiftRightLogical, u32, value, shift);
        if (bits < 32)
            value = binary(spv::OpBitwiseAnd, u32, value, constant((1u << bits) - 1));
        return value;
    }
    uint32_t float_zero() {
        const auto n = id();
        put(declarations, spv::OpConstant, {f32, n, 0});
        return n;
    }
    uint32_t buffer(uint32_t set, uint32_t binding, bool readonly) {
        const auto array = type(spv::OpTypeRuntimeArray, {u32});
        put(annotations, spv::OpDecorate, {array, spv::DecorationArrayStride, 4});
        const auto block = type(spv::OpTypeStruct, {array});
        put(annotations, spv::OpDecorate,
            {block, storage == spv::StorageClassUniform ? spv::DecorationBufferBlock
                                                        : spv::DecorationBlock});
        put(annotations, spv::OpMemberDecorate, {block, 0, spv::DecorationOffset, 0});
        const auto ptr = pointer(storage, block), var = id();
        put(declarations, spv::OpVariable, {ptr, var, storage});
        put(annotations, spv::OpDecorate, {var, spv::DecorationDescriptorSet, set});
        put(annotations, spv::OpDecorate, {var, spv::DecorationBinding, binding});
        if (readonly)
            put(annotations, spv::OpDecorate, {var, spv::DecorationNonWritable});
        return var;
    }
};
struct Leaf {
    uint32_t variable, type;
    Words path;
};
struct Lowering {
    std::vector<Inst> ins;
    std::unordered_map<uint32_t, Inst> defs;
    std::map<uint32_t, uint32_t> builtin, location;
    std::set<uint32_t> interfaces, io_types, globals;
    std::vector<Leaf> leaves;
    uint32_t entry_id = 0;
    void flatten(uint32_t var, uint32_t type, Words path, unsigned depth = 0) {
        if (depth > 16 || leaves.size() > 256)
            throw std::runtime_error("vertex output interface is too large");
        const auto &d = defs.at(type);
        const auto &w = d.w;
        if (d.op() == spv::OpTypeFloat || d.op() == spv::OpTypeInt) {
            if (w[2] != 32)
                throw std::runtime_error("only 32-bit vertex interface scalars are supported");
            leaves.push_back({var, type, path});
            return;
        }
        uint32_t count = 0;
        if (d.op() == spv::OpTypeVector || d.op() == spv::OpTypeMatrix)
            count = w[3];
        else if (d.op() == spv::OpTypeArray) {
            const auto &c = defs.at(w[3]);
            if (c.op() != spv::OpConstant)
                throw std::runtime_error("specialization-sized output arrays are unsupported");
            count = c.w[3];
        } else if (d.op() == spv::OpTypeStruct)
            count = uint32_t(w.size() - 2);
        else
            throw std::runtime_error("unsupported vertex output type");
        if (count > 128)
            throw std::runtime_error("vertex output array is too large");
        for (uint32_t i = 0; i < count; ++i) {
            auto p = path;
            p.push_back(i);
            flatten(var, d.op() == spv::OpTypeStruct ? w[2 + i] : w[2], p, depth + 1);
        }
    }
    void mark_type(uint32_t t) {
        if (!io_types.insert(t).second)
            return;
        const auto &d = defs.at(t);
        if (d.op() == spv::OpTypeStruct)
            for (size_t i = 2; i < d.w.size(); ++i)
                mark_type(d.w[i]);
        else if (d.op() == spv::OpTypeArray || d.op() == spv::OpTypeVector ||
                 d.op() == spv::OpTypeMatrix)
            mark_type(d.w[2]);
    }
};
spv_result_t parse_inst(void *data, const spv_parsed_instruction_t *i) {
    auto &l = *static_cast<Lowering *>(data);
    Inst inst{{i->words, i->words + i->num_words}, i->result_id, i->type_id};
    for (uint32_t n = 0; n < i->num_operands; ++n) {
        const auto &operand = i->operands[n];
        if (operand.type == SPV_OPERAND_TYPE_ID)
            inst.references.push_back(i->words[operand.offset]);
    }
    l.ins.push_back(inst);
    if (i->result_id)
        l.defs.emplace(i->result_id, inst);
    return SPV_SUCCESS;
}
bool validate(const Words &code, std::string &error) {
    spv_target_env env = code[1] >= 0x10600   ? SPV_ENV_VULKAN_1_3
                         : code[1] >= 0x10500 ? SPV_ENV_VULKAN_1_2
                         : code[1] == 0x10400 ? SPV_ENV_VULKAN_1_1_SPIRV_1_4
                         : code[1] > 0x10000  ? SPV_ENV_VULKAN_1_1
                                              : SPV_ENV_VULKAN_1_0;
    spvtools::SpirvTools tools(env);
    tools.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &,
                                 const char *message) { error = message; });
    return tools.Validate(code);
}
uint32_t component(Builder &b, uint32_t value, const Format &f, uint32_t target_type,
                   const Lowering &l) {
    const auto &t = l.defs.at(target_type).w;
    const bool target_float = l.defs.at(target_type).op() == spv::OpTypeFloat;
    if (f.kind == Format::Float) {
        if (!target_float)
            throw std::runtime_error("floating vertex format bound to integer input");
        return b.unary(spv::OpBitcast, target_type, value);
    }
    if (f.kind == Format::UInt) {
        if (target_float || t[3])
            throw std::runtime_error("unsigned vertex format/input mismatch");
        return b.unary(spv::OpBitcast, target_type, value);
    }
    const bool signed_value = f.kind == Format::SInt || f.kind == Format::SNorm;
    uint32_t signed_id = 0;
    if (signed_value) {
        signed_id = b.unary(spv::OpBitcast, b.i32, value);
        if (f.bits < 32) {
            signed_id =
                b.binary(spv::OpShiftLeftLogical, b.i32, signed_id, b.constant(32 - f.bits));
            signed_id =
                b.binary(spv::OpShiftRightArithmetic, b.i32, signed_id, b.constant(32 - f.bits));
        }
    }
    if (f.kind == Format::SInt) {
        if (target_float || !t[3])
            throw std::runtime_error("signed vertex format/input mismatch");
        return b.unary(spv::OpBitcast, target_type, signed_id);
    }
    if (!target_float)
        throw std::runtime_error("normalized vertex format bound to integer input");
    const auto converted = b.unary(signed_value ? spv::OpConvertSToF : spv::OpConvertUToF,
                                   target_type, signed_value ? signed_id : value);
    float scale = signed_value ? float((1u << (f.bits - 1)) - 1) : float((1u << f.bits) - 1);
    uint32_t scale_bits;
    memcpy(&scale_bits, &scale, 4);
    const auto denominator = b.unary(spv::OpBitcast, target_type, b.constant(scale_bits));
    auto normalized = b.binary(spv::OpFDiv, target_type, converted, denominator);
    if (signed_value) {
        const auto minus_one = b.unary(spv::OpBitcast, target_type, b.constant(0xbf800000));
        const auto less = b.binary(spv::OpFOrdLessThan, b.boolean, normalized, minus_one);
        normalized = b.op(spv::OpSelect, target_type, {less, minus_one, normalized});
    }
    return normalized;
}
} // namespace

bool vertex_has_storage_effects(const Words &source) {
    if (source.size() < 5)
        return true;
    Lowering l;
    const auto ctx = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
    const auto parsed =
        spvBinaryParse(ctx, &l, source.data(), source.size(), nullptr, parse_inst, nullptr);
    spvContextDestroy(ctx);
    if (parsed != SPV_SUCCESS)
        return true;
    for (const auto &i : l.ins) {
        if (i.op() == spv::OpImageWrite || i.op() == spv::OpAtomicFlagTestAndSet ||
            i.op() == spv::OpAtomicFlagClear || i.op() == spv::OpAtomicFAddEXT ||
            i.op() == spv::OpAtomicFMinEXT || i.op() == spv::OpAtomicFMaxEXT ||
            (i.op() >= spv::OpAtomicLoad && i.op() <= spv::OpAtomicXor))
            return true;
        if (i.op() != spv::OpStore && i.op() != spv::OpCopyMemory &&
            i.op() != spv::OpCopyMemorySized)
            continue;
        const auto ptr = l.defs.find(i.w[1]);
        if (ptr == l.defs.end())
            return true;
        const auto type = l.defs.find(ptr->second.type);
        if (type == l.defs.end() || type->second.op() != spv::OpTypePointer)
            return true;
        const auto sc = type->second.w[2];
        if (sc == spv::StorageClassUniform || sc == spv::StorageClassStorageBuffer ||
            sc == spv::StorageClassImage || sc == spv::StorageClassPhysicalStorageBuffer)
            return true;
    }
    return false;
}

bool lower_vertex_stores(const Words &source, const char *entry, uint32_t set,
                         const std::vector<VertexStoreAttribute> &attributes,
                         VertexStoreProgram &result) {
    result = {};
    try {
        if (source.size() < 5 || source[0] != spv::MagicNumber || source[3] > 1000000)
            throw std::runtime_error("invalid SPIR-V header");
        Lowering l;
        const auto ctx = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
        const auto parsed =
            spvBinaryParse(ctx, &l, source.data(), source.size(), nullptr, parse_inst, nullptr);
        spvContextDestroy(ctx);
        if (parsed != SPV_SUCCESS)
            throw std::runtime_error("invalid SPIR-V instruction stream");
        for (const auto &i : l.ins) {
            const auto &w = i.w;
            if (i.op() == spv::OpEntryPoint) {
                if (l.entry_id || w[1] != spv::ExecutionModelVertex ||
                    strcmp(reinterpret_cast<const char *>(w.data() + 3), entry))
                    throw std::runtime_error("only a single vertex entry point is supported");
                l.entry_id = w[2];
            }
            if (i.op() == spv::OpDecorate && w.size() >= 4) {
                if (w[2] == spv::DecorationBuiltIn)
                    l.builtin[w[1]] = w[3];
                if (w[2] == spv::DecorationLocation)
                    l.location[w[1]] = w[3];
            }
            if (i.op() == spv::OpDecorationGroup || i.op() == spv::OpGroupDecorate ||
                i.op() == spv::OpGroupMemberDecorate)
                throw std::runtime_error("grouped interface decorations are unsupported");
        }
        if (!l.entry_id)
            throw std::runtime_error("vertex entry point not found");
        for (const auto &i : l.ins) {
            if (i.op() == spv::OpFunction)
                break;
            if (i.op() != spv::OpVariable)
                continue;
            l.globals.insert(i.result);
            if (i.w[3] != spv::StorageClassInput && i.w[3] != spv::StorageClassOutput)
                continue;
            l.interfaces.insert(i.result);
            const auto type = l.defs.at(i.type).w[3];
            l.mark_type(type);
            if (i.w[3] == spv::StorageClassOutput)
                l.flatten(i.result, type, {});
            else if (!l.builtin.count(i.result) && !l.location.count(i.result))
                throw std::runtime_error("input blocks and undecorated inputs are unsupported");
        }
        std::map<uint32_t, std::vector<Inst>> functions_by_id;
        uint32_t function_id = 0;
        for (const auto &i : l.ins) {
            if (i.op() == spv::OpFunction)
                function_id = i.result;
            if (function_id)
                functions_by_id[function_id].push_back(i);
            if (i.op() == spv::OpFunctionEnd)
                function_id = 0;
        }
        std::set<uint32_t> used_globals, visited_functions, resource_sets;
        std::vector<uint32_t> pending{l.entry_id};
        while (!pending.empty()) {
            const auto function = pending.back();
            pending.pop_back();
            if (!visited_functions.insert(function).second)
                continue;
            for (const auto &i : functions_by_id[function]) {
                if (i.op() == spv::OpFunctionCall)
                    pending.push_back(i.w[3]);
                for (auto id : i.references)
                    if (l.globals.count(id))
                        used_globals.insert(id);
            }
        }
        for (const auto &i : l.ins)
            if (i.op() == spv::OpDecorate && i.w[2] == spv::DecorationDescriptorSet &&
                used_globals.count(i.w[1]))
                resource_sets.insert(i.w[3]);
        result.resource_sets.assign(resource_sets.begin(), resource_sets.end());
        // GLSL's gl_PerVertex often declares ClipDistance/CullDistance without
        // writing them. Replaying those members would make them statically
        // written and could enable clipping with undefined values.
        struct OutputTarget {
            uint32_t variable;
            Words path;
            bool dynamic = false;
        };
        std::function<OutputTarget(uint32_t, unsigned)> output_target;
        output_target = [&](uint32_t pointer, unsigned depth) -> OutputTarget {
            if (depth > 64)
                throw std::runtime_error("cyclic output pointer");
            const auto &definition = l.defs.at(pointer);
            if (l.globals.count(pointer))
                return {pointer, {}};
            if (definition.op() == spv::OpCopyObject)
                return output_target(definition.w[3], depth + 1);
            if (definition.op() != spv::OpAccessChain &&
                definition.op() != spv::OpInBoundsAccessChain)
                throw std::runtime_error("aliased output pointers are unsupported");
            auto target = output_target(definition.w[3], depth + 1);
            for (size_t n = 4; n < definition.w.size() && !target.dynamic; ++n) {
                const auto &index = l.defs.at(definition.w[n]);
                if (index.op() != spv::OpConstant) {
                    target.dynamic = true;
                    break;
                }
                target.path.push_back(index.w[3]);
            }
            return target;
        };
        std::map<uint32_t, std::vector<Words>> written_outputs;
        for (auto function : visited_functions) {
            for (const auto &i : functions_by_id[function]) {
                if (i.op() != spv::OpStore && i.op() != spv::OpCopyMemory &&
                    i.op() != spv::OpCopyMemorySized)
                    continue;
                const auto &pointer = l.defs.at(i.w[1]);
                if (l.defs.at(pointer.type).w[2] != spv::StorageClassOutput)
                    continue;
                auto target = output_target(i.w[1], 0);
                written_outputs[target.variable].push_back(std::move(target.path));
            }
        }
        l.leaves.erase(std::remove_if(l.leaves.begin(), l.leaves.end(), [&](const Leaf &leaf) {
                           for (const auto &path : written_outputs[leaf.variable])
                               if (path.size() <= leaf.path.size() &&
                                   std::equal(path.begin(), path.end(), leaf.path.begin()))
                                   return false;
                           return true;
                       }),
                       l.leaves.end());
        if (l.leaves.empty())
            throw std::runtime_error("vertex output interface is empty");
        result.output_words = uint32_t(l.leaves.size());
        for (bool compute : {true, false}) {
            const uint32_t storage =
                source[1] >= 0x10300 ? spv::StorageClassStorageBuffer : spv::StorageClassUniform;
            Builder b(source[3], storage);
            // Reuse existing scalar types: SPIR-V forbids duplicate non-aggregate types.
            for (const auto &i : l.ins) {
                if (i.op() == spv::OpTypeVoid)
                    b.void_type = i.result;
                if (i.op() == spv::OpTypeBool)
                    b.boolean = i.result;
                if (i.op() == spv::OpTypeInt && i.w[2] == 32)
                    (i.w[3] ? b.i32 : b.u32) = i.result;
                if (i.op() == spv::OpTypeFloat && i.w[2] == 32)
                    b.f32 = i.result;
            }
            // Builder's initial scalar declarations are regenerated below after deduplication.
            b.declarations.clear();
            b.pointers.clear();
            std::set<uint32_t> existing;
            for (const auto &i : l.ins)
                existing.insert(i.result);
            for (auto item :
                 {std::make_pair(b.void_type, spv::OpTypeVoid),
                  std::make_pair(b.boolean, spv::OpTypeBool), std::make_pair(b.u32, spv::OpTypeInt),
                  std::make_pair(b.i32, spv::OpTypeInt), std::make_pair(b.f32, spv::OpTypeFloat)}) {
                if (existing.count(item.first))
                    continue;
                Words args{item.first};
                if (item.second == spv::OpTypeInt)
                    args.insert(args.end(), {32, item.first == b.i32 ? 1u : 0u});
                if (item.second == spv::OpTypeFloat)
                    args.push_back(32);
                put(b.declarations, item.second, args);
            }
            b.v3uint = 0;
            b.fn_type = 0;
            for (const auto &i : l.ins) {
                if (i.op() == spv::OpTypeVector && i.w[2] == b.u32 && i.w[3] == 3)
                    b.v3uint = i.result;
                if (i.op() == spv::OpTypeFunction && i.w.size() == 3 && i.w[2] == b.void_type)
                    b.fn_type = i.result;
                if (i.op() == spv::OpTypePointer) {
                    auto sc = i.w[2];
                    if (compute && (sc == spv::StorageClassInput || sc == spv::StorageClassOutput))
                        sc = spv::StorageClassPrivate;
                    b.pointers[{sc, i.w[3]}] = i.result;
                }
            }
            if (!b.v3uint)
                b.v3uint = b.type(spv::OpTypeVector, {b.u32, 3});
            if (!b.fn_type)
                b.fn_type = b.type(spv::OpTypeFunction, {b.void_type});
            b.scratch = b.buffer(set, 0, !compute);
            b.inputs = b.buffer(set, 1, true);
            uint32_t gid = 0, vertex_id = 0, instance_id = 0;
            Words entry_interfaces;
            if (compute) {
                const auto ptr = b.pointer(spv::StorageClassInput, b.v3uint);
                gid = b.id();
                put(b.declarations, spv::OpVariable, {ptr, gid, spv::StorageClassInput});
                put(b.annotations, spv::OpDecorate,
                    {gid, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId});
                entry_interfaces.push_back(gid);
            } else {
                const auto ptr = b.pointer(spv::StorageClassInput, b.u32);
                vertex_id = b.id();
                instance_id = b.id();
                put(b.declarations, spv::OpVariable, {ptr, vertex_id, spv::StorageClassInput});
                put(b.declarations, spv::OpVariable, {ptr, instance_id, spv::StorageClassInput});
                put(b.annotations, spv::OpDecorate,
                    {vertex_id, spv::DecorationBuiltIn, spv::BuiltInVertexIndex});
                put(b.annotations, spv::OpDecorate,
                    {instance_id, spv::DecorationBuiltIn, spv::BuiltInInstanceIndex});
                entry_interfaces = {vertex_id, instance_id};
                for (auto var : l.interfaces)
                    if (l.defs.at(var).w[3] == spv::StorageClassOutput)
                        entry_interfaces.push_back(var);
            }
            const auto zero_lod = b.float_zero();
            const auto wrapper = b.id();
            put(b.body, spv::OpFunction,
                {b.void_type, wrapper, spv::FunctionControlMaskNone, b.fn_type});
            put(b.body, spv::OpLabel, {b.id()});
            uint32_t x, y;
            if (compute) {
                const auto value = b.load(b.v3uint, gid);
                x = b.op(spv::OpCompositeExtract, b.u32, {value, 0});
                y = b.op(spv::OpCompositeExtract, b.u32, {value, 1});
            } else {
                x = b.load(b.u32, vertex_id);
                y = b.load(b.u32, instance_id);
            }
            const auto count = b.param(0);
            uint32_t merge = 0;
            if (compute) {
                const auto valid = b.binary(spv::OpULessThan, b.boolean, x, count);
                const auto then_block = b.id();
                merge = b.id();
                put(b.body, spv::OpSelectionMerge, {merge, spv::SelectionControlMaskNone});
                put(b.body, spv::OpBranchConditional, {valid, then_block, merge});
                put(b.body, spv::OpLabel, {then_block});
                const auto indexed = b.param(4), first = b.param(2);
                // Avoid reading an index buffer for non-indexed draws.
                const auto idx = b.id(), direct = b.id(), join = b.id();
                const auto is_indexed =
                    b.binary(spv::OpINotEqual, b.boolean, indexed, b.constant(0));
                put(b.body, spv::OpSelectionMerge, {join, spv::SelectionControlMaskNone});
                put(b.body, spv::OpBranchConditional, {is_indexed, idx, direct});
                put(b.body, spv::OpLabel, {idx});
                const auto index_addr =
                    b.add(b.param(6),
                          b.mul(b.add(x, b.param(5)),
                                b.binary(spv::OpShiftRightLogical, b.u32, indexed, b.constant(3))));
                auto index = b.raw(index_addr, 32);
                const auto short_index =
                    b.binary(spv::OpIEqual, b.boolean, indexed, b.constant(16));
                index = b.op(spv::OpSelect, b.u32,
                             {short_index,
                              b.binary(spv::OpBitwiseAnd, b.u32, index, b.constant(65535)), index});
                const auto indexed_vertex = b.add(index, first);
                put(b.body, spv::OpBranch, {join});
                put(b.body, spv::OpLabel, {direct});
                const auto direct_vertex = b.add(x, first);
                put(b.body, spv::OpBranch, {join});
                put(b.body, spv::OpLabel, {join});
                const auto vertex =
                    b.op(spv::OpPhi, b.u32, {indexed_vertex, idx, direct_vertex, direct});
                const auto instance = b.add(y, b.param(3));
                for (auto var : l.interfaces) {
                    const auto &d = l.defs.at(var);
                    if (d.w[3] != spv::StorageClassInput)
                        continue;
                    const auto t = l.defs.at(d.type).w[3];
                    if (l.builtin.count(var)) {
                        const auto builtin = l.builtin.at(var);
                        uint32_t value = 0;
                        if (builtin == spv::BuiltInVertexIndex || builtin == spv::BuiltInVertexId)
                            value = vertex;
                        else if (builtin == spv::BuiltInInstanceIndex ||
                                 builtin == spv::BuiltInInstanceId)
                            value = instance;
                        else if (builtin == spv::BuiltInBaseVertex)
                            value = first;
                        else if (builtin == spv::BuiltInBaseInstance)
                            value = b.param(3);
                        else if (builtin == spv::BuiltInDrawIndex)
                            value = b.constant(0);
                        else
                            throw std::runtime_error("unsupported vertex input builtin");
                        b.store(var, b.unary(spv::OpBitcast, t, value));
                        continue;
                    }
                    const auto it = std::find_if(attributes.begin(), attributes.end(),
                                                 [&](const VertexStoreAttribute &a) {
                                                     return a.location == l.location.at(var);
                                                 });
                    if (it == attributes.end() || it->binding >= kVertexStoreBindings)
                        throw std::runtime_error("vertex input has no supported binding");
                    const auto fmt = format_info(it->format);
                    if (it->offset % (fmt.bits / 8))
                        throw std::runtime_error("unaligned vertex attribute");
                    const auto base = b.add(
                        b.param(8 + 2 * it->binding),
                        b.add(b.mul(it->rate == VK_VERTEX_INPUT_RATE_INSTANCE ? instance : vertex,
                                    b.param(9 + 2 * it->binding)),
                              b.constant(it->offset)));
                    const auto &td = l.defs.at(t);
                    const bool vector = td.op() == spv::OpTypeVector;
                    const uint32_t components = vector ? td.w[3] : 1, scalar = vector ? td.w[2] : t;
                    Words values;
                    for (uint32_t c = 0; c < components; ++c) {
                        if (c >= fmt.components) {
                            const auto bits =
                                c == 3 ? (l.defs.at(scalar).op() == spv::OpTypeFloat ? 0x3f800000u
                                                                                     : 1u)
                                       : 0u;
                            values.push_back(b.unary(spv::OpBitcast, scalar, b.constant(bits)));
                        } else
                            values.push_back(component(
                                b, b.raw(b.add(base, b.constant(c * fmt.bits / 8)), fmt.bits), fmt,
                                scalar, l));
                    }
                    b.store(var, vector ? b.op(spv::OpCompositeConstruct, t, values) : values[0]);
                }
                b.op(spv::OpFunctionCall, b.void_type, {l.entry_id});
            }
            const auto output_base =
                b.mul(b.add(b.mul(y, count), x), b.constant(result.output_words));
            for (uint32_t n = 0; n < l.leaves.size(); ++n) {
                const auto &leaf = l.leaves[n];
                Words indices;
                for (auto i : leaf.path)
                    indices.push_back(b.constant(i));
                const auto ptr = indices.empty() ? leaf.variable
                                                 : b.access(compute ? spv::StorageClassPrivate
                                                                    : spv::StorageClassOutput,
                                                            leaf.type, leaf.variable, indices);
                const auto dst = b.access(storage, b.u32, b.scratch,
                                          {b.constant(0), b.add(output_base, b.constant(n))});
                if (compute)
                    b.store(dst, b.unary(spv::OpBitcast, b.u32, b.load(leaf.type, ptr)));
                else
                    b.store(ptr, b.unary(spv::OpBitcast, leaf.type, b.load(b.u32, dst)));
            }
            if (compute) {
                put(b.body, spv::OpBranch, {merge});
                put(b.body, spv::OpLabel, {merge});
            }
            put(b.body, spv::OpReturn, {});
            put(b.body, spv::OpFunctionEnd, {});
            Words prefix, annotations, declarations, functions;
            bool in_function = false;
            std::set<uint32_t> global_results;
            for (const auto &i : l.ins) {
                if (i.op() == spv::OpFunction)
                    break;
                if (i.result)
                    global_results.insert(i.result);
            }
            for (const auto &original : l.ins) {
                auto w = original.w;
                const auto op = original.op();
                if (op == spv::OpFunction)
                    in_function = true;
                if (debug_op(op) || op == spv::OpEntryPoint || op == spv::OpExecutionMode ||
                    op == spv::OpExecutionModeId)
                    continue;
                if (in_function) {
                    if (compute) {
                        uint32_t explicit_op = 0, mask_index = 5;
                        if (op == spv::OpImageSampleImplicitLod)
                            explicit_op = spv::OpImageSampleExplicitLod;
                        if (op == spv::OpImageSampleDrefImplicitLod) {
                            explicit_op = spv::OpImageSampleDrefExplicitLod;
                            mask_index = 6;
                        }
                        if (explicit_op) {
                            const auto mask = w.size() > mask_index ? w[mask_index] : 0;
                            if (mask & spv::ImageOperandsBiasMask)
                                throw std::runtime_error(
                                    "biased implicit texture LOD is unsupported");
                            if (w.size() == mask_index)
                                w.push_back(spv::ImageOperandsLodMask);
                            else
                                w[mask_index] |= spv::ImageOperandsLodMask;
                            w.insert(w.begin() + mask_index + 1, zero_lod);
                            w[0] = (uint32_t(w.size()) << 16) | explicit_op;
                        }
                        functions.insert(functions.end(), w.begin(), w.end());
                    }
                    continue;
                }
                if (op == spv::OpDecorate || op == spv::OpMemberDecorate) {
                    if (!global_results.count(w[1]))
                        continue;
                    const auto deco = w[op == spv::OpDecorate ? 2 : 3];
                    if (compute && (l.interfaces.count(w[1]) || l.io_types.count(w[1])) &&
                        (deco == spv::DecorationBuiltIn || deco == spv::DecorationLocation ||
                         deco == spv::DecorationComponent || deco == spv::DecorationBlock ||
                         deco == spv::DecorationFlat || deco == spv::DecorationNoPerspective ||
                         deco == spv::DecorationCentroid || deco == spv::DecorationSample ||
                         deco == spv::DecorationInvariant))
                        continue;
                    annotations.insert(annotations.end(), w.begin(), w.end());
                    continue;
                }
                if (op == spv::OpCapability || op == spv::OpExtension ||
                    op == spv::OpExtInstImport || op == spv::OpMemoryModel) {
                    prefix.insert(prefix.end(), w.begin(), w.end());
                    continue;
                }
                if (compute && op == spv::OpTypePointer &&
                    (w[2] == spv::StorageClassInput || w[2] == spv::StorageClassOutput))
                    w[2] = spv::StorageClassPrivate;
                if (compute && op == spv::OpVariable && l.interfaces.count(original.result))
                    w[3] = spv::StorageClassPrivate;
                declarations.insert(declarations.end(), w.begin(), w.end());
            }
            if (source[1] >= 0x10400) {
                for (auto global : l.globals) {
                    // Replay has fresh index builtins and never reads the old
                    // vertex inputs. Listing both creates duplicate builtins.
                    if (!compute && l.defs.at(global).w[3] == spv::StorageClassInput)
                        continue;
                    if (std::find(entry_interfaces.begin(), entry_interfaces.end(), global) ==
                        entry_interfaces.end())
                        entry_interfaces.push_back(global);
                }
                entry_interfaces.push_back(b.scratch);
                entry_interfaces.push_back(b.inputs);
            }
            Words entry_args{compute ? spv::ExecutionModelGLCompute : spv::ExecutionModelVertex,
                             wrapper};
            const auto name = string_words("main");
            entry_args.insert(entry_args.end(), name.begin(), name.end());
            entry_args.insert(entry_args.end(), entry_interfaces.begin(), entry_interfaces.end());
            put(prefix, spv::OpEntryPoint, entry_args);
            if (compute)
                put(prefix, spv::OpExecutionMode, {wrapper, spv::ExecutionModeLocalSize, 64, 1, 1});
            for (const auto &i : l.ins)
                if (i.op() == spv::OpExecutionMode || i.op() == spv::OpExecutionModeId) {
                    if (i.w[2] != spv::ExecutionModeDenormPreserve &&
                        i.w[2] != spv::ExecutionModeDenormFlushToZero &&
                        i.w[2] != spv::ExecutionModeSignedZeroInfNanPreserve &&
                        i.w[2] != spv::ExecutionModeRoundingModeRTE &&
                        i.w[2] != spv::ExecutionModeRoundingModeRTZ)
                        throw std::runtime_error("unsupported vertex execution mode");
                    auto mode = i.w;
                    mode[1] = wrapper;
                    prefix.insert(prefix.end(), mode.begin(), mode.end());
                }
            Words code(source.begin(), source.begin() + 5);
            code[3] = b.next;
            for (const auto *section : {&prefix, &annotations, &b.annotations, &declarations,
                                        &b.declarations, &functions, &b.body})
                code.insert(code.end(), section->begin(), section->end());
            std::string error;
            if (!validate(code, error))
                throw std::runtime_error(std::string(compute ? "compute: " : "replay: ") + error);
            (compute ? result.compute : result.replay) = std::move(code);
        }
        return true;
    } catch (const std::exception &e) {
        result.error = e.what();
        result.compute.clear();
        result.replay.clear();
        return false;
    }
}
} // namespace vkfix

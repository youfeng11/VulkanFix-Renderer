#include "shader_rewriter.h"
#include <cstring>
#include <map>
#include <set>
#include <spirv/unified1/spirv.hpp>

namespace {
using Words = std::vector<uint32_t>;
void emit(Words& out, spv::Op op, std::initializer_list<uint32_t> operands) {
    out.push_back((uint32_t(operands.size() + 1) << 16) | uint32_t(op));
    out.insert(out.end(), operands);
}
}

bool ShaderRewriter::flip_position_y(Words& code, const char* entry_point, uint32_t model) {
    if (code.size() < 5 || code[0] != 0x07230203u ||
        (model != spv::ExecutionModelVertex && model != spv::ExecutionModelTessellationEvaluation &&
         model != spv::ExecutionModelGeometry)) return false;
    if (!validate(code, 0x00010300u)) return false;
    struct Inst { size_t at; uint32_t wc, op, function; };
    std::vector<Inst> instructions;
    std::map<uint32_t, size_t> types;
    std::set<uint32_t> positions, interfaces;
    std::map<uint32_t, uint32_t> members;
    std::map<uint32_t, std::set<uint32_t>> calls;
    uint32_t function = 0, entry = 0;
    for (size_t at = 5; at < code.size();) {
        const uint32_t wc = code[at] >> 16, op = code[at] & 0xffff;
        if (!wc || at + wc > code.size()) return false;
        const auto* w = code.data() + at;
        if (op == spv::OpFunction && wc >= 5) function = w[2];
        instructions.push_back({at, wc, op, function});
        if (op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer && wc >= 2)
            types[w[1]] = at;
        if (op == spv::OpDecorate && wc == 4 && w[2] == spv::DecorationBuiltIn &&
            w[3] == spv::BuiltInPosition) positions.insert(w[1]);
        if (op == spv::OpMemberDecorate && wc == 5 && w[3] == spv::DecorationBuiltIn &&
            w[4] == spv::BuiltInPosition) members[w[1]] = w[2];
        if (op == spv::OpFunctionCall && wc >= 4) calls[function].insert(w[3]);
        if (op == spv::OpEntryPoint && wc >= 4 && w[1] == model) {
            const auto* name = reinterpret_cast<const char*>(w + 3);
            const auto* end = static_cast<const char*>(std::memchr(name, 0, (wc - 3) * 4));
            if (!end) return false;
            if (!std::strcmp(name, entry_point ? entry_point : "main")) {
                entry = w[2];
                const uint32_t first = 3 + uint32_t(end - name) / 4 + 1;
                interfaces.insert(w + first, w + wc);
            }
        }
        if (op == spv::OpFunctionEnd) function = 0;
        at += wc;
    }
    if (!entry) return false;
    // BuiltIn decorations may legally be applied through decoration groups.
    for (const auto& inst : instructions) {
        const auto* w = code.data() + inst.at;
        if (inst.op == spv::OpGroupDecorate && inst.wc >= 3 && positions.count(w[1]))
            positions.insert(w + 2, w + inst.wc);
        if (inst.op == spv::OpGroupMemberDecorate && inst.wc >= 4 && positions.count(w[1]))
            for (uint32_t i = 2; i + 1 < inst.wc; i += 2) members[w[i]] = w[i + 1];
    }
    uint32_t variable = 0, vector = 0, member = UINT32_MAX;
    for (const auto& inst : instructions) {
        const auto* w = code.data() + inst.at;
        if (inst.op != spv::OpVariable || inst.wc < 4 ||
            w[3] != spv::StorageClassOutput || !interfaces.count(w[2])) continue;
        const auto pointer = types.find(w[1]);
        if (pointer == types.end()) return false;
        const auto* ptr = code.data() + pointer->second;
        if ((ptr[0] & 0xffff) != spv::OpTypePointer) return false;
        uint32_t output = ptr[3], position_member = UINT32_MAX;
        if (!positions.count(w[2])) {
            const auto m = members.find(output);
            if (m == members.end()) continue;
            const auto st = types.find(output);
            if (st == types.end()) return false;
            const auto* structure = code.data() + st->second;
            if ((structure[0] & 0xffff) != spv::OpTypeStruct ||
                m->second + 2 >= (structure[0] >> 16)) return false;
            output = structure[2 + m->second]; position_member = m->second;
        }
        if (variable) return false; // multiple rasterization streams need separate selection
        variable = w[2]; vector = output; member = position_member;
    }
    if (!variable) return false;
    const auto vt = types.find(vector);
    if (vt == types.end()) return false;
    const auto* v = code.data() + vt->second;
    if ((v[0] & 0xffff) != spv::OpTypeVector || (v[0] >> 16) != 4 || v[3] != 4) return false;
    const uint32_t scalar = v[2];
    const auto ft = types.find(scalar);
    if (ft == types.end() || (code[ft->second] & 0xffff) != spv::OpTypeFloat) return false;
    std::set<uint32_t> reachable{entry};
    bool added;
    do {
        added = false;
        for (const auto& call : calls) if (reachable.count(call.first))
            for (auto target : call.second) added |= reachable.insert(target).second;
    } while (added);
    Words declarations;
    uint32_t bound = code[3], pointer_type = 0, index = 0;
    if (member != UINT32_MAX) {
        // Reuse types when present: duplicate non-aggregate type declarations are invalid.
        for (const auto& t : types) {
            const auto* w = code.data() + t.second;
            if ((w[0] & 0xffff) == spv::OpTypePointer && w[2] == spv::StorageClassOutput &&
                w[3] == vector) pointer_type = t.first;
        }
        if (!pointer_type) {
            pointer_type = bound++;
            emit(declarations, spv::OpTypePointer, {pointer_type, spv::StorageClassOutput, vector});
        }
        uint32_t integer = 0;
        for (const auto& t : types) {
            const auto* w = code.data() + t.second;
            if ((w[0] & 0xffff) == spv::OpTypeInt && w[2] == 32 && w[3] == 0) integer = t.first;
        }
        if (!integer) { integer = bound++; emit(declarations, spv::OpTypeInt, {integer, 32, 0}); }
        index = bound++; emit(declarations, spv::OpConstant, {integer, index, member});
    }
    Words output(code.begin(), code.begin() + 5);
    bool inserted_declarations = false, changed = false;
    for (const auto& inst : instructions) {
        const auto* w = code.data() + inst.at;
        if (!inserted_declarations && inst.op == spv::OpFunction) {
            output.insert(output.end(), declarations.begin(), declarations.end());
            inserted_declarations = true;
        }
        const bool boundary = model == spv::ExecutionModelGeometry ?
            reachable.count(inst.function) &&
                (inst.op == spv::OpEmitVertex || inst.op == spv::OpEmitStreamVertex) :
            inst.function == entry && inst.op == spv::OpReturn;
        uint32_t pointer = variable, original = 0;
        if (boundary) {
            if (member != UINT32_MAX) {
                pointer = bound++;
                emit(output, spv::OpAccessChain, {pointer_type, pointer, variable, index});
            }
            original = bound++;
            const uint32_t y = bound++, negated = bound++, flipped = bound++;
            emit(output, spv::OpLoad, {vector, original, pointer});
            emit(output, spv::OpCompositeExtract, {scalar, y, original, 1});
            emit(output, spv::OpFNegate, {scalar, negated, y});
            emit(output, spv::OpCompositeInsert, {vector, flipped, negated, original, 1});
            emit(output, spv::OpStore, {pointer, flipped});
            changed = true;
        }
        output.insert(output.end(), w, w + inst.wc);
        // Geometry outputs become undefined after emission. Restoring the saved
        // value also prevents our transform accumulating across repeated emits.
        if (boundary && model == spv::ExecutionModelGeometry)
            emit(output, spv::OpStore, {pointer, original});
    }
    if (!changed) return false;
    output[3] = bound;
    if (!validate(output, 0x00010300u)) return false;
    code = std::move(output);
    return true;
}

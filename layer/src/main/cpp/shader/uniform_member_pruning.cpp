#include "shader/uniform_member_pruning.h"

#include <algorithm>
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>

namespace vkfix {
namespace {
enum class Scan { None, LargeUniform, Invalid };

Scan scan(const uint32_t* words, size_t size) {
    if (!words || size < 5 || words[0] != spv::MagicNumber)
        return Scan::Invalid;
    std::vector<uint32_t> large_structs;
    for (size_t i = 5; i < size;) {
        const size_t count = words[i] >> 16;
        const auto op = static_cast<spv::Op>(words[i] & 0xffff);
        if (!count || count > size - i)
            return Scan::Invalid;
        if (op == spv::OpTypeStruct && count > 258)
            large_structs.push_back(words[i + 1]);
        i += count;
    }
    if (large_structs.empty())
        return Scan::None;
    std::vector<uint32_t> blocks;
    for (size_t i = 5; i < size; i += words[i] >> 16) {
        const size_t count = words[i] >> 16;
        const auto op = static_cast<spv::Op>(words[i] & 0xffff);
        if (op == spv::OpDecorate && count >= 3 && words[i + 2] == spv::DecorationBlock &&
            std::find(large_structs.begin(), large_structs.end(), words[i + 1]) != large_structs.end())
            blocks.push_back(words[i + 1]);
    }
    for (size_t i = 5; i < size; i += words[i] >> 16) {
        const auto op = static_cast<spv::Op>(words[i] & 0xffff);
        if (op == spv::OpTypePointer && (words[i] >> 16) == 4 &&
            words[i + 2] == spv::StorageClassUniform &&
            std::find(blocks.begin(), blocks.end(), words[i + 3]) != blocks.end())
            return Scan::LargeUniform;
    }
    return Scan::None;
}

spv_target_env environment(uint32_t version) {
    if (version >= 0x10600) return SPV_ENV_VULKAN_1_3;
    if (version >= 0x10500) return SPV_ENV_VULKAN_1_2;
    if (version >= 0x10300) return SPV_ENV_VULKAN_1_1;
    return SPV_ENV_VULKAN_1_0;
}
} // namespace

bool has_large_uniform_block(const uint32_t* words, size_t word_count) {
    return scan(words, word_count) == Scan::LargeUniform;
}

UniformPruneResult prune_unused_uniform_members(std::vector<uint32_t>& code,
                                                uint32_t max_spirv_version,
                                                std::string& error) {
    error.clear();
    const auto candidate = scan(code.data(), code.size());
    if (candidate == Scan::Invalid) {
        error = "Malformed SPIR-V module";
        return UniformPruneResult::Invalid;
    }
    if (candidate == Scan::None)
        return UniformPruneResult::Unchanged;

    const auto env = environment(max_spirv_version);
    auto message = [&](spv_message_level_t, const char*, const spv_position_t&, const char* text) {
        if (error.empty() && text) error.assign(text, std::min<size_t>(std::strlen(text), 4096));
    };
    spvtools::Optimizer optimizer(env);
    optimizer.SetMessageConsumer(message);
    optimizer.RegisterPass(spvtools::CreateEliminateDeadMembersPass());
    std::vector<uint32_t> rewritten;
    if (!optimizer.Run(code.data(), code.size(), &rewritten)) {
        if (error.empty()) error = "Unused uniform member elimination failed";
        return UniformPruneResult::Invalid;
    }
    spvtools::SpirvTools validator(env);
    validator.SetMessageConsumer(message);
    if (!validator.Validate(rewritten)) {
        if (error.empty()) error = "Pruned uniform module failed validation";
        return UniformPruneResult::Invalid;
    }
    error.clear();
    if (rewritten == code)
        return UniformPruneResult::Unchanged;
    code.swap(rewritten);
    return UniformPruneResult::Changed;
}
} // namespace vkfix

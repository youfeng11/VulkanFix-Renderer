#include "specialize_constants.h"
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>

namespace vkfix {
bool specialize_constants(std::vector<uint32_t>& code, const VkSpecializationInfo* info,
                          std::string& error) {
    error.clear();
    auto message=[&](spv_message_level_t, const char*, const spv_position_t&, const char* text) {
        error=text;
    };
    spvtools::SpirvTools validator(SPV_ENV_UNIVERSAL_1_6);
    validator.SetMessageConsumer(message);
    if (!validator.Validate(code)) return false;
    std::vector<uint32_t> flat;
    spvtools::Optimizer flatten(SPV_ENV_UNIVERSAL_1_6);
    flatten.SetMessageConsumer(message);
    flatten.RegisterPass(spvtools::CreateCompactIdsPass());
    flatten.RegisterPass(spvtools::CreateFlattenDecorationPass());
    if (!flatten.Run(code.data(),code.size(),&flat)) return false;
    std::map<uint32_t,size_t> type_sizes, constant_types, spec_sizes;
    std::map<uint32_t,uint32_t> ids;
    for (size_t at=5;at<flat.size();) {
        const auto* w=flat.data()+at;
        const uint32_t wc=w[0]>>16, op=w[0]&0xffff;
        if (op==spv::OpTypeBool) type_sizes[w[1]]=sizeof(VkBool32);
        else if (op==spv::OpTypeInt || op==spv::OpTypeFloat) type_sizes[w[1]]=(w[2]+7)/8;
        else if (op==spv::OpSpecConstant || op==spv::OpSpecConstantTrue || op==spv::OpSpecConstantFalse)
            constant_types[w[2]]=w[1];
        else if (op==spv::OpDecorate && wc==4 && w[2]==spv::DecorationSpecId) ids[w[1]]=w[3];
        at+=wc;
    }
    for (const auto& id:ids) {
        auto constant=constant_types.find(id.first);
        if (constant!=constant_types.end()) spec_sizes[id.second]=type_sizes[constant->second];
    }
    std::unordered_map<uint32_t,std::vector<uint32_t>> values;
    if (info) {
        if ((info->mapEntryCount && !info->pMapEntries) || (info->dataSize && !info->pData)) {
            error="missing specialization storage"; return false;
        }
        std::set<uint32_t> seen;
        for (uint32_t i=0;i<info->mapEntryCount;++i) {
            const auto& entry=info->pMapEntries[i];
            if (!seen.insert(entry.constantID).second || entry.offset>=info->dataSize ||
                entry.size>info->dataSize-entry.offset) {
                error="invalid specialization map range or duplicate constantID"; return false;
            }
            auto size=spec_sizes.find(entry.constantID);
            if (size==spec_sizes.end()) continue;
            if (!size->second || entry.size!=size->second) {
                error="specialization entry size does not match the scalar type"; return false;
            }
            auto& value=values[entry.constantID];
            value.resize((entry.size+3)/4,0);
            // pData and offset need not be aligned for a scalar access.
            std::memcpy(value.data(),static_cast<const uint8_t*>(info->pData)+entry.offset,entry.size);
        }
    }
    spvtools::Optimizer freeze(SPV_ENV_UNIVERSAL_1_6);
    freeze.SetMessageConsumer(message);
    freeze.RegisterPass(spvtools::CreateSetSpecConstantDefaultValuePass(values));
    freeze.RegisterPass(spvtools::CreateFreezeSpecConstantValuePass());
    freeze.RegisterPass(spvtools::CreateFoldSpecConstantOpAndCompositePass());
    std::vector<uint32_t> result;
    if (!freeze.Run(flat.data(),flat.size(),&result) || !validator.Validate(result)) return false;
    code=std::move(result);
    return true;
}
}

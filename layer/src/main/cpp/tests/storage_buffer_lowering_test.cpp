#include "shader/shader_rewriter.h"
#include "shader/storage_buffer_lowering.h"
#include "shader/specialize_constants.h"
#include "core/shader_module_cache.h"
#include "core/specialized_shader_stages.h"
#include "storage_buffer_fixture.h"
#include "diagnostics/driver_post_shaders.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <cstring>
#include <spirv/unified1/spirv.hpp>
#include <spirv-tools/libspirv.hpp>
namespace {
std::set<uint32_t> array_lengths(const std::vector<uint32_t>& code) {
    std::map<uint32_t,uint32_t> constants;
    std::set<uint32_t> lengths;
    for (size_t at=5;at<code.size();at+=code[at]>>16) {
        const auto* w=code.data()+at;
        if ((w[0]&0xffff)==spv::OpConstant && (w[0]>>16)==4) constants[w[2]]=w[3];
        if ((w[0]&0xffff)==spv::OpTypeArray) lengths.insert(constants[w[3]]);
    }
    return lengths;
}
std::map<VkShaderModule,std::vector<uint32_t>> private_modules;
unsigned next_private=0, fail_private=0, destroyed_private=0;
VKAPI_ATTR VkResult VKAPI_CALL private_create(VkDevice device,const VkShaderModuleCreateInfo* ci,
                                  const VkAllocationCallbacks* allocator,VkShaderModule* module) {
    if (allocator) return VK_ERROR_UNKNOWN;
    ++next_private;
    if (next_private==fail_private) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *module=(VkShaderModule)(uintptr_t)(0x7000+next_private);
    private_modules[*module]={ci->pCode,ci->pCode+ci->codeSize/4};
    vkfix::ShaderModuleCache::get().remember(device,*module,ci);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL private_destroy(VkDevice device,VkShaderModule module,const VkAllocationCallbacks*) {
    if (private_modules.erase(module)) ++destroyed_private;
    vkfix::ShaderModuleCache::get().forget(device,module);
}
}
int main() {
    setenv("VULKAN_FIX_SHADER_CACHE_PATH", "/dev/null", 1);
    unsigned failures = 0;
    auto check = [&](bool okay, const char* label) {
        printf("%s: %s\n", okay ? "PASS" : "FAIL", label); failures += !okay;
    };
    spvtools::SpirvTools modern(SPV_ENV_VULKAN_1_1), legacy(SPV_ENV_VULKAN_1_0);
    auto messages = [](spv_message_level_t, const char*, const spv_position_t& pos, const char* text) {
        printf("SPIR-V word %zu: %s\n", pos.index, text);
    };
    modern.SetMessageConsumer(messages); legacy.SetMessageConsumer(messages);
    for (unsigned mode : {0u,1u,2u}) for (bool grouped : {false, true}) for (bool extension : {false, true}) {
        std::vector<uint32_t> code;
        auto& source = extension ? legacy : modern;
        check(source.Assemble(storage_buffer_fixture(grouped, extension, mode>0, mode==2), &code) && source.Validate(code),
              "shared UBO/SSBO block, helper, descriptor array, CopyMemory and atomics fixture is legal");
        auto native=code;
        const auto version=native[1];
        std::string native_error;
        const auto native_result=vkfix::normalize_storage_buffer_arrays(native,native_error);
        if (native_result==vkfix::StorageBufferLowering::Invalid) printf("Native normalization: %s\n",native_error.c_str());
        check(native_result==(mode? vkfix::StorageBufferLowering::Changed : vkfix::StorageBufferLowering::Unchanged) &&
              source.Validate(native) && native[1]==version && vkfix::has_storage_buffer_class(native.data(),native.size()),
              "modern normalization preserves the original version and StorageBuffer class");
        const auto normalized=native;
        check(vkfix::normalize_storage_buffer_arrays(native,native_error)==vkfix::StorageBufferLowering::Unchanged &&
              native==normalized, "modern normalization is idempotent");
        const bool rewritten = ShaderRewriter::get().rewrite(code, 0x10000);
        if (!rewritten) {
            auto debug = code; std::string error;
            auto result = vkfix::lower_storage_buffer_class(debug, error);
            printf("Lowering result=%u error=%s\n", unsigned(result), error.c_str());
            debug[1] = 0x10000; legacy.Validate(debug);
        }
        check(rewritten && legacy.Validate(code) && code[1] == 0x10000 &&
                  !vkfix::has_storage_buffer_class(code.data(), code.size()),
              "Vulkan 1.0 downgrade preserves UBO while replacing every StorageBuffer pointer");
        const auto once = code;
        check(!ShaderRewriter::get().rewrite(code, 0x10000) && code == once,
              "a fully lowered module is unchanged on repeated processing");
    }
    std::vector<uint32_t> plain(post_dispatch_vector_comp,
        post_dispatch_vector_comp + sizeof(post_dispatch_vector_comp) / 4);
    check(ShaderRewriter::get().rewrite(plain, 0x10000) && legacy.Validate(plain),
          "ordinary storage-buffer compute shader validates against Vulkan 1.0");
    spvtools::SpirvTools later(SPV_ENV_VULKAN_1_2);
    auto text=storage_buffer_fixture(false,false,true);
    auto where=text.find("OpEntryPoint GLCompute %main \"main\"");
    text.replace(where, std::string("OpEntryPoint GLCompute %main \"main\"").size(),
                 "OpEntryPoint GLCompute %main \"main\" %ubo %a %b %array");
    where=text.find("OpCopyMemory %b %ubo Volatile|Aligned|Nontemporal 16");
    text.replace(where, std::string("OpCopyMemory %b %ubo Volatile|Aligned|Nontemporal 16").size(), "OpCopyMemory %b %ubo");
    std::vector<uint32_t> version14;
    check(later.Assemble(text, &version14), "SPIR-V 1.4 entry-interface fixture assembles");
    version14[1]=0x10400;
    auto native14=version14;
    std::string error14;
    check(later.Validate(native14) &&
          vkfix::normalize_storage_buffer_arrays(native14,error14)==vkfix::StorageBufferLowering::Changed &&
          later.Validate(native14) && native14[1]==0x10400,
          "modern normalization preserves SPIR-V 1.4 global entry-point interfaces");
    check(later.Validate(version14) && ShaderRewriter::get().rewrite(version14, 0x10000) && legacy.Validate(version14),
          "storage-class conversion composes with SPIR-V 1.4 interface lowering");
    auto readonly_text=storage_buffer_fixture(false,false,true);
    const std::string alias="OpDecorate %array Aliased";
    readonly_text.insert(readonly_text.find(alias),"OpDecorate %array NonWritable\n");
    for (const auto line : {"OpStore %lastField %zero\n", "OpStore %array %whole\n"}) {
        auto at=readonly_text.find(line); readonly_text.erase(at,std::string(line).size());
    }
    auto at=readonly_text.find("%ptrS %arrayA %two");
    readonly_text.replace(at,std::string("%ptrS %arrayA %two").size(),"%ptrS %a %two");
    at=readonly_text.find("%ptrS %arrayB %one");
    readonly_text.replace(at,std::string("%ptrS %arrayB %one").size(),"%ptrS %b %one");
    for (bool restrict_array : {false,true}) {
        auto source_text=readonly_text;
        if (restrict_array) source_text.replace(source_text.find(alias),alias.size(),"OpDecorate %array Restrict");
        std::vector<uint32_t> readonly;
        std::string error;
        check(modern.Assemble(source_text,&readonly) && modern.Validate(readonly) &&
              vkfix::normalize_storage_buffer_arrays(readonly,error)==vkfix::StorageBufferLowering::Changed &&
              modern.Validate(readonly), "read-only and Restrict descriptor-array declarations remain valid");
        if (!error.empty()) printf("Readonly normalization: %s\n",error.c_str());
    }
    auto static_text=storage_buffer_fixture(false,false,true);
    auto erase_line=[&](std::string& input,const std::string& line) { input.erase(input.find(line),line.size()); };
    erase_line(static_text,"OpCapability StorageBufferArrayDynamicIndexing\n");
    erase_line(static_text,"OpDecorate %arrayLength SpecId 7\n");
    const std::string spec="%arrayLength = OpSpecConstant %uint 2";
    static_text.replace(static_text.find(spec),spec.size(),"%arrayLength = OpConstant %uint 3");
    const std::string last="%lastBlock = OpAccessChain %ptrSBlock %array %lastIndex";
    static_text.replace(static_text.find(last),last.size(),"%lastBlock = OpAccessChain %ptrSBlock %array %two");
    std::vector<uint32_t> static_array;
    std::string static_error;
    check(modern.Assemble(static_text,&static_array) && modern.Validate(static_array) &&
          vkfix::normalize_storage_buffer_arrays(static_array,static_error)==vkfix::StorageBufferLowering::Changed &&
          modern.Validate(static_array),"fixed descriptor arrays remain legal without dynamic indexing capability");
    if (!static_error.empty()) printf("Static array normalization: %s\n",static_error.c_str());
    auto deferred_text=storage_buffer_fixture(false,false,true);
    erase_line(deferred_text,"OpCapability StorageBufferArrayDynamicIndexing\n");
    deferred_text.replace(deferred_text.find(last),last.size(),"%lastBlock = OpAccessChain %ptrSBlock %array %one");
    std::vector<uint32_t> deferred;
    check(modern.Assemble(deferred_text,&deferred) && modern.Validate(deferred),
          "a specialized array can be used without enabling dynamic descriptor indexing");
    const auto deferred_original=deferred;
    check(vkfix::normalize_storage_buffer_arrays(deferred,static_error)==vkfix::StorageBufferLowering::NeedsSpecialization &&
          deferred==deferred_original,"a valid shader needing pipeline-stage processing is preserved without adding a capability");
    uint8_t spec_data[9]{};
    const uint32_t three=3, four=4;
    memcpy(spec_data+1,&three,4); memcpy(spec_data+5,&four,4);
    VkSpecializationMapEntry spec_entry{7,1,4};
    VkSpecializationInfo spec_info{1,&spec_entry,sizeof spec_data,spec_data};
    for (uint32_t length:{2u,3u,4u}) {
        auto specialized=deferred_original;
        auto map=spec_entry; map.offset=length==4?5:1;
        auto info=spec_info; info.pMapEntries=&map;
        std::string error;
        check(vkfix::specialize_constants(specialized,length==2?nullptr:&info,error) &&
              array_lengths(specialized)==std::set<uint32_t>{length} &&
              vkfix::normalize_storage_buffer_arrays(specialized,error)==vkfix::StorageBufferLowering::Changed &&
              modern.Validate(specialized) && ShaderRewriter::get().rewrite(specialized,0x10000) &&
              legacy.Validate(specialized),
              "actual or default specialization freezes the array length before static modern/legacy conversion");
        if (!error.empty()) printf("Specialization: %s\n",error.c_str());
    }
    auto deferred_sparse=deferred_original; deferred_sparse[3]=0x3fffff;
    check(modern.Validate(deferred_sparse) && vkfix::specialize_constants(deferred_sparse,&spec_info,static_error) &&
          deferred_sparse[3]<0x3fffff && array_lengths(deferred_sparse)==std::set<uint32_t>{3} &&
          modern.Validate(deferred_sparse),"specialization compacts a valid sparse ID space before folding derived constants");
    auto invalid_spec=deferred_original;
    auto invalid_entry=spec_entry; invalid_entry.offset=UINT32_MAX;
    auto invalid_info=spec_info; invalid_info.pMapEntries=&invalid_entry;
    check(!vkfix::specialize_constants(invalid_spec,&invalid_info,static_error) &&
          invalid_spec==deferred_original,"out-of-range specialization cannot read data or modify source");
    auto unused_entry=spec_entry; unused_entry.constantID=999;
    auto unused_info=spec_info; unused_info.pMapEntries=&unused_entry;
    auto unused_spec=deferred_original;
    check(vkfix::specialize_constants(unused_spec,&unused_info,static_error) &&
          array_lengths(unused_spec)==std::set<uint32_t>{2},"unused constantIDs preserve shader defaults");
    std::vector<uint32_t> scalar_specs;
    check(modern.Assemble(R"(OpCapability Shader
OpCapability Int16
OpCapability Int64
OpCapability Float64
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%specGroup = OpDecorationGroup
OpDecorate %specGroup SpecId 8
OpGroupDecorate %specGroup %boolean
OpDecorate %narrow SpecId 9
OpDecorate %wide SpecId 10
OpDecorate %single SpecId 11
OpDecorate %double SpecId 12
%void = OpTypeVoid
%bool = OpTypeBool
%i16 = OpTypeInt 16 1
%u64 = OpTypeInt 64 0
%f32 = OpTypeFloat 32
%f64 = OpTypeFloat 64
%fn = OpTypeFunction %void
%boolean = OpSpecConstantFalse %bool
%narrow = OpSpecConstant %i16 1
%wide = OpSpecConstant %u64 2
%single = OpSpecConstant %f32 0
%double = OpSpecConstant %f64 0
%main = OpFunction %void None %fn
%entry = OpLabel
OpReturn
OpFunctionEnd
)",&scalar_specs) && modern.Validate(scalar_specs),"scalar specialization fixture includes grouped SpecId, bool, int16 and uint64");
    uint8_t scalar_data[27]{};
    const uint32_t bool_value=0x80000000;
    const uint16_t narrow_value=0xff83;
    const uint64_t wide_value=0xfedcba9876543210ull;
    const uint32_t single_value=0x7fc12345;
    const uint64_t double_value=0x8000000000000000ull;
    memcpy(scalar_data+1,&bool_value,4); memcpy(scalar_data+5,&narrow_value,2); memcpy(scalar_data+7,&wide_value,8);
    memcpy(scalar_data+15,&single_value,4); memcpy(scalar_data+19,&double_value,8);
    VkSpecializationMapEntry scalar_entries[5]{{8,1,4},{9,5,2},{10,7,8},{11,15,4},{12,19,8}};
    VkSpecializationInfo scalar_info{5,scalar_entries,sizeof scalar_data,scalar_data};
    const bool scalar_ok=vkfix::specialize_constants(scalar_specs,&scalar_info,static_error);
    bool boolean_ok=false,narrow_ok=false,wide_ok=false,single_ok=false,double_ok=false,spec_remains=false;
    if (scalar_ok) for (size_t pos=5;pos<scalar_specs.size();pos+=scalar_specs[pos]>>16) {
        const auto* w=scalar_specs.data()+pos;
        const auto op=w[0]&0xffff, wc=w[0]>>16;
        boolean_ok |= op==spv::OpConstantTrue;
        narrow_ok |= op==spv::OpConstant && wc==4 && w[3]==0xffffff83;
        wide_ok |= op==spv::OpConstant && wc==5 && w[3]==0x76543210 && w[4]==0xfedcba98;
        single_ok |= op==spv::OpConstant && wc==4 && w[3]==single_value;
        double_ok |= op==spv::OpConstant && wc==5 && w[3]==0 && w[4]==0x80000000;
        spec_remains |= op==spv::OpSpecConstant || op==spv::OpSpecConstantTrue || op==spv::OpSpecConstantFalse;
    }
    check(scalar_ok && modern.Validate(scalar_specs) && boolean_ok && narrow_ok && wide_ok && single_ok && double_ok && !spec_remains,
          "unaligned specialization preserves boolean truth, signed narrow/64-bit literals, NaN payload and negative zero");
    const auto mock_device=(VkDevice)(uintptr_t)0x1234;
    const auto original_module=(VkShaderModule)(uintptr_t)0x1235;
    VkShaderModuleCreateInfo original_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    original_ci.pCode=deferred_original.data(); original_ci.codeSize=deferred_original.size()*4;
    vkfix::ShaderModuleCache::get().remember(mock_device,original_module,&original_ci,true);
    VkComputePipelineCreateInfo compute[2]{};
    auto four_entry=spec_entry; four_entry.offset=5;
    auto four_info=spec_info; four_info.pMapEntries=&four_entry;
    for (auto& ci:compute) {
        ci.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=original_module;
        ci.stage.pName="main"; ci.stage.pSpecializationInfo=&spec_info;
    }
    compute[1].stage.pSpecializationInfo=&four_info;
    compute[1].basePipelineIndex=0; compute[1].flags=VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    {
        vkfix::SpecializedShaderStages stages(mock_device,private_create,private_destroy);
        check(stages.prepare(2,compute)==VK_SUCCESS,"one source module supports two compute pipeline specializations");
        const auto* patched=stages.compute(compute);
        check(patched!=compute && patched[0].stage.module!=patched[1].stage.module &&
              !patched[0].stage.pSpecializationInfo && !patched[1].stage.pSpecializationInfo &&
              array_lengths(private_modules.at(patched[0].stage.module))==std::set<uint32_t>{3} &&
              array_lengths(private_modules.at(patched[1].stage.module))==std::set<uint32_t>{4} &&
              patched[1].basePipelineIndex==0 && patched[1].flags==compute[1].flags &&
              compute[0].stage.module==original_module && compute[0].stage.pSpecializationInfo==&spec_info,
              "batch specialization keeps caller memory and derivative indices intact");
    }
    check(private_modules.empty() && destroyed_private==2,"temporary modules are released after pipeline processing");
    VkPipelineShaderStageCreateInfo graphics_stages[2]{compute[0].stage,compute[1].stage};
    graphics_stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; graphics_stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    graphics.stageCount=2; graphics.pStages=graphics_stages;
    {
        vkfix::SpecializedShaderStages stages(mock_device,private_create,private_destroy);
        check(stages.prepare(1,&graphics)==VK_SUCCESS && stages.graphics(&graphics)->pStages!=graphics_stages &&
              stages.graphics(&graphics)->pStages[0].stage==VK_SHADER_STAGE_VERTEX_BIT &&
              stages.graphics(&graphics)->pStages[1].stage==VK_SHADER_STAGE_FRAGMENT_BIT,
              "graphics stages use independent private modules without changing stage identity");
    }
    fail_private=next_private+2;
    {
        vkfix::SpecializedShaderStages stages(mock_device,private_create,private_destroy);
        check(stages.prepare(2,compute)==VK_ERROR_OUT_OF_DEVICE_MEMORY,"private shader creation failures propagate");
    }
    check(private_modules.empty() && destroyed_private==5,
          "partial preparation failure frees earlier private modules");
    auto retained=vkfix::ShaderModuleCache::get().acquire_deferred(mock_device,original_module);
    check(retained && *retained==deferred_original,"pipeline creation never replaces the unspecialized source");
    vkfix::ShaderModuleCache::get().forget(mock_device,original_module);
    vkfix::ShaderModuleCache::get().remember(mock_device,original_module,&original_ci);
    check(!vkfix::ShaderModuleCache::get().acquire_deferred(mock_device,original_module) &&
          retained && *retained==deferred_original,"reused module handles do not inherit deferred metadata");
    vkfix::ShaderModuleCache::get().forget_device(mock_device);
    std::vector<uint32_t> payload;
    check(modern.Assemble(R"(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %Block Block
OpMemberDecorate %Block 0 Offset 0
OpDecorate %Array ArrayStride 4
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 0
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%three = OpConstant %uint 3
%Array = OpTypeArray %uint %three
%Block = OpTypeStruct %Array
%ptrBlock = OpTypePointer StorageBuffer %Block
%ptrArray = OpTypePointer StorageBuffer %Array
%fn = OpTypeFunction %void
%buffer = OpVariable %ptrBlock StorageBuffer
%main = OpFunction %void None %fn
%entry = OpLabel
%pointer = OpAccessChain %ptrArray %buffer %zero
%value = OpLoad %Array %pointer
OpStore %pointer %value
OpReturn
OpFunctionEnd
)",&payload) && modern.Validate(payload),"ordinary arrays inside a buffer are legal without descriptor-indexing capability");
    const auto payload_original=payload;
    check(vkfix::normalize_storage_buffer_arrays(payload,static_error)==vkfix::StorageBufferLowering::Unchanged &&
          payload==payload_original,"ordinary buffer payload arrays are not mistaken for descriptor arrays");
    std::vector<uint32_t> sparse;
    check(modern.Assemble(storage_buffer_fixture(false,false,true),&sparse),"sparse-bound fixture assembles");
    sparse[3]=0x3fffff;
    check(modern.Validate(sparse) &&
          vkfix::normalize_storage_buffer_arrays(sparse,static_error)==vkfix::StorageBufferLowering::Changed &&
          sparse[3]<0x3fffff && modern.Validate(sparse),
          "a valid large ID bound is compacted before allocating transformation IDs");
    std::vector<uint32_t> unused;
    check(legacy.Assemble(R"(OpCapability Shader
OpExtension "SPV_KHR_storage_buffer_storage_class"
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
OpReturn
OpFunctionEnd
)", &unused) && legacy.Validate(unused) && ShaderRewriter::get().rewrite(unused, 0x10000) &&
        legacy.Validate(unused) && !vkfix::has_storage_buffer_class(unused.data(), unused.size()),
        "an unused modern extension is removed even when the header is already SPIR-V 1.0");
    std::string error;
    auto damaged = plain; damaged.pop_back(); const auto saved = damaged;
    check(vkfix::lower_storage_buffer_class(damaged, error) == vkfix::StorageBufferLowering::Invalid && damaged == saved,
          "malformed input is rejected transactionally");
    printf("Storage-buffer lowering failures: %u\n", failures);
    return failures ? 1 : 0;
}

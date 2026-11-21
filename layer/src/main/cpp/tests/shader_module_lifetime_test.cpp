#include "core/created_shader_module.h"
#include "core/device_object_key.h"
#include "core/shader_module_cache.h"
#include "core/specialized_shader_stages.h"
#include "diagnostics/driver_post_shaders.h"
#include "shader/tessellation_domain_origin.h"
#include "shader/specialize_constants.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <set>

static std::atomic<bool> fail_new{false};
void* operator new(size_t size) {
    if (fail_new.exchange(false)) throw std::bad_alloc();
    if (auto p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
using namespace vkfix;
namespace {
template<class T> T handle(uint64_t value) { return (T)(uintptr_t)value; }
using Key = std::pair<uint64_t,uint64_t>;
Key key(VkDevice device, VkShaderModule module) { return {uint64_t(uintptr_t(device)),DeviceObjectKey::make(device,module).handle}; }
unsigned failures = 0, calls = 0, created = 0, destroyed = 0, invalid_destroy = 0, serial = 0x10000;
unsigned fail_call = 0;
bool null_success = false, fail_after_create = false, allocator_matches = true;
const VkAllocationCallbacks* expected_allocator = nullptr;
std::set<Key> live, metadata;
bool capture_code = false;
std::vector<uint32_t> last_code;
void check(bool okay, const char* label) { printf("%s: %s\n",okay ? "PASS" : "FAIL",label); failures += !okay; }
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice device, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* allocator, VkShaderModule* out) {
    ++calls; allocator_matches &= allocator == expected_allocator;
    if (calls == fail_call) { *out = handle<VkShaderModule>(0xdead); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    if (null_success) { *out = VK_NULL_HANDLE; return VK_SUCCESS; }
    if (capture_code) last_code.assign(info->pCode, info->pCode + info->codeSize / 4);
    *out = handle<VkShaderModule>(++serial); live.insert(key(device,*out)); ++created;
    if (fail_after_create) fail_new = true;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice device, VkShaderModule module, const VkAllocationCallbacks* allocator) {
    allocator_matches &= allocator == expected_allocator;
    invalid_destroy += live.erase(key(device,module)) != 1; ++destroyed;
    metadata.erase(key(device,module)); ShaderModuleCache::get().forget(device,module);
}
void reset_faults() { calls = fail_call = 0; null_success = fail_after_create = false; expected_allocator = nullptr; }
VKAPI_ATTR void* VKAPI_CALL allocate(void*,size_t bytes,size_t,VkSystemAllocationScope) { return std::malloc(bytes); }
VKAPI_ATTR void* VKAPI_CALL reallocate(void*,void* p,size_t bytes,size_t,VkSystemAllocationScope) { return std::realloc(p,bytes); }
VKAPI_ATTR void VKAPI_CALL free_memory(void*,void* p) { std::free(p); }
}
int main() {
    const auto device = handle<VkDevice>(0x100);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = sizeof(post_draw_vert); ci.pCode = post_draw_vert;
    const auto original_created = created, original_destroyed = destroyed;
    reset_faults(); fail_call = 1;
    {
        CreatedShaderModule module(device,destroy);
        check(module.create(create,&ci) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !module.get(),
            "native failure with non-null garbage output is never adopted");
    }
    check(created == original_created && destroyed == original_destroyed && !invalid_destroy,
        "failed native output is never passed to destroy");
    reset_faults(); null_success = true;
    {
        CreatedShaderModule module(device,destroy);
        check(module.create(create,&ci) == VK_ERROR_INITIALIZATION_FAILED && !module.get(),
            "success with null native handle cannot publish a private shader");
    }
    VkAllocationCallbacks allocator{}; allocator.pfnAllocation = allocate; allocator.pfnReallocation = reallocate; allocator.pfnFree = free_memory;
    reset_faults(); expected_allocator = &allocator;
    VkShaderModule released{};
    {
        CreatedShaderModule module(device,destroy,&allocator);
        check(module.create(create,&ci) == VK_SUCCESS && module.get(),"adopt a native shader created with caller allocation callbacks");
        released = module.release(); check(!module.get() && live.count(key(device,released)),"ownership transfer leaves the native module live");
    }
    destroy(device,released,&allocator);
    check(live.empty() && allocator_matches && !invalid_destroy,"ownership transfer and explicit retirement use the matching allocator exactly once");

    // Exercise actual shader-cache allocations after native creation and a
    // successful first metadata registration, as in the public layer entry.
    reset_faults(); expected_allocator = &allocator;
    VkShaderModule registered{}; bool threw = false;
    try {
        CreatedShaderModule module(device,destroy,&allocator);
        check(module.create(create,&ci) == VK_SUCCESS,"create before metadata fault injection"); registered = module.get();
        metadata.insert(key(device,registered)); ShaderModuleCache::get().remember(device,registered,&ci);
        fail_new = true; ShaderModuleCache::get().remember(device,registered,&ci,true,true);
    } catch (const std::bad_alloc&) { threw = true; }
    check(threw && !fail_new && live.empty() && metadata.empty() && !ShaderModuleCache::get().acquire(device,registered),
        "metadata OOM retires the successfully created module and all earlier registrations");

    const auto vertex = handle<VkShaderModule>(0x400), fragment = handle<VkShaderModule>(0x500);
    ShaderModuleCache::get().remember(device,vertex,&ci,true);
    auto fragment_ci = ci; fragment_ci.codeSize = sizeof(post_draw_frag); fragment_ci.pCode = post_draw_frag;
    ShaderModuleCache::get().remember(device,fragment,&fragment_ci,true);
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (unsigned i = 0; i < 2; ++i) { stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; stages[i].module = i ? fragment : vertex; stages[i].pName = "main"; }
    VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; graphics.stageCount = 2; graphics.pStages = stages;
    reset_faults(); fail_call = 2;
    const auto before_failure = destroyed;
    {
        SpecializedShaderStages prepared(device,create,destroy);
        check(prepared.prepare(1,&graphics) == VK_ERROR_OUT_OF_DEVICE_MEMORY && live.size() == 1,
            "failure on the second specialized stage retains only the successful first stage for cleanup");
    }
    check(live.empty() && destroyed == before_failure+1 && !invalid_destroy && stages[0].module == vertex && stages[1].module == fragment,
        "partial stage preparation destroys exactly one owned module and preserves caller stage definitions");
    reset_faults(); fail_after_create = true; threw = false;
    const auto before_host_failure = destroyed;
    try {
        SpecializedShaderStages prepared(device,create,destroy);
        prepared.prepare(1,&graphics);
    } catch (const std::bad_alloc&) { threw = true; }
    check(threw && !fail_new && live.empty() && destroyed == before_host_failure+1 && !invalid_destroy,
        "host allocation failure while publishing a private module rolls back native success");
    reset_faults(); null_success = true;
    {
        SpecializedShaderStages prepared(device,create,destroy);
        check(prepared.prepare(1,&graphics) == VK_ERROR_INITIALIZATION_FAILED,
            "specialized stage preparation rejects a null handle returned with success");
    }
    reset_faults();
    {
        SpecializedShaderStages prepared(device,create,destroy);
        check(prepared.prepare(1,&graphics) == VK_SUCCESS && live.size() == 2 &&
            prepared.graphics(&graphics)->pStages[0].module != vertex && prepared.graphics(&graphics)->pStages[1].module != fragment,
            "retry prepares independent native shaders for both stages");
    }
    check(live.empty() && metadata.empty() && created == destroyed && !invalid_destroy && allocator_matches,
        "all successful modules are retired once and no failed output reaches cleanup");
    check(ShaderModuleCache::get().acquire_deferred(device,vertex) && ShaderModuleCache::get().acquire_deferred(device,fragment),
        "temporary module retirement does not erase original shader-cache entries");
    const auto evaluation = handle<VkShaderModule>(0x600);
    std::vector<uint32_t> evaluation_code(post_viewport_stage_tese, post_viewport_stage_tese + sizeof post_viewport_stage_tese / 4);
    const auto original_evaluation = evaluation_code;
    auto reversed_evaluation = evaluation_code;
    check(lower_tessellation_domain_origin(reversed_evaluation, "main") && reversed_evaluation != original_evaluation &&
          lower_tessellation_domain_origin(reversed_evaluation, "main") && reversed_evaluation == original_evaluation,
          "domain origin reverses TES winding without changing coordinates or other instructions");
    auto missing_entry = original_evaluation;
    check(!lower_tessellation_domain_origin(missing_entry, "missing") && missing_entry == original_evaluation,
          "a missing TES entry point leaves shader bytes unchanged");
    VkShaderModuleCreateInfo evaluation_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    evaluation_ci.pCode = evaluation_code.data(); evaluation_ci.codeSize = evaluation_code.size() * 4;
    ShaderModuleCache::get().remember(device, evaluation, &evaluation_ci);
    VkPipelineShaderStageCreateInfo evaluation_stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    evaluation_stage.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; evaluation_stage.module = evaluation; evaluation_stage.pName = "main";
    VkPipelineTessellationDomainOriginStateCreateInfo origin{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO};
    origin.domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
    VkPipelineTessellationStateCreateInfo tessellation{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, &origin};
    tessellation.patchControlPoints = 3;
    VkGraphicsPipelineCreateInfo domain_pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    domain_pipeline.stageCount = 1; domain_pipeline.pStages = &evaluation_stage; domain_pipeline.pTessellationState = &tessellation;
    const auto saved_origin = origin; const auto saved_tessellation = tessellation;
    reset_faults(); capture_code = true;
    {
        SpecializedShaderStages prepared(device,create,destroy,std::nullopt,false,false);
        check(prepared.prepare(1,&domain_pipeline) == VK_SUCCESS && live.size() == 1,
              "legacy domain origin creates one owned TES variant");
        const auto* lowered = prepared.graphics(&domain_pipeline);
        check(lowered->pTessellationState != &tessellation && !lowered->pTessellationState->pNext &&
              lowered->pTessellationState->patchControlPoints == 3 && lowered->pStages[0].module != evaluation &&
              !std::memcmp(&origin,&saved_origin,sizeof origin) && !std::memcmp(&tessellation,&saved_tessellation,sizeof tessellation),
              "legacy pipeline strips only its owned domain-origin node and preserves caller inputs");
        auto specialized_original = original_evaluation;
        std::string specialization_error;
        check(specialize_constants(specialized_original,nullptr,specialization_error) &&
              lower_tessellation_domain_origin(last_code,"main") && last_code == specialized_original,
              "the actual private module contains the reversed winding");
    }
    check(live.empty() && created == destroyed && !invalid_destroy, "domain variants are retired after pipeline preparation");
    {
        SpecializedShaderStages prepared(device,create,destroy,std::nullopt,false,true);
        check(prepared.prepare(1,&domain_pipeline) == VK_SUCCESS && prepared.graphics(&domain_pipeline) == &domain_pipeline && live.empty(),
              "an authorized native maintenance2 route receives the original origin and shader");
    }
    origin.domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT;
    {
        SpecializedShaderStages prepared(device,create,destroy,std::nullopt,false,false);
        check(prepared.prepare(1,&domain_pipeline) == VK_SUCCESS && !prepared.graphics(&domain_pipeline)->pTessellationState->pNext &&
              prepared.graphics(&domain_pipeline)->pStages[0].module == evaluation && live.empty(),
              "legacy upper-left default removes the unsupported node without rewriting its shader");
    }
    origin.domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
    reset_faults(); fail_call = 1;
    {
        SpecializedShaderStages prepared(device,create,destroy,std::nullopt,false,false);
        check(prepared.prepare(1,&domain_pipeline) == VK_ERROR_OUT_OF_DEVICE_MEMORY && live.empty() && !invalid_destroy,
              "domain variant creation failure propagates without adopting a failed handle");
    }
    capture_code = false;
    ShaderModuleCache::get().forget_device(device);
    printf("Shader module lifetime failures: %u\n",failures); return failures ? 1 : 0;
}

#include "shader/subgroup_basic_lowering.h"
#include "shader/shader_rewriter.h"
#include "subgroup_basic_fixture.h"
#include "subgroup_legacy_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <spirv-tools/libspirv.hpp>
int main() {
    setenv("VULKAN_FIX_SHADER_CACHE_PATH","/dev/null",1);
    unsigned failures=0;
    auto check=[&](bool valid,const char* text) { printf("%s: %s\n",valid?"PASS":"FAIL",text); failures+=!valid; };
    spvtools::SpirvTools source(SPV_ENV_VULKAN_1_1),target(SPV_ENV_VULKAN_1_0);
    auto message=[](spv_message_level_t,const char*,const spv_position_t& pos,const char* text) { printf("word %zu: %s\n",pos.index,text); };
    source.SetMessageConsumer(message); target.SetMessageConsumer(message);
    for (bool grouped:{false,true}) for (bool existing:{false,true})
        for (bool spec:{false,true}) for (bool divergent:{false,true}) for (bool signed_types:{false,true}) {
        std::vector<uint32_t> code;
        const bool legal=source.Assemble(subgroup_basic_fixture(grouped,existing,spec,divergent,signed_types),&code) && source.Validate(code);
        check(legal,"BASIC builtins, pointer copy/helper, barriers, atomics and workgroup-size fixture is legal");
        if (!legal) continue;
        std::string error;
        auto result=vkfix::lower_basic_subgroups(code,error);
        if (!error.empty()) printf("Lowering: %s\n",error.c_str());
        check(result==vkfix::BasicSubgroupLowering::Changed && source.Validate(code) &&
              ShaderRewriter::get().rewrite(code,0x10000) && target.Validate(code),
              "one-invocation subgroup lowering validates as Vulkan 1.0");
        const auto once=code;
        check(vkfix::lower_basic_subgroups(code,error)==vkfix::BasicSubgroupLowering::Unchanged && code==once,
              "lowering is idempotent");
    }
    for (bool signed_types:{false,true}) {
        std::vector<uint32_t> code; std::string error;
        const bool legal=source.Assemble(subgroup_basic_multi_entry(signed_types),&code) && source.Validate(code);
        check(legal,"one module can have compute entries with different local sizes");
        if (legal) check(vkfix::lower_basic_subgroups(code,error)==vkfix::BasicSubgroupLowering::Changed &&
            source.Validate(code) && ShaderRewriter::get().rewrite(code,0x10000) && target.Validate(code),
            "per-entry initialization and signed input copies validate in a shared module");
        if (!error.empty()) printf("Multi-entry: %s\n",error.c_str());
    }
    // Vulkan memory model does not imply device-scope support. The BASIC
    // fallback must not require the VulkanMemoryModelDeviceScope capability.
    auto model=subgroup_basic_fixture();
    const std::string memory="OpMemoryModel Logical GLSL450";
    model.replace(model.find(memory),memory.size(),
        "OpCapability VulkanMemoryModelKHR\nOpExtension \"SPV_KHR_vulkan_memory_model\"\nOpMemoryModel Logical VulkanKHR");
    std::vector<uint32_t> memory_code; std::string memory_error;
    const bool model_legal=source.Assemble(model,&memory_code) && source.Validate(memory_code);
    check(model_legal,"subgroup memory operations are legal without device-scope capability");
    if (model_legal) check(vkfix::lower_basic_subgroups(memory_code,memory_error)==vkfix::BasicSubgroupLowering::Changed &&
        source.Validate(memory_code),"compute memory scopes preserve Vulkan memory model feature requirements");
    if (!memory_error.empty()) printf("Memory model: %s\n",memory_error.c_str());
    auto no_memory=subgroup_basic_fixture();
    const std::string barrier="OpControlBarrier %subgroup %subgroup %wgSemantics";
    no_memory.replace(no_memory.find(barrier),barrier.size(),"OpControlBarrier %subgroup %subgroup %zero");
    std::vector<uint32_t> no_memory_code; std::string no_memory_error;
    const bool barrier_legal=source.Assemble(no_memory,&no_memory_code) && source.Validate(no_memory_code);
    check(barrier_legal,"a subgroup execution barrier can have no memory semantics");
    if (barrier_legal) check(vkfix::lower_basic_subgroups(no_memory_code,no_memory_error)==vkfix::BasicSubgroupLowering::Changed &&
        ShaderRewriter::get().rewrite(no_memory_code,0x10000) && target.Validate(no_memory_code),
        "an execution-only single-invocation barrier does not create an invalid relaxed memory barrier");
    // A function variable must remain before the inserted invocation setup.
    auto copy=subgroup_basic_fixture();
    copy.insert(copy.find("%fn = OpTypeFunction"),"%ptrFunction = OpTypePointer Function %uint\n");
    const std::string label="%entry = OpLabel\n";
    copy.insert(copy.find(label)+label.size(),"%copySlot = OpVariable %ptrFunction Function\n");
    const std::string read="%copyRead = OpLoad %uint %copied";
    copy.replace(copy.find(read),read.size(),
        "OpCopyMemory %copySlot %copied Volatile|Aligned|Nontemporal 4\n%copyRead = OpLoad %uint %copySlot");
    std::vector<uint32_t> copy_code; std::string copy_error;
    const bool copy_legal=source.Assemble(copy,&copy_code) && source.Validate(copy_code);
    check(copy_legal,"builtin input can be copied into a function local with memory-access operands");
    if (copy_legal) check(vkfix::lower_basic_subgroups(copy_code,copy_error)==vkfix::BasicSubgroupLowering::Changed &&
        ShaderRewriter::get().rewrite(copy_code,0x10000) && target.Validate(copy_code),
        "invocation initialization follows function locals and private-copy alignment is weakened safely");
    if (!copy_error.empty()) printf("CopyMemory: %s\n",copy_error.c_str());
    auto sparse=memory_code;
    // Use a fresh source so a large bound reaches the transformation itself.
    check(source.Assemble(subgroup_basic_fixture(),&sparse),"sparse subgroup module assembles");
    sparse[3]=0x3fffff;
    std::string sparse_error;
    check(source.Validate(sparse) && vkfix::lower_basic_subgroups(sparse,sparse_error)==vkfix::BasicSubgroupLowering::Changed &&
          sparse[3]<0x3fffff && source.Validate(sparse),"a sparse valid ID bound is compacted before adding subgroup state");
    std::vector<uint32_t> broken;
    source.Assemble(subgroup_basic_fixture(),&broken); broken.pop_back();
    const auto saved=broken;
    check(vkfix::lower_basic_subgroups(broken,sparse_error)==vkfix::BasicSubgroupLowering::Invalid && broken==saved,
          "malformed subgroup code is rejected transactionally");
    for (bool fragment:{false,true}) {
        std::vector<uint32_t> legacy_code; std::string error;
        const bool legal=target.Assemble(subgroup_legacy_fixture(fragment),&legacy_code) && target.Validate(legacy_code);
        check(legal,"legacy ballot/vote shader including input masks is legal Vulkan 1.0");
        if (legal) check(vkfix::lower_basic_subgroups(legacy_code,error)==vkfix::BasicSubgroupLowering::Changed &&
            target.Validate(legacy_code),"legacy compute/fragment subgroup operations and masks lower to plain Shader capability");
        if (!error.empty()) printf("Legacy subgroup: %s\n",error.c_str());
    }
    for (const std::string operation:{"AtomicLoad","AtomicStore","AtomicExchange","AtomicCompareExchange",
         "AtomicIIncrement","AtomicIDecrement","AtomicIAdd","AtomicISub","AtomicSMin","AtomicUMin",
         "AtomicSMax","AtomicUMax","AtomicAnd","AtomicOr","AtomicXor","AtomicFAddEXT"}) {
        const bool floating=operation=="AtomicFAddEXT",store=operation=="AtomicStore";
        const bool unary=operation=="AtomicLoad" || operation=="AtomicIIncrement" || operation=="AtomicIDecrement";
        std::string instruction=store?"OpAtomicStore %pointer %scope %zero %one":
            "%result = Op"+operation+" "+(floating?"%float":"%uint")+" %pointer %scope %zero";
        if (!store) {
            if (operation=="AtomicCompareExchange") instruction+=" %zero %one %zero";
            else if (!unary) instruction+=floating?" %fOne":" %one";
        }
        const bool signed_type=operation=="AtomicSMin" || operation=="AtomicSMax";
        std::vector<uint32_t> atomic_code; std::string error;
        const bool legal=source.Assemble(subgroup_atomic_fixture(instruction,floating,signed_type),&atomic_code) && source.Validate(atomic_code);
        check(legal,(operation+" source is legal").c_str());
        if (legal) check(vkfix::lower_basic_subgroups(atomic_code,error)==vkfix::BasicSubgroupLowering::Changed &&
            ShaderRewriter::get().rewrite(atomic_code,0x10000) && target.Validate(atomic_code),
            (operation+" scope lowers without changing value/result operands").c_str());
        if (!error.empty()) printf("Atomic subgroup: %s\n",error.c_str());
    }
    printf("Basic subgroup failures: %u\n",failures);
    return failures?1:0;
}

#include "diagnostics/core11_requirements.h"
#include <algorithm>
#include <cstdio>
using namespace vkfix::post;
int main() {
    unsigned failures = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s: %s\n", ok ? "PASS" : "FAIL", message); failures += !ok;
    };
    Core11RequiredState state;
    state.graphics = state.compute = true;
    state.base.robustBufferAccess = VK_TRUE;
    state.multiview.multiview = VK_TRUE;
    state.views.maxMultiviewViewCount = 6;
    state.views.maxMultiviewInstanceIndex = (1u << 27) - 1;
    state.subgroup.subgroupSize = 1;
    state.subgroup.supportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
    state.subgroup.supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
    state.maintenance.maxPerSetDescriptors = 1024;
    state.maintenance.maxMemoryAllocationSize = VkDeviceSize(1) << 30;
    check(missing_core11_requirements(state).empty(), "minimum Core 1.1 requirements allow absent optional features");
    auto bad = state; bad.multiview.multiview = VK_FALSE;
    auto missing = missing_core11_requirements(bad);
    check(missing.size() == 1 && missing[0] == "multiview", "missing mandatory multiview is a failure, not a skipped optional probe");
    bad = state; bad.views.maxMultiviewViewCount = 5;
    check(!missing_core11_requirements(bad).empty(), "five multiview layers do not meet the minimum of six");
    bad = state; --bad.views.maxMultiviewInstanceIndex;
    check(!missing_core11_requirements(bad).empty(), "multiview instance index minimum is enforced");
    for (auto size : {0u, 3u}) {
        bad = state; bad.subgroup.subgroupSize = size;
        check(!missing_core11_requirements(bad).empty(), "zero and non-power-of-two subgroups cannot pass");
    }
    bad = state; bad.subgroup.supportedOperations = 0;
    check(!missing_core11_requirements(bad).empty(), "subgroup BASIC is mandatory");
    bad = state; bad.subgroup.supportedStages = VK_SHADER_STAGE_FRAGMENT_BIT;
    check(!missing_core11_requirements(bad).empty(), "compute queues require compute subgroup support");
    bad = state; --bad.maintenance.maxPerSetDescriptors;
    check(!missing_core11_requirements(bad).empty(), "descriptor limit below 1024 fails");
    bad = state; --bad.maintenance.maxMemoryAllocationSize;
    check(!missing_core11_requirements(bad).empty(), "memory allocation limit below 1 GiB fails");
    bad = state; bad.storage.uniformAndStorageBuffer16BitAccess = VK_TRUE;
    check(!missing_core11_requirements(bad).empty(), "16-bit feature implication is enforced");
    bad = state; bad.draw_extension = true;
    check(!missing_core11_requirements(bad).empty(), "advertising shader draw parameters requires the feature");
    printf("Core 1.1 requirement failures: %u\n", failures);
    return failures ? 1 : 0;
}

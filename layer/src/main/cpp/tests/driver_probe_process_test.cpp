#include "core/driver_probe_process.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
int main(int argc,char **argv) {
    if (argc!=2) return 1;
    setenv("VULKAN_FIX_DRIVER_PROBE_WORKER",argv[1],1);
    unsigned failures=0;
    auto check=[&](bool pass,const char *name) { std::printf("%s: %s\n",pass?"PASS":"FAIL",name); failures+=!pass; };
    using S=vkfix::ProbeProcessStatus; using V=vkfix::DriverProbeVerdict;
    VkPhysicalDeviceProperties properties{};
    auto run=[&](const char *fault) {
        setenv("VKFIX_TEST_PROCESS_FAULT",fault,1);
        return vkfix::isolated_push_descriptor_probe(properties,1);
    };
    check(run("healthy").verdict==V::Healthy,"healthy isolated subject preserves native route");
    check(run("subject-mismatch").verdict==V::RepairConfirmed,"readback failure with both controls selects emulation");
    check(run("subject-crash").verdict==V::RepairConfirmed,"subject crash is contained and both controls establish repair");
    check(run("control-mismatch").verdict==V::Inconclusive,"failed control establishes no defect");
    check(run("setup-crash").verdict==V::Inconclusive,"setup crash establishes no transition defect");
    check(run("bad-protocol").verdict==V::Inconclusive,"exit zero without valid protocol is not a pass");
    setenv("VKFIX_TEST_PROCESS_FAULT","hang",1);
    auto result=vkfix::run_driver_probe_process("push-subject","test",100);
    check(result.status==S::TimedOut,"hung child is bounded and terminated independently of caller");
    setenv("VKFIX_TEST_PROCESS_FAULT","closed-output-hang",1);
    check(vkfix::run_driver_probe_process("push-subject","test",100).status==S::TimedOut,"closed stdout cannot bypass bounded child waiting");
    setenv("VKFIX_TEST_PROCESS_FAULT","flood",1);
    check(vkfix::run_driver_probe_process("push-subject","test",100).status==S::TimedOut,"unbounded worker output cannot starve deadline checks");
    setenv("VKFIX_TEST_PROCESS_FAULT","healthy",1);
    int streams[]{dup(0),dup(1),dup(2)};
    close(0); close(1); close(2);
    const auto no_streams=vkfix::run_driver_probe_process("push-subject","test");
    for (unsigned i=0;i<3;++i) { dup2(streams[i],i); close(streams[i]); }
    check(no_streams.status==S::Passed,"worker protocol works when parent standard descriptors are closed");
    setenv("VULKAN_FIX_DRIVER_PROBE_WORKER","/no/such/vulkanfix-probe",1);
    check(vkfix::run_driver_probe_process("push-subject","test").status==S::Unavailable,"missing worker is inconclusive");
    std::printf("Driver probe process regression: %u failure(s)\n",failures);
    return failures ? 1 : 0;
}

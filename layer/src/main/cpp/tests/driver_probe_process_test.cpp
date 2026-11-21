#include "core/driver_probe_process.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {
VKAPI_ATTR void VKAPI_CALL get_properties(VkPhysicalDevice, VkPhysicalDeviceProperties *properties) {
    properties->vendorID = 0x1234;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_gipa(VkInstance, const char *name) {
    if (!std::strcmp(name, "vkGetPhysicalDeviceProperties"))
        return reinterpret_cast<PFN_vkVoidFunction>(get_properties);
    return nullptr;
}
}

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
    check(run("pass-without-ready").verdict==V::Inconclusive,"pass token without completed setup is rejected");
    check(run("graphics-subject-mismatch").verdict==V::RepairConfirmed,"compute success cannot hide graphics readback failure");
    check(run("graphics-subject-crash").verdict==V::RepairConfirmed,"graphics subject crash is contained and controlled");
    check(run("graphics-control-mismatch").verdict==V::Inconclusive,"failed graphics control does not blame native push");
    check(run("vertex-subject-mismatch").verdict==V::RepairConfirmed,"fragment success cannot hide a vertex descriptor failure");
    check(run("vertex-subject-crash").verdict==V::RepairConfirmed,"vertex subject crash is contained and controlled");
    check(run("vertex-control-mismatch").verdict==V::Inconclusive,"failed vertex control cannot establish repair");
    check(run("stage-mask-subject-mismatch").verdict==V::RepairConfirmed,"compute-visible vertex resources have an independent measured gate");
    setenv("VKFIX_TEST_PROCESS_FAULT","repair",1);
    auto assessment=vkfix::isolated_driver_assessment("sampler",properties,1);
    check(assessment.verdict==V::RepairConfirmed && assessment.detail=="controlled repair","completed differential worker publishes verdict and evidence");
    setenv("VKFIX_TEST_PROCESS_FAULT","setup-crash",1);
    check(vkfix::isolated_driver_assessment("uniform",properties,1).verdict==V::Inconclusive,"isolated compiler/setup crash cannot authorize shader repair");
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
    setenv("VKFIX_PROBE_CHILD", "1", 1);
    const auto nested = vkfix::run_driver_probe_process("push-subject", "test");
    unsetenv("VKFIX_PROBE_CHILD");
    check(nested.status == S::Unavailable && nested.detail == "nested isolated probe suppressed",
          "a worker cannot start nested probes through a forwarding driver");
    const vkfix::NativeDriverProbes probes(reinterpret_cast<VkInstance>(1), reinterpret_cast<VkPhysicalDevice>(1), fake_gipa);
    const auto sampler = probes.measure(vkfix::DriverProbe::SamplerAliases);
    setenv("VKFIX_TEST_PROCESS_FAULT", "repair", 1);
    const auto uniform = probes.measure(vkfix::DriverProbe::LargeUniform);
    const auto sampler_again = probes.measure(vkfix::DriverProbe::SamplerAliases);
    check(sampler.verdict == V::Healthy && uniform.verdict == V::RepairConfirmed &&
          sampler_again.verdict == V::Healthy, "each feature measures lazily and caches its own result");
    auto timeline = [&](uintptr_t physical, const char *fault) {
        setenv("VKFIX_TEST_PROCESS_FAULT", fault, 1);
        const vkfix::NativeDriverProbes queue_probes(
            reinterpret_cast<VkInstance>(1), reinterpret_cast<VkPhysicalDevice>(physical), fake_gipa);
        return queue_probes.measure(vkfix::DriverProbe::TimelineQueue);
    };
    check(timeline(2, "healthy").verdict == V::Healthy, "timeline queue data reaches the shared controller");
    check(timeline(3, "subject-mismatch").verdict == V::DefectObserved,
          "timeline queue readback fails with passing binary controls");
    check(timeline(4, "subject-crash").verdict == V::DefectObserved,
          "timeline queue subject crash is isolated and compared with binary controls");
    check(timeline(5, "control-mismatch").verdict == V::Inconclusive,
          "failed binary control cannot establish a timeline defect");
    check(timeline(3, "healthy").verdict == V::DefectObserved,
          "repeated timeline selection reuses the measured result");
    setenv("VKFIX_TEST_PROCESS_FAULT", "subject-mismatch", 1);
    check(probes.measure(vkfix::DriverProbe::Sync2Submit).verdict == V::DefectObserved,
          "submit2 readback failure is retained with passing legacy queue controls");
    setenv("VKFIX_TEST_PROCESS_FAULT", "control-mismatch", 1);
    check(probes.measure(vkfix::DriverProbe::Sync2Events).verdict == V::Inconclusive,
          "event control failure does not inherit an independently observed submit2 defect");
    setenv("VKFIX_TEST_PROCESS_FAULT", "known-mismatch", 1);
    check(probes.measure(vkfix::DriverProbe::DepthStencilResolve).verdict == V::DefectObserved &&
          probes.measure(vkfix::DriverProbe::E5BufferTransfer).verdict == V::DefectObserved,
          "diagnostic-only depth resolve and packed transfer findings report observed defects");
    check(probes.measure(vkfix::DriverProbe::CrossFormatImageSlices).verdict == V::RepairConfirmed,
          "an available per-slice copy repair still requires the recognized failure and working controls");
    auto events = [&](uintptr_t physical, const char *fault) {
        setenv("VKFIX_TEST_PROCESS_FAULT", fault, 1);
        const vkfix::NativeDriverProbes event_probes(
            reinterpret_cast<VkInstance>(1), reinterpret_cast<VkPhysicalDevice>(physical), fake_gipa);
        return event_probes.measure(vkfix::DriverProbe::Sync2Events);
    };
    const auto repaired = events(6, "known-mismatch");
    check(repaired.verdict == V::RepairConfirmed && repaired.detail.find("legacy reset with native set/wait") != std::string::npos,
          "event reset repair requires the mixed native set/wait and legacy reset controls");
    const auto failed_reset_control = events(7, "reset-control-mismatch");
    check(failed_reset_control.verdict == V::DefectObserved &&
          failed_reset_control.detail.find("reset-only repair unconfirmed") != std::string::npos,
          "failed mixed control preserves the event defect without enabling the repair");
    check(events(8, "subject-mismatch").verdict == V::DefectObserved,
          "an unspecified event readback failure does not enable the reset-only repair");
    setenv("VULKAN_FIX_DRIVER_PROBE_WORKER","/no/such/vulkanfix-probe",1);
    check(vkfix::run_driver_probe_process("push-subject","test").status==S::Unavailable,"missing worker is inconclusive");
    std::printf("Driver probe process regression: %u failure(s)\n",failures);
    return failures ? 1 : 0;
}

#include "driver_post_trust.h"
#include "../core/driver_probe_process.h"
#include "../core/driver_trust.h"

namespace vkfix {
namespace {
using V = DriverProbeVerdict;
using S = ProbeProcessStatus;

struct TrustCheck {
    const char *name;
    DriverProbe probe;
    const char *confirmed_status = "WARN";
    const char *passed_detail = nullptr;
};
constexpr TrustCheck trust_checks[]{
    {"driverTrust.samplerAliases", DriverProbe::SamplerAliases},
    {"driverTrust.largeUniform", DriverProbe::LargeUniform},
    {"driverTrust.pushDescriptor", DriverProbe::PushDescriptor},
    {"driverTrust.tilerPressure", DriverProbe::TilerBudget},
    {"driverTrust.msaaArrayResolve", DriverProbe::MsaaArrayResolve},
    {"driverTrust.crossFormatImageSlices", DriverProbe::CrossFormatImageSlices, "FAIL",
        "packed UINT array to UNORM volume copied all eight slices and preserved surrounding texels"},
    {"driverTrust.rasterizerDiscardState", DriverProbe::RasterizerDiscardState, "FAIL"},
    {"driverTrust.storageArrayStatic", DriverProbe::StorageArrayStatic},
    {"driverTrust.storageArrayDynamic", DriverProbe::StorageArrayDynamic},
    {"driverTrust.depthStencilResolve", DriverProbe::DepthStencilResolve, "FAIL"},
    {"driverTrust.rgb9e5TransferBuffer", DriverProbe::E5BufferTransfer, "FAIL"},
    {"driverTrust.rgb9e5TransferImage", DriverProbe::E5ImageTransfer, "FAIL"},
    {"driverTrust.queue2", DriverProbe::Queue2},
    {"driverTrust.timelineQueue", DriverProbe::TimelineQueue, "FAIL",
        "queued wait-before-signal, host release, two queue signals and 64 transferred words verified"},
    {"driverTrust.sync2Events", DriverProbe::Sync2Events, "WARN",
        "two set/wait/reset cycles verified event state and all event-dependent transfer words"},
    {"driverTrust.sync2Submit", DriverProbe::Sync2Submit, "FAIL",
        "submit2 semaphore linked a GPU fill to a later copy and all 64 transferred words matched"},
    {"driverTrust.timestampQueries", DriverProbe::TimestampQueries},
    {"driverTrust.hostQueryReset", DriverProbe::HostQueryReset, "FAIL",
        "two partial host resets and rewrites matched command-buffer controls and preserved neighboring queries"},
};
} // namespace

void report_driver_trust(VkInstance instance, VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr gipa,
                         DriverPostRow row) {
    const NativeDriverProbes probes(instance, physical, gipa);
    for (const auto &check : trust_checks) {
        const auto result = probes.measure(check.probe);
        const char *status = "SKIP";
        switch (result.verdict) {
        case V::Healthy: status = "PASS"; break;
        case V::RepairConfirmed: status = check.confirmed_status; break;
        case V::DefectObserved: status = "FAIL"; break;
        case V::Inconclusive: break;
        }
        const auto detail = result.verdict == V::Healthy && check.passed_detail ?
            check.passed_detail : std::string(driver_probe_verdict_name(result.verdict)) + ": " + result.detail;
        row(check.name, status, detail, "functional");
    }

    const auto identity = probes.worker_identity();
    const auto unavailable = "native device properties unavailable";
    const auto worker = identity.empty() ? ProbeProcessResult{S::Unavailable, unavailable} :
                                           run_driver_probe_process("push-control", identity);
    row("driverTrust.workerControl", worker.status == S::Passed ? "PASS" :
        worker.status == S::Unavailable ? "SKIP" : "FAIL", worker.detail, "functional");
}
} // namespace vkfix

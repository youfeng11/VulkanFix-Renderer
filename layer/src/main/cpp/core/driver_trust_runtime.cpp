#include "driver_trust.h"
#include "driver_probe_process.h"
#include "driver_trust_shaders.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace vkfix {
namespace {
DriverProbeAssessment measure_push_descriptor(VkInstance instance, VkPhysicalDevice physical,
                                              PFN_vkGetInstanceProcAddr gipa,
                                              const VkPhysicalDeviceProperties &properties) {
    auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    if (!enumerate) return {DriverProbeVerdict::Inconclusive, "native extension query unavailable"};
    uint32_t count = 0;
    if (enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS || count > 65536)
        return {DriverProbeVerdict::Inconclusive, "invalid native extension count"};
    std::vector<VkExtensionProperties> extensions(count);
    if (count && (enumerate(physical, nullptr, &count, extensions.data()) != VK_SUCCESS || count > extensions.size()))
        return {DriverProbeVerdict::Inconclusive, "native extension enumeration failed"};
    const bool supported = std::any_of(extensions.begin(), extensions.begin() + count, [](const auto &entry) {
        return std::strcmp(entry.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0;
    });
    if (!supported)
        return {DriverProbeVerdict::Inconclusive, "native push descriptors unsupported; missing feature uses emulation"};
    return isolated_push_descriptor_probe(properties, nativeprobe::revision);
}

enum class KnownMismatchPolicy { ReportDefect, ConfirmRepair };

DriverProbeAssessment assess_controlled_probe(
    const ProbeProcessResult &result, KnownMismatchPolicy policy = KnownMismatchPolicy::ReportDefect) {
    using S = ProbeProcessStatus;
    using V = DriverProbeVerdict;
    switch (result.status) {
    case S::Passed:
        return {V::Healthy, result.detail};
    case S::KnownMismatch:
        return {policy == KnownMismatchPolicy::ConfirmRepair ? V::RepairConfirmed : V::DefectObserved, result.detail};
    case S::Mismatch:
    case S::Crashed:
        return {V::DefectObserved, result.detail};
    default:
        return {V::Inconclusive, result.detail};
    }
}

DriverProbeAssessment measure_sync2_events(const std::string &identity) {
    const auto result = run_controlled_driver_probe("sync2-events-subject", "sync2-events-control",
                                                   "legacy event", identity);
    if (result.status != ProbeProcessStatus::KnownMismatch) return assess_controlled_probe(result);
    // Keep SetEvent2 and WaitEvents2 in the repair control. Passing the
    // entirely legacy sequence alone does not establish a reset-only repair.
    const auto reset = run_controlled_driver_probe("sync2-events-subject", "sync2-events-reset-control",
                                                  "legacy reset with native set/wait", identity);
    if (reset.status == ProbeProcessStatus::KnownMismatch)
        return assess_controlled_probe(reset, KnownMismatchPolicy::ConfirmRepair);
    return {DriverProbeVerdict::DefectObserved, result.detail + "; reset-only repair unconfirmed: " + reset.detail};
}
} // namespace

NativeDriverProbes::NativeDriverProbes(VkInstance instance, VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr gipa)
    : instance_(instance), physical_(physical), gipa_(gipa) {
    if (!instance_ || !physical_ || !gipa_) return;
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa_(instance_, "vkGetPhysicalDeviceProperties"));
    if (!query) return;
    query(physical_, &properties_);
    identity_ = driver_identity(properties_, nativeprobe::revision);
    identity_.physical_cookie = reinterpret_cast<uintptr_t>(physical_);
    identity_.provider_cookie = reinterpret_cast<uintptr_t>(gipa_);
    valid_ = true;
}

std::string NativeDriverProbes::worker_identity() const {
    return valid_ ? driver_probe_identity(properties_, nativeprobe::revision) : std::string{};
}

DriverProbeAssessment NativeDriverProbes::measure(DriverProbe probe) const {
    static std::array<DriverTrustCache, static_cast<size_t>(DriverProbe::Count)> caches;
    const auto index = static_cast<size_t>(probe);
    if (!valid_ || index >= caches.size()) return {};
    return caches[index].get(identity_, [this, probe]() -> DriverProbeAssessment {
        switch (probe) {
        case DriverProbe::SamplerAliases:
            return isolated_driver_assessment("sampler", properties_, nativeprobe::revision);
        case DriverProbe::LargeUniform:
            return isolated_driver_assessment("uniform", properties_, nativeprobe::revision);
        case DriverProbe::PushDescriptor:
            return measure_push_descriptor(instance_, physical_, gipa_, properties_);
        case DriverProbe::TilerBudget:
            return isolated_tiler_probe(properties_, nativeprobe::revision);
        case DriverProbe::MsaaArrayResolve:
            return isolated_driver_assessment("msaa", properties_, nativeprobe::revision);
        case DriverProbe::CrossFormatImageSlices:
            return assess_controlled_probe(run_controlled_driver_probe("image-slices-subject", "image-slices-control",
                "per-slice copy", worker_identity()), KnownMismatchPolicy::ConfirmRepair);
        case DriverProbe::RasterizerDiscardState: {
            const auto identity = worker_identity();
            auto result = run_controlled_driver_probe("discard-state-subject", "discard-state-reemit",
                "viewport/scissor reissue", identity);
            if (result.status == ProbeProcessStatus::KnownMismatch) {
                const auto control = run_driver_probe_process("discard-state-rasterized", identity);
                if (control.status != ProbeProcessStatus::Passed)
                    return {DriverProbeVerdict::Inconclusive, "rasterized control: " + control.detail};
                result.detail += "; rasterized control passed";
            }
            return assess_controlled_probe(result, KnownMismatchPolicy::ConfirmRepair);
        }
        case DriverProbe::StorageArrayStatic:
            return assess_controlled_probe(run_controlled_driver_probe("storage-array-static-subject",
                "storage-array-static-control", "scalar field store", worker_identity()), KnownMismatchPolicy::ConfirmRepair);
        case DriverProbe::StorageArrayDynamic:
            return assess_controlled_probe(run_controlled_driver_probe("storage-array-dynamic-subject",
                "storage-array-dynamic-control", "scalar field store", worker_identity()), KnownMismatchPolicy::ConfirmRepair);
        case DriverProbe::DepthStencilResolve:
            return assess_controlled_probe(run_controlled_driver_probe("depth-resolve-subject",
                "depth-resolve-control", "shader sample-zero resolve", worker_identity()));
        case DriverProbe::E5BufferTransfer:
            return assess_controlled_probe(run_controlled_driver_probe("e5-buffer-subject",
                "e5-control", "R32_UINT view", worker_identity()));
        case DriverProbe::E5ImageTransfer:
            return assess_controlled_probe(run_controlled_driver_probe("e5-image-subject",
                "e5-control", "R32_UINT view", worker_identity()));
        case DriverProbe::Queue2:
            return assess_controlled_probe(run_controlled_driver_probe("queue2-subject",
                "queue2-control", "legacy queue transfer", worker_identity()), KnownMismatchPolicy::ConfirmRepair);
        case DriverProbe::TimelineQueue:
            return assess_controlled_probe(run_controlled_driver_probe("timeline-subject", "timeline-control",
                "binary semaphore", worker_identity()));
        case DriverProbe::Sync2Events:
            return measure_sync2_events(worker_identity());
        case DriverProbe::Sync2Submit:
            return assess_controlled_probe(run_controlled_driver_probe("sync2-submit-subject", "timeline-control",
                "legacy queue submit", worker_identity()));
        case DriverProbe::TimestampQueries:
            return assess_controlled_probe(run_driver_probe_process("timestamps", worker_identity()));
        case DriverProbe::HostQueryReset:
            return assess_controlled_probe(run_controlled_driver_probe("host-query-reset-subject",
                "host-query-reset-control", "command-buffer reset", worker_identity()));
        case DriverProbe::Count:
            return {};
        }
        return {};
    });
}
} // namespace vkfix

#pragma once
#include "driver_trust.h"

namespace vkfix {
enum class ProbeProcessStatus { Passed, Mismatch, Unavailable, Crashed, TimedOut };
struct ProbeProcessResult {
    ProbeProcessStatus status=ProbeProcessStatus::Unavailable;
    std::string detail;
};
std::string driver_probe_identity(const VkPhysicalDeviceProperties &,uint64_t revision);
// Executes only the bundled worker, without a shell, on its own process/device.
// A stale/missing worker, failed setup or failed control never establishes a bug.
ProbeProcessResult run_driver_probe_process(const char *mode,const std::string &identity,
                                           unsigned timeout_ms=8000);
DriverProbeAssessment isolated_push_descriptor_probe(const VkPhysicalDeviceProperties &,uint64_t revision);
DriverProbeAssessment isolated_tiler_probe(const VkPhysicalDeviceProperties &,uint64_t revision);
// Entry point of the separately executable, APK-packaged native worker.
int driver_trust_worker(const char *mode,const std::string &identity);
} // namespace vkfix

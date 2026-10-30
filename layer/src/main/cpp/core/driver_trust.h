#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace vkfix {
// Trust is per defect, never a blanket verdict about a GPU vendor.
enum class DriverProbeVerdict { Healthy, RepairConfirmed, Inconclusive };
struct DriverProbeAssessment {
    DriverProbeVerdict verdict = DriverProbeVerdict::Inconclusive;
    std::string detail = "not measured";
};
// A failed control does not establish a driver defect or authorize a repair.
DriverProbeAssessment assess_driver_probe(bool control_before, bool subject,
                                         bool control_after);
bool use_measured_repair(const DriverProbeAssessment &, const char *override_value);
// A missing/failed pressure measurement retains bounded rendering. This is a
// conservative fallback, not a claim that an unmeasured device is defective.
bool use_bounded_tiler_guard(const DriverProbeAssessment &,const char *override_value);
const char *driver_probe_verdict_name(DriverProbeVerdict);

struct DriverIdentity {
    uint32_t vendor = 0, device = 0, driver = 0, api = 0;
    std::array<uint8_t, VK_UUID_SIZE> pipeline_uuid{};
    uint64_t probe_revision = 0;
    uintptr_t physical_cookie = 0, provider_cookie = 0;
    bool operator==(const DriverIdentity &other) const;
};
DriverIdentity driver_identity(const VkPhysicalDeviceProperties &, uint64_t revision);
struct DriverIdentityHash { size_t operator()(const DriverIdentity &) const; };
struct DriverTrustReport {
    DriverProbeAssessment sampler_aliases;
    // Further driver-specific probes are migrated independently. An unmeasured
    // field must never be presented as a healthy driver or a confirmed repair.
    DriverProbeAssessment large_uniform;
    DriverProbeAssessment push_descriptor;
    DriverProbeAssessment tiler_budget;
};

// Process-local, bounded single-flight memoization. Never hold the map mutex
// while compiling shaders, submitting GPU work, or waiting on another thread.
class DriverTrustCache {
public:
    explicit DriverTrustCache(size_t capacity = 32) : capacity_(capacity) {}
    DriverTrustReport get(const DriverIdentity &, const std::function<DriverTrustReport()> &);
    size_t size() const;
private:
    struct Entry { std::shared_future<DriverTrustReport> future; std::thread::id owner; };
    size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<DriverIdentity, Entry, DriverIdentityHash> entries_;
};

// Calls native function pointers on a throwaway native device, never VulkanFix
// interception or an application's queues. Used by both POST and game startup.
DriverTrustReport native_driver_trust(VkInstance, VkPhysicalDevice,
                                      PFN_vkGetInstanceProcAddr);
} // namespace vkfix

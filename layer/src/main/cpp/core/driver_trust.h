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
enum class DriverProbeVerdict { Healthy, RepairConfirmed, DefectObserved, Inconclusive };
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
// Process-local single-flight memoization. At capacity, evict the least
// recently used completed result and retain every measurement in flight.
class DriverTrustCache {
public:
    explicit DriverTrustCache(size_t capacity = 32) : capacity_(capacity) {}
    DriverProbeAssessment get(const DriverIdentity &, const std::function<DriverProbeAssessment()> &);
    size_t size() const;
private:
    struct Entry {
        std::shared_future<DriverProbeAssessment> future;
        std::thread::id owner;
        uint64_t last_use;
    };
    size_t capacity_;
    uint64_t access_count_ = 0;
    mutable std::mutex mutex_;
    std::unordered_map<DriverIdentity, Entry, DriverIdentityHash> entries_;
};

enum class DriverProbe { SamplerAliases, LargeUniform, PushDescriptor, TilerBudget, MsaaArrayResolve,
                         CrossFormatImageSlices, RasterizerDiscardState, StorageArrayStatic, StorageArrayDynamic,
                         DepthStencilResolve, E5BufferTransfer, E5ImageTransfer, Queue2,
                         TimelineQueue, Sync2Events, Sync2Submit, TimestampQueries, HostQueryReset, Count };

// Resolve the raw driver identity once. Measure and cache each behavior
// independently so an extension query only runs the probe it depends on.
class NativeDriverProbes {
public:
    NativeDriverProbes(VkInstance, VkPhysicalDevice, PFN_vkGetInstanceProcAddr);
    DriverProbeAssessment measure(DriverProbe) const;
    std::string worker_identity() const;
private:
    VkInstance instance_;
    VkPhysicalDevice physical_;
    PFN_vkGetInstanceProcAddr gipa_;
    VkPhysicalDeviceProperties properties_{};
    DriverIdentity identity_;
    bool valid_ = false;
};
} // namespace vkfix

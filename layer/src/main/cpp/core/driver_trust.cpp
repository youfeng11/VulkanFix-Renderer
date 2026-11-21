#include "driver_trust.h"
#include <cstring>
#include <stdexcept>

namespace vkfix {
DriverProbeAssessment assess_driver_probe(bool before, bool subject, bool after) {
    if (!before || !after) return {DriverProbeVerdict::Inconclusive, "equivalent control did not pass before and after subject"};
    if (subject) return {DriverProbeVerdict::Healthy, "native subject and equivalent controls produced expected values"};
    return {DriverProbeVerdict::RepairConfirmed, "native subject mismatched; equivalent repaired control passed before and after"};
}
bool use_measured_repair(const DriverProbeAssessment &assessment, const char *value) {
    if (value && !std::strcmp(value,"0")) return false;
    if (value && !std::strcmp(value,"1")) return true;
    return assessment.verdict == DriverProbeVerdict::RepairConfirmed;
}
const char *driver_probe_verdict_name(DriverProbeVerdict verdict) {
    switch (verdict) {
    case DriverProbeVerdict::Healthy: return "healthy";
    case DriverProbeVerdict::RepairConfirmed: return "repair-confirmed";
    case DriverProbeVerdict::DefectObserved: return "defect-observed";
    default: return "inconclusive";
    }
}
bool use_bounded_tiler_guard(const DriverProbeAssessment &assessment,const char *value) {
    if (value && !std::strcmp(value,"0")) return false;
    if (value && !std::strcmp(value,"1")) return true;
    return assessment.verdict!=DriverProbeVerdict::Healthy;
}
bool DriverIdentity::operator==(const DriverIdentity &other) const {
    return vendor==other.vendor && device==other.device && driver==other.driver && api==other.api &&
           pipeline_uuid==other.pipeline_uuid && probe_revision==other.probe_revision &&
           physical_cookie==other.physical_cookie && provider_cookie==other.provider_cookie;
}
DriverIdentity driver_identity(const VkPhysicalDeviceProperties &properties, uint64_t revision) {
    DriverIdentity out;
    out.vendor=properties.vendorID; out.device=properties.deviceID;
    out.driver=properties.driverVersion; out.api=properties.apiVersion; out.probe_revision=revision;
    std::memcpy(out.pipeline_uuid.data(),properties.pipelineCacheUUID,VK_UUID_SIZE);
    return out;
}
size_t DriverIdentityHash::operator()(const DriverIdentity &id) const {
    size_t hash = 2166136261u;
    auto append=[&](uint64_t value) { for (unsigned i=0;i<8;++i) { hash=(hash ^ (value&255))*16777619u; value>>=8; } };
    append(id.vendor); append(id.device); append(id.driver); append(id.api); append(id.probe_revision);
    append(id.physical_cookie); append(id.provider_cookie);
    for (auto byte:id.pipeline_uuid) append(byte);
    return hash;
}
DriverProbeAssessment DriverTrustCache::get(const DriverIdentity &id, const std::function<DriverProbeAssessment()> &probe) {
    std::shared_future<DriverProbeAssessment> future;
    std::shared_ptr<std::promise<DriverProbeAssessment>> promise;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=entries_.find(id);
        if (it!=entries_.end()) {
            future=it->second.future;
            if (it->second.owner==std::this_thread::get_id() &&
                future.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
                return {};
            it->second.last_use = ++access_count_;
        } else {
            if (entries_.size() >= capacity_) {
                auto oldest = entries_.end();
                for (auto candidate = entries_.begin(); candidate != entries_.end(); ++candidate) {
                    if (candidate->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
                        (oldest == entries_.end() || candidate->second.last_use < oldest->second.last_use))
                        oldest = candidate;
                }
                if (oldest == entries_.end())
                    return {DriverProbeVerdict::Inconclusive, "probe cache has no completed entry to replace"};
                entries_.erase(oldest);
            }
            promise=std::make_shared<std::promise<DriverProbeAssessment>>();
            future=promise->get_future().share();
            entries_.emplace(id,Entry{future,std::this_thread::get_id(),++access_count_});
        }
    }
    if (promise) {
        DriverProbeAssessment result;
        try { result=probe(); }
        catch (const std::exception &error) { result.detail=std::string(error.what()).substr(0,4096); }
        catch (...) { result.detail="probe failed without a result"; }
        promise->set_value(std::move(result));
    }
    return future.get();
}
size_t DriverTrustCache::size() const { std::lock_guard<std::mutex> lock(mutex_); return entries_.size(); }
} // namespace vkfix

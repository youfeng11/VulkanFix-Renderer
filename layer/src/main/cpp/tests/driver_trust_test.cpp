#include "core/driver_trust.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

static unsigned failures=0;
static void check(bool ok,const char *what) { std::printf("%s: %s\n",ok?"PASS":"FAIL",what); failures+=!ok; }
int main() {
    using namespace vkfix;
    for (bool before:{false,true}) for (bool subject:{false,true}) for (bool after:{false,true}) {
        auto result=assess_driver_probe(before,subject,after);
        auto expected=(!before||!after) ? DriverProbeVerdict::Inconclusive : subject ? DriverProbeVerdict::Healthy : DriverProbeVerdict::RepairConfirmed;
        check(result.verdict==expected,"controlled verdict truth table");
        check(use_measured_repair(result,nullptr)==(expected==DriverProbeVerdict::RepairConfirmed),"auto repair requires confirmed differential failure");
        check(!use_measured_repair(result,"0") && use_measured_repair(result,"1"),"explicit diagnostic overrides survive every verdict");
        check(use_measured_repair(result,"unknown")==use_measured_repair(result,nullptr),"unrecognized override uses measurement");
        check(use_bounded_tiler_guard(result,nullptr)==(expected!=DriverProbeVerdict::Healthy),"bounded rendering remains until pressure paths pass controls");
        check(!use_bounded_tiler_guard(result,"0") && use_bounded_tiler_guard(result,"1"),"tiler force/bypass override every measured state");
    }
    DriverTrustCache cache;
    VkPhysicalDeviceProperties properties{};
    properties.apiVersion=VK_API_VERSION_1_1;
    auto id=driver_identity(properties,1);
    std::atomic<unsigned> calls{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    std::vector<DriverTrustReport> reports(16);
    for (unsigned i=0;i<16;++i) threads.emplace_back([&,i] {
        while (!start.load()) std::this_thread::yield();
        reports[i]=cache.get(id,[&]{ ++calls; DriverTrustReport result; result.sampler_aliases=assess_driver_probe(true,false,true); return result; });
    });
    start=true;
    for (auto &thread:threads) thread.join();
    check(calls==1 && cache.size()==1,"sixteen concurrent callers run exactly one probe");
    bool same=true;
    for (const auto &report:reports) same &= report.sampler_aliases.verdict==DriverProbeVerdict::RepairConfirmed;
    check(same,"all waiters receive the published measurement");
    auto distinct=[&](DriverIdentity other) {
        cache.get(other,[&]{ ++calls; return DriverTrustReport{}; });
    };
    for (unsigned field=0;field<8;++field) {
        auto other=id;
        switch(field) {
        case 0: ++other.vendor; break; case 1: ++other.device; break;
        case 2: ++other.driver; break; case 3: ++other.api; break;
        case 4: ++other.pipeline_uuid[0]; break; case 5: ++other.probe_revision; break;
        case 6: ++other.physical_cookie; break; case 7: ++other.provider_cookie; break;
        }
        distinct(other);
    }
    check(calls==9 && cache.size()==9,"device/driver/API/UUID/revision/physical/provider changes invalidate reuse");
    DriverTrustCache recursion;
    auto nested=recursion.get(id,[&]{ return recursion.get(id,[]{ throw std::runtime_error("must not run nested factory"); return DriverTrustReport{}; }); });
    check(nested.sampler_aliases.verdict==DriverProbeVerdict::Inconclusive,"recursive probe does not deadlock or fabricate health");
    DriverTrustCache throwing;
    auto failed=throwing.get(id,[]{ throw std::runtime_error("setup failed"); return DriverTrustReport{}; });
    auto repeated=throwing.get(id,[]{ throw std::runtime_error("must not retry"); return DriverTrustReport{}; });
    check(failed.sampler_aliases.detail=="setup failed" && repeated.sampler_aliases.detail=="setup failed","failed setup is inconclusive and memoized");
    DriverTrustCache bounded(2);
    for (unsigned i=0;i<20;++i) { auto key=id; key.driver=i; bounded.get(key,[]{ return DriverTrustReport{}; }); }
    check(bounded.size()==2,"cache growth is bounded");
    std::printf("Driver trust regression: %u failure(s)\n",failures);
    return failures?1:0;
}

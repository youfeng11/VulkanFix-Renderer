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
    std::vector<DriverProbeAssessment> reports(16);
    for (unsigned i=0;i<16;++i) threads.emplace_back([&,i] {
        while (!start.load()) std::this_thread::yield();
        reports[i]=cache.get(id,[&]{ ++calls; return assess_driver_probe(true,false,true); });
    });
    start=true;
    for (auto &thread:threads) thread.join();
    check(calls==1 && cache.size()==1,"sixteen concurrent callers run exactly one probe");
    bool same=true;
    for (const auto &report:reports) same &= report.verdict==DriverProbeVerdict::RepairConfirmed;
    check(same,"all waiters receive the published measurement");
    auto distinct=[&](DriverIdentity other) {
        cache.get(other,[&]{ ++calls; return DriverProbeAssessment{}; });
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
    auto nested=recursion.get(id,[&]{ return recursion.get(id,[]{ throw std::runtime_error("must not run nested factory"); return DriverProbeAssessment{}; }); });
    check(nested.verdict==DriverProbeVerdict::Inconclusive,"recursive probe does not deadlock or fabricate health");
    DriverTrustCache throwing;
    auto failed=throwing.get(id,[]{ throw std::runtime_error("setup failed"); return DriverProbeAssessment{}; });
    auto repeated=throwing.get(id,[]{ throw std::runtime_error("must not retry"); return DriverProbeAssessment{}; });
    check(failed.detail=="setup failed" && repeated.detail=="setup failed","failed setup is inconclusive and memoized");
    DriverTrustCache bounded(2);
    unsigned bounded_calls = 0;
    const auto measure = [&] { ++bounded_calls; return assess_driver_probe(true,true,true); };
    for (unsigned i=0;i<20;++i) { auto key=id; key.driver=i; bounded.get(key,measure); }
    check(bounded.size()==2 && bounded_calls==20, "new devices still run probes after the cache fills");
    auto recent = id, old = id, newcomer = id;
    recent.driver = 18;
    old.driver = 19;
    newcomer.driver = 20;
    bounded.get(recent, measure);
    bounded.get(newcomer, measure);
    bounded.get(recent, measure);
    check(bounded_calls==21, "a recently used result survives eviction");
    bounded.get(old, measure);
    check(bounded_calls==22, "least recently used result is measured again");

    DriverTrustCache busy(1);
    std::promise<void> entered, finish;
    auto finish_signal = finish.get_future();
    std::thread running([&] {
        busy.get(id, [&] {
            entered.set_value();
            finish_signal.wait();
            return assess_driver_probe(true,true,true);
        });
    });
    entered.get_future().wait();
    bool ran_new = false;
    const auto full = busy.get(newcomer, [&] { ran_new = true; return measure(); });
    check(!ran_new && full.verdict==DriverProbeVerdict::Inconclusive, "a running probe is never evicted");
    finish.set_value();
    running.join();
    check(busy.get(newcomer, measure).verdict==DriverProbeVerdict::Healthy && bounded_calls==23,
          "finishing the running probe makes capacity available again");
    std::printf("Driver trust regression: %u failure(s)\n",failures);
    return failures?1:0;
}

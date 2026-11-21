#include "core/driver_trust.h"
#include "core/driver_probe_process.h"
#include "core/driver_trust_shaders.h"
#include <cstdio>
#include <dlfcn.h>
#include <cstring>
int main(int argc,char **argv) {
    void *library=dlopen(argc>1?argv[1]:"libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    if (!library) { std::puts(dlerror()); return 1; }
    auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr"));
    if (!gipa) return 1;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr,"vkCreateInstance"));
    if (!create) return 1;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app;
    VkInstance instance{};
    if (create(&ci,nullptr,&instance)!=VK_SUCCESS) return 1;
    auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance,"vkEnumeratePhysicalDevices"));
    uint32_t count=1; VkPhysicalDevice physical{};
    if (!enumerate || enumerate(instance,&count,&physical)!=VK_SUCCESS || count!=1) return 1;
    if (argc>2 && (!std::strcmp(argv[2],"--push-only") || !std::strcmp(argv[2],"--identity"))) {
        VkPhysicalDeviceProperties properties{};
        auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance,"vkGetPhysicalDeviceProperties"));
        if (!query) return 1;
        query(physical,&properties);
        if (!std::strcmp(argv[2],"--identity")) {
            std::puts(vkfix::driver_probe_identity(properties,vkfix::nativeprobe::revision).c_str());
            return 0;
        }
        const auto result=vkfix::isolated_push_descriptor_probe(properties,vkfix::nativeprobe::revision);
        const auto verdict=vkfix::driver_probe_verdict_name(result.verdict);
        std::printf("push_descriptor=%s detail=%s\n",verdict,result.detail.c_str());
        return argc>3 && std::strcmp(argv[3],verdict) ? 3 : 0;
    }
    using vkfix::DriverProbe;
    const vkfix::NativeDriverProbes probes(instance, physical, gipa);
    if (argc > 2 && !std::strcmp(argv[2], "--queue2-only")) {
        const auto result = probes.measure(DriverProbe::Queue2);
        const auto verdict = vkfix::driver_probe_verdict_name(result.verdict);
        std::printf("queue2=%s detail=%s\n", verdict, result.detail.c_str());
        return argc > 3 && std::strcmp(argv[3], verdict) ? 3 : 0;
    }
    auto report = [&](const char *name, DriverProbe probe, const char *policy_name = nullptr,
                      bool (*policy)(const vkfix::DriverProbeAssessment &, const char *) = nullptr) {
        const auto first = probes.measure(probe);
        const auto second = probes.measure(probe);
        std::printf("%s=%s", name, vkfix::driver_probe_verdict_name(first.verdict));
        if (policy) std::printf(" %s=%u", policy_name, unsigned(policy(first, nullptr)));
        std::printf(" repeat=%s detail=%s\n",
                    vkfix::driver_probe_verdict_name(second.verdict), first.detail.c_str());
        return first.verdict;
    };
    const auto sampler = report("sampler_aliases", DriverProbe::SamplerAliases, "repair", vkfix::use_measured_repair);
    const auto uniform = report("large_uniform", DriverProbe::LargeUniform, "repair", vkfix::use_measured_repair);
    const auto push = report("push_descriptor", DriverProbe::PushDescriptor, "repair", vkfix::use_measured_repair);
    const auto tiler = report("tiler_budget", DriverProbe::TilerBudget, "guard", vkfix::use_bounded_tiler_guard);
    report("msaa_array_resolve", DriverProbe::MsaaArrayResolve);
    // The native probe may deliberately retain in-flight objects on timeout.
    // This executable is disposable; leave its instance alive until exit.
    if (argc>2 && std::strcmp(argv[2],vkfix::driver_probe_verdict_name(push))) return 3;
    if (argc>3 && std::strcmp(argv[3],vkfix::driver_probe_verdict_name(tiler))) return 4;
    return sampler==vkfix::DriverProbeVerdict::Inconclusive ||
           uniform==vkfix::DriverProbeVerdict::Inconclusive ? 2 : 0;
}

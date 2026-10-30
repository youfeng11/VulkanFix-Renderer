#include "core/driver_trust.h"
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
    const auto first=vkfix::native_driver_trust(instance,physical,gipa);
    const auto second=vkfix::native_driver_trust(instance,physical,gipa);
    std::printf("sampler_aliases=%s repair=%u repeat=%s detail=%s\n",vkfix::driver_probe_verdict_name(first.sampler_aliases.verdict),
        unsigned(vkfix::use_measured_repair(first.sampler_aliases,nullptr)),vkfix::driver_probe_verdict_name(second.sampler_aliases.verdict),first.sampler_aliases.detail.c_str());
    std::printf("large_uniform=%s repair=%u repeat=%s detail=%s\n",vkfix::driver_probe_verdict_name(first.large_uniform.verdict),
        unsigned(vkfix::use_measured_repair(first.large_uniform,nullptr)),vkfix::driver_probe_verdict_name(second.large_uniform.verdict),first.large_uniform.detail.c_str());
    std::printf("push_descriptor=%s repair=%u repeat=%s detail=%s\n",vkfix::driver_probe_verdict_name(first.push_descriptor.verdict),
        unsigned(vkfix::use_measured_repair(first.push_descriptor,nullptr)),vkfix::driver_probe_verdict_name(second.push_descriptor.verdict),first.push_descriptor.detail.c_str());
    std::printf("tiler_budget=%s guard=%u repeat=%s detail=%s\n",vkfix::driver_probe_verdict_name(first.tiler_budget.verdict),
        unsigned(vkfix::use_bounded_tiler_guard(first.tiler_budget,nullptr)),
        vkfix::driver_probe_verdict_name(second.tiler_budget.verdict),first.tiler_budget.detail.c_str());
    // The native probe may deliberately retain in-flight objects on timeout.
    // This executable is disposable; leave its instance alive until exit.
    if (argc>2 && std::strcmp(argv[2],vkfix::driver_probe_verdict_name(first.push_descriptor.verdict))) return 3;
    if (argc>3 && std::strcmp(argv[3],vkfix::driver_probe_verdict_name(first.tiler_budget.verdict))) return 4;
    return first.sampler_aliases.verdict==vkfix::DriverProbeVerdict::Inconclusive ||
           first.large_uniform.verdict==vkfix::DriverProbeVerdict::Inconclusive ? 2 : 0;
}

#pragma once
#include "../core/native_probe_resources.h"

namespace vkfix::nativeprobe {
struct Queue2Readback {
    std::string detail;
    bool passed = false;
    bool null_queue2 = false;
};

template<class Context, class Ready>
Queue2Readback probe_queue2(Context &p, VkPhysicalDevice physical, uint32_t family, bool subject, Ready ready) {
    Resources<Context> gpu(p, physical, family);
    constexpr uint32_t words = 64, pattern = 0x15a479e3u;
    constexpr VkDeviceSize bytes = words * sizeof(uint32_t);
    const auto output = gpu.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto cb = gpu.begin();
    auto fill = p.template dev<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    auto legacy_get = p.template dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
    auto modern_get = subject ? p.template dev<PFN_vkGetDeviceQueue2>("vkGetDeviceQueue2") : nullptr;
    VkDeviceQueueInfo2 info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    info.queueFamilyIndex = family;
    VkQueue first{}, legacy{}, modern{};
    ready();
    if (subject) modern_get(p.device, &info, &first);
    legacy_get(p.device, family, 0, &legacy);
    if (!legacy) return {"vkGetDeviceQueue returned no queue"};
    if (subject) {
        modern_get(p.device, &info, &modern);
        if (!first && !modern) return {"vkGetDeviceQueue2(flags=0) returned null before and after a valid legacy lookup", false, true};
        if (!first || first != modern) return {"vkGetDeviceQueue2(flags=0) returned inconsistent handles"};
    }
    fill(cb, output.handle, 0, bytes, pattern);
    gpu.host_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    gpu.submit(cb, false, subject ? modern : legacy);
    gpu.invalidate(output);
    const auto *read = static_cast<const uint32_t *>(output.mapped);
    if (!std::all_of(read, read + words, [=](uint32_t value) { return value == pattern; }))
        return {"retrieved queue did not produce the expected transfer readback"};
    if (subject && modern != legacy)
        return {"vkGetDeviceQueue2 returned a distinct handle, but its transfer readback matched"};
    return {subject ? "both lookups returned the same handle and its transfer readback matched" :
                      "legacy queue executed the transfer and all 64 words matched", true};
}
} // namespace vkfix::nativeprobe

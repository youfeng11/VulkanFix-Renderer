#pragma once
#include "../core/native_probe_resources.h"
#include <array>
#include <chrono>
#include <cmath>

namespace vkfix::nativeprobe {
struct TimestampReadback {
    bool passed;
    std::string detail;
};

template<class Context, class Ready>
TimestampReadback probe_timestamps(Context &p, VkPhysicalDevice physical, uint32_t family,
                                  uint32_t bits, float period, Ready ready) {
    struct QueryResult { uint64_t timestamp, available; };
    constexpr uint32_t count = 3;
    constexpr VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    constexpr uint64_t guard = 0xa5a5a5a5a5a5a5a5ull;
    constexpr VkDeviceSize offset = sizeof(uint64_t);
    constexpr VkDeviceSize stride = sizeof(QueryResult) + sizeof(uint64_t);
    constexpr VkDeviceSize bytes = offset + count * stride;
    Resources<Context> gpu(p, physical, family);
    const auto output = gpu.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto workload = gpu.buffer(65536, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto destroy = p.template dev<PFN_vkDestroyQueryPool>("vkDestroyQueryPool");
    auto reset = p.template dev<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool");
    auto timestamp = p.template dev<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp");
    auto fill = p.template dev<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    auto copy = p.template dev<PFN_vkCmdCopyQueryPoolResults>("vkCmdCopyQueryPoolResults");
    auto read = p.template dev<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults");

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    VkQueryPool pool{};
    check(p.template dev<PFN_vkCreateQueryPool>("vkCreateQueryPool")(p.device, &info, nullptr, &pool),
          "create timestamp probe pool");
    gpu.objects.push_back([=, &p] { destroy(p.device, pool, nullptr); });

    ready();
    if (bits < 36 || bits > 64 || !std::isfinite(period) || period <= 0)
        return {false, "invalid timestampValidBits or timestampPeriod"};
    const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
    uint64_t elapsed_ticks = 0;
    for (uint32_t pattern : {0x24acf097u, 0x5c39e162u}) {
        auto cb = gpu.begin();
        reset(cb, pool, 0, count);
        gpu.submit(cb);
        std::array<QueryResult, count> host{};
        for (auto &entry : host) entry.available = guard;
        const auto reset_result = read(p.device, pool, 0, count, sizeof host, host.data(), sizeof(QueryResult), flags);
        if (reset_result != VK_NOT_READY || std::any_of(host.begin(), host.end(), [](const auto &entry) {
            return entry.available != 0;
        })) return {false, "command-buffer reset did not leave timestamp queries unavailable"};

        cb = gpu.begin();
        timestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
        fill(cb, workload.handle, 0, VK_WHOLE_SIZE, pattern);
        timestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
        gpu.host_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const auto submit_start = std::chrono::steady_clock::now();
        gpu.submit(cb);
        const double submit_ns = std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - submit_start).count();
        gpu.invalidate(workload);
        const auto *words = static_cast<const uint32_t *>(workload.mapped);
        if (!std::all_of(words, words + 65536 / sizeof(uint32_t), [=](uint32_t word) { return word == pattern; }))
            return {false, "timestamp probe workload readback mismatch"};

        // The completion fence bounds execution. Avoid a second unbounded
        // wait if a broken driver did not make the timestamps available.
        for (auto &entry : host) entry = {guard, guard};
        const auto result = read(p.device, pool, 0, count, sizeof host, host.data(), sizeof(QueryResult), flags);
        if (result != VK_NOT_READY || !host[0].available || !host[1].available || host[2].available)
            return {false, "host timestamp availability did not match the two writes and unwritten query"};
        if ((host[0].timestamp | host[1].timestamp) & ~mask)
            return {false, "timestamp readback has bits set above timestampValidBits"};
        elapsed_ticks = (host[1].timestamp - host[0].timestamp) & mask;
        // Accept a counter wrap and equal timestamps. Only test direction when
        // even a half wrap would take longer than the complete submission.
        const uint64_t half_range = uint64_t{1} << (bits - 1);
        if (elapsed_ticks >= half_range && double(half_range) * period > submit_ns)
            return {false, "timestamp order reversed within the counter range"};

        cb = gpu.begin();
        fill(cb, output.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
        const VkMemoryBarrier writes{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        p.template dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &writes, 0, nullptr, 0, nullptr);
        copy(cb, pool, 0, count, output.handle, offset, stride, flags);
        gpu.host_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        gpu.submit(cb);
        gpu.invalidate(output);
        const auto *data = static_cast<const uint64_t *>(output.mapped);
        if (data[0] != guard) return {false, "timestamp copy modified the destination prefix"};
        for (uint32_t index = 0; index < count; ++index) {
            const auto *copied = data + 1 + index * (stride / sizeof(uint64_t));
            if (copied[2] != guard) return {false, "timestamp copy modified destination stride padding"};
            const bool expected_available = host[index].available != 0;
            if (expected_available != (copied[1] != 0) ||
                (expected_available && host[index].timestamp != copied[0]))
                return {false, "GPU-copied timestamp result or availability differs from host readback"};
        }
    }
    return {true, "queue family " + std::to_string(family) + "; " + std::to_string(bits) +
        " valid bits; " + std::to_string(period) + " ns per tick; observed " + std::to_string(elapsed_ticks) +
        " ticks; two reset/write cycles and strided GPU copies matched host results"};
}
} // namespace vkfix::nativeprobe

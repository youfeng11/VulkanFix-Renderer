#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace vkfix::nativeprobe {
// A command-buffer reset supplies the independent control. Both arms write
// and read the same timestamp pool, including the slots outside the reset.
template<class Context, class Gpu, class Ready>
std::string probe_host_query_reset(Context &p, Gpu &gpu, const char *host_reset, Ready ready) {
    struct QueryResult { uint64_t value, available; };
    constexpr uint32_t count = 4;
    constexpr uint32_t first = 1;
    constexpr uint32_t reset_count = 2;
    constexpr VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    auto host = host_reset ? p.template dev<PFN_vkResetQueryPool>(host_reset) : nullptr;
    auto create = p.template dev<PFN_vkCreateQueryPool>("vkCreateQueryPool");
    auto destroy = p.template dev<PFN_vkDestroyQueryPool>("vkDestroyQueryPool");
    auto reset = p.template dev<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool");
    auto write = p.template dev<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp");
    auto results = p.template dev<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults");
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    VkQueryPool pool{};
    if (const auto status = create(p.device, &info, nullptr, &pool); status != VK_SUCCESS)
        throw std::runtime_error("create query reset probe pool: VkResult " + std::to_string(status));
    gpu.objects.push_back([=, &p] { destroy(p.device, pool, nullptr); });

    auto read = [&](std::array<QueryResult, count> &data, bool expect_reset = false) {
        data.fill({});
        if (expect_reset)
            for (uint32_t index = first; index < first + reset_count; ++index) data[index].available = 1;
        return results(p.device, pool, 0, count, sizeof data, data.data(), sizeof(QueryResult), flags);
    };
    auto write_range = [&](VkCommandBuffer cb, uint32_t start, uint32_t end) {
        for (uint32_t query = start; query < end; ++query)
            write(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, query);
    };
    auto all_available = [](const auto &data) {
        return std::all_of(data.begin(), data.end(), [](const auto &entry) {
            return entry.available != 0;
        });
    };

    ready();
    auto cb = gpu.begin();
    reset(cb, pool, 0, count);
    write_range(cb, 0, count);
    gpu.submit(cb);
    std::array<QueryResult, count> initial{};
    if (read(initial) != VK_SUCCESS || !all_available(initial))
        return "written queries did not become available";

    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        if (host) {
            host(p.device, pool, first, reset_count);
        } else {
            cb = gpu.begin();
            reset(cb, pool, first, reset_count);
            gpu.submit(cb);
        }
        std::array<QueryResult, count> data{};
        if (read(data, true) != VK_NOT_READY) return "reset queries did not return unavailable/NOT_READY";
        for (uint32_t index = first; index < first + reset_count; ++index) {
            if (data[index].available != 0) return "reset queries did not return unavailable/NOT_READY";
        }
        const auto neighbors_unchanged = [&](const auto &entries) {
            for (const auto index : {0u, count - 1}) {
                if (!entries[index].available || entries[index].value != initial[index].value) return false;
            }
            return true;
        };
        if (!neighbors_unchanged(data))
            return "partial reset changed a neighboring query";

        cb = gpu.begin();
        write_range(cb, first, first + reset_count);
        gpu.submit(cb);
        if (read(data) != VK_SUCCESS || !all_available(data))
            return "rewritten queries did not become available after reset";
        if (!neighbors_unchanged(data))
            return "reusing the reset range changed a neighboring query";
    }
    return {};
}
} // namespace vkfix::nativeprobe

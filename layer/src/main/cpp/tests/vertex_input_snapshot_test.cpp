#include "core/vertex_input_snapshot.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
unsigned failures;
void check(bool ok, const char* message) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
VkBuffer buffer(uint64_t id) { return (VkBuffer)id; }
constexpr uint32_t header = vkfix::kVertexStoreHeaderWords * sizeof(uint32_t);
}
int main() {
    uint32_t address = 0;
    vkfix::VertexInputSnapshot shared(128 * 1024 * 1024);
    bool accepted = true;
    for (uint32_t i = 0; i < 5; ++i) {
        accepted &= shared.append(buffer(1), 4 * 1024 * 1024, i * 16, address);
        accepted &= address == header + i * 16;
    }
    check(accepted && shared.copy_count == 1 && shared.size == header + 4 * 1024 * 1024,
          "four vertex bindings plus an index binding share one 4 MiB copy instead of five");
    vkfix::VertexInputSnapshot distinct(4096);
    accepted = distinct.append(buffer(1), 5, 4, address) && address == header + 4;
    accepted &= distinct.append(buffer(2), 7, 6, address) && address == header + 8 + 6;
    check(accepted && distinct.size == header + 16 && distinct.copy_count == 2,
          "distinct buffers remain separate and each snapshot starts on a word boundary");
    const auto saved_size = distinct.size;
    const auto saved_count = distinct.copy_count;
    address = 0xdeadbeef;
    check(!distinct.append(buffer(3), UINT64_MAX, 0, address) && address == 0xdeadbeef &&
              distinct.size == saved_size && distinct.copy_count == saved_count,
          "overflow-sized buffers are rejected before changing the plan or destination address");
    check(!distinct.append(buffer(1), 5, 5, address) &&
              !distinct.append(buffer(0), 5, 0, address) &&
              !distinct.append(buffer(1), 6, 0, address),
          "out-of-range offsets, null buffers and inconsistent duplicate sizes are rejected");
    vkfix::VertexInputSnapshot padding(header + 5);
    check(!padding.append(buffer(1), 5, 0, address),
          "storage range includes trailing alignment bytes");
    vkfix::VertexInputSnapshot small(header - 1);
    check(!small.append(buffer(1), 1, 0, address), "storage range must contain the complete draw header");
    vkfix::VertexInputSnapshot boundary(UINT64_MAX);
    const VkDeviceSize last_size = (VkDeviceSize(UINT32_MAX) & ~VkDeviceSize(3)) - header;
    check(boundary.append(buffer(1), last_size, last_size - 1, address) && address == UINT32_MAX - 4 &&
              !boundary.append(buffer(2), 4, 0, address),
          "all shader addresses and aligned snapshot sizes remain representable in 32 bits");
    vkfix::VertexInputSnapshot capacity(4096);
    accepted = true;
    for (uint32_t i = 0; i < vkfix::kVertexStoreBindings + 1; ++i)
        accepted &= capacity.append(buffer(i + 1), 4, 0, address);
    check(accepted && !capacity.append(buffer(100), 4, 0, address) &&
              capacity.append(buffer(1), 4, 3, address) && address == header + 3,
          "fixed storage covers all 16 vertex buffers plus one index buffer without overrun");

    // Execute the planned copies into host memory, then compare every requested
    // byte against its source buffer. Each draw reuses handles with new contents;
    // repeated/aliased bindings use varied offsets within that draw.
    uint32_t random = 0x5eed;
    auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    bool equivalent = true;
    for (unsigned draw = 0; draw < 1000; ++draw) {
        std::vector<uint8_t> sources[6];
        for (auto& source : sources) {
            source.resize(1 + next() % 511);
            for (auto& byte : source) byte = uint8_t(next());
        }
        vkfix::VertexInputSnapshot plan(8192);
        struct Request { unsigned source, offset; uint32_t address; };
        std::vector<Request> requests;
        for (unsigned slot = 0; slot < 17; ++slot) {
            const unsigned id = next() % 6, offset = next() % sources[id].size();
            uint32_t dest = 0;
            equivalent &= plan.append(buffer(id + 1), sources[id].size(), offset, dest);
            requests.push_back({id, offset, dest});
        }
        std::vector<uint8_t> snapshot(plan.size, 0xcd);
        for (uint32_t i = 0; i < plan.copy_count; ++i) {
            const auto& copy = plan.copies[i];
            const auto id = uint64_t(copy.source) - 1;
            std::memcpy(snapshot.data() + copy.destination, sources[id].data(), copy.size);
        }
        for (const auto& request : requests) {
            const auto& source = sources[request.source];
            for (unsigned byte = request.offset; byte < source.size(); ++byte)
                equivalent &= snapshot.at(request.address + byte - request.offset) == source[byte];
        }
    }
    check(equivalent, "1000 randomized draw snapshots preserve every aliased vertex/index byte");
    printf("Vertex input snapshot regressions: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}

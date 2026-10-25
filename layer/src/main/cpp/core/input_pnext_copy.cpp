#include "input_pnext_copy.h"
#include <new>
#include <set>
namespace vkfix {
namespace {
#include "input_pnext_metadata.inc"
} // namespace
VkResult InputPNextCopy::copy(const void *source, InputChainKind kind, bool deep) {
    nodes.clear();
    payloads.clear();
    std::set<const void *> seen;
    try {
        for (auto node = static_cast<const VkBaseInStructure *>(source); node; node = node->pNext) {
            if (!seen.insert(node).second) {
                nodes.clear();
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            auto owned = clone_input_node(node, int(kind), *this, deep);
            if (!owned) {
                nodes.clear();
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
            nodes.push_back(std::move(owned));
        }
        for (size_t i = 0; i < nodes.size(); ++i)
            nodes[i]->pNext = i + 1 < nodes.size() ? nodes[i + 1].get() : nullptr;
        return VK_SUCCESS;
    } catch (const std::bad_alloc &) {
        nodes.clear();
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
} // namespace vkfix

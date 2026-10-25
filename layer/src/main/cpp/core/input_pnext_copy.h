#pragma once
#include "vk_common.h"
#include <cstdint>
#include <memory>
#include <vector>
namespace vkfix {
enum class InputChainKind { Submit = 1, Semaphore = 2, BindSparse = 4 };
// Owns typed nodes and links. deep=true also owns counted pointer payloads.
// Top-level submission arrays and nested sparse bindings are owned separately
// by the queue snapshot; unsupported pointer payloads are never borrowed deep.
class InputPNextCopy {
  public:
    VkResult copy(const void *source, InputChainKind kind, bool deep = false);
    template <class T> T *retain_array(const T *source, size_t count) {
        if (!source || !count)
            return nullptr;
        auto values = std::make_shared<std::vector<T>>(source, source + count);
        auto result = values->data();
        payloads.push_back(values);
        return result;
    }
    const void *retain_array(const void *source, size_t count) {
        return retain_array(static_cast<const uint8_t *>(source), count);
    }
    void *head() const {
        return nodes.empty() ? nullptr : nodes.front().get();
    }

  private:
    std::vector<std::shared_ptr<VkBaseOutStructure>> nodes;
    std::vector<std::shared_ptr<void>> payloads;
};
} // namespace vkfix

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vkfix {
enum class StorageArrayPreparation { Unchanged, Normalized, FailedSpecialization };
// Admit a default module while preserving source for stage-specific lengths.
// A shader needing a version rewrite can be retried after that rewrite.
StorageArrayPreparation prepare_storage_arrays(std::vector<uint32_t> &code,
    std::vector<uint32_t> &deferred_source, uint32_t native_extensions, std::string &error);
}

#include "storage_array_preparation.h"
#include "specialize_constants.h"
#include "storage_buffer_lowering.h"

namespace vkfix {
StorageArrayPreparation prepare_storage_arrays(std::vector<uint32_t> &code,
    std::vector<uint32_t> &deferred_source, uint32_t native_extensions, std::string &error) {
    auto normalized = code;
    auto result = normalize_storage_buffer_arrays(normalized, error, native_extensions);
    if (result == StorageBufferLowering::NeedsSpecialization) {
        if (!specialize_constants(normalized, nullptr, error)) return StorageArrayPreparation::FailedSpecialization;
        result = normalize_storage_buffer_arrays(normalized, error, native_extensions);
        if (result != StorageBufferLowering::Changed) return StorageArrayPreparation::FailedSpecialization;
        deferred_source = std::move(code);
    }
    if (result != StorageBufferLowering::Changed) return StorageArrayPreparation::Unchanged;
    code = std::move(normalized);
    return StorageArrayPreparation::Normalized;
}
} // namespace vkfix

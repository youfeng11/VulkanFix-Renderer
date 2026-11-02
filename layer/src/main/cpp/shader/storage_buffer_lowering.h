#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace vkfix {
enum class StorageBufferLowering { Unchanged, Changed, Invalid, NeedsSpecialization };
bool has_storage_buffer_class(const uint32_t*, size_t word_count);
bool has_storage_buffer_array_access(const uint32_t*, size_t word_count);
// Lower StorageBuffer + Block to Vulkan 1.0's Uniform + BufferBlock without
// changing Uniform/PushConstant users of the same source structure.
StorageBufferLowering lower_storage_buffer_class(std::vector<uint32_t>&, std::string& error);
// Scalarize descriptor-array aggregate memory accesses while preserving the
// shader's SPIR-V version, StorageBuffer class, and pipeline specialization.
StorageBufferLowering normalize_storage_buffer_arrays(std::vector<uint32_t>&, std::string& error,
                                                     uint32_t native_extensions = 0);
}

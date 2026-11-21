#include "modules/vulkan_1_1/descriptor_update_template.h"
#include "core/layer_manager.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

namespace {
unsigned failures = 0, calls = 0, invalid = 0;
unsigned translated_calls = 0;
bool fail_allocation = false, push_valid = true;
std::vector<uint64_t> values;
void check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
VKAPI_ATTR void VKAPI_CALL update(VkDevice, uint32_t count, const VkWriteDescriptorSet* writes,
                                  uint32_t, const VkCopyDescriptorSet*) {
    ++calls;
    for (uint32_t w = 0; w < count; ++w) {
        const auto& write = writes[w];
        // Binding 0 has two elements and binding 1 is its compatible successor.
        // The driver's consecutive-binding rule applies inside a single write;
        // starting a separate write at array element 2 of binding 0 is invalid.
        if (write.dstBinding == 0 && write.dstArrayElement >= 2) ++invalid;
        for (uint32_t i = 0; i < write.descriptorCount; ++i) {
            switch (write.descriptorType) {
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    values.push_back(uint64_t(write.pImageInfo[i].imageView)); break;
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    values.push_back(uint64_t(write.pBufferInfo[i].buffer)); break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    values.push_back(uint64_t(write.pTexelBufferView[i])); break;
                case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: {
                    auto bytes = (const VkWriteDescriptorSetInlineUniformBlock*)write.pNext;
                    if (!bytes || bytes->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK ||
                        bytes->pNext || bytes->dataSize != write.descriptorCount) { ++invalid; break; }
                    values.push_back(((const uint8_t*)bytes->pData)[i]); break;
                }
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                    auto info = (const VkWriteDescriptorSetAccelerationStructureKHR*)write.pNext;
                    if (!info || info->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR ||
                        info->pNext || info->accelerationStructureCount != write.descriptorCount) { ++invalid; break; }
                    values.push_back(uint64_t(info->pAccelerationStructures[i])); break;
                }
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV: {
                    auto info = (const VkWriteDescriptorSetAccelerationStructureNV*)write.pNext;
                    if (!info || info->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_NV ||
                        info->pNext || info->accelerationStructureCount != write.descriptorCount) { ++invalid; break; }
                    values.push_back(uint64_t(info->pAccelerationStructures[i])); break;
                }
                default: ++invalid; break;
            }
        }
    }
}
VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t set, uint32_t count, const VkWriteDescriptorSet* writes) {
    push_valid &= point == VK_PIPELINE_BIND_POINT_COMPUTE && layout == (VkPipelineLayout)(uintptr_t)0xdef && set == 2;
    for (uint32_t i = 0; i < count; ++i) push_valid &= writes[i].dstSet == VK_NULL_HANDLE;
    update({}, count, writes, 0, nullptr);
}
VKAPI_ATTR void VKAPI_CALL translated_update(VkDevice device, uint32_t wc, const VkWriteDescriptorSet* writes,
    uint32_t cc, const VkCopyDescriptorSet* copies) {
    ++translated_calls; update(device,wc,writes,cc,copies);
}
struct Element {
    VkDescriptorBufferInfo buffer;
    uint64_t padding;
    VkDescriptorImageInfo image;
    VkBufferView view;
};
struct AllocationLog {
    std::array<void*, 64> live{};
    unsigned requests = 0, successful = 0, freed = 0, failAt = 0;
    bool valid = true;
    void* lastFreeData = nullptr;
    void* expectedFreeData = nullptr;
};
struct CallbackData { AllocationLog* log; };
VKAPI_ATTR void* VKAPI_CALL allocate_object(void* data, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    auto& log = *static_cast<CallbackData*>(data)->log;
    ++log.requests;
    log.valid &= size && alignment && !(alignment & (alignment - 1)) && scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT;
    if (log.requests == log.failAt) return nullptr;
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment < sizeof(void*) ? sizeof(void*) : alignment, size)) return nullptr;
    for (auto& entry : log.live) if (!entry) { entry = memory; ++log.successful; return memory; }
    log.valid = false; std::free(memory); return nullptr;
}
VKAPI_ATTR void* VKAPI_CALL reallocate_object(void*, void*, size_t, size_t, VkSystemAllocationScope) { return nullptr; }
VKAPI_ATTR void VKAPI_CALL free_object(void* data, void* memory) {
    if (!memory) return;
    auto& log = *static_cast<CallbackData*>(data)->log;
    log.lastFreeData = data;
    log.valid &= !log.expectedFreeData || log.expectedFreeData == data;
    for (auto& entry : log.live) if (entry == memory) {
        entry = nullptr; ++log.freed; std::free(memory); return;
    }
    log.valid = false;
}
VkAllocationCallbacks callbacks(CallbackData* data) {
    return {data, allocate_object, reallocate_object, free_object, nullptr, nullptr};
}
}

void* operator new(size_t size) {
    if (fail_allocation) throw std::bad_alloc();
    if (auto p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

void LayerManager::register_module_factory(ModuleFactory) {}
extern "C" VkInstance get_last_instance() { return VK_NULL_HANDLE; }
extern "C" void* get_real_proc(VkInstance, VkDevice, const char* name) {
    return std::strcmp(name, "vkUpdateDescriptorSets") == 0 ? (void*)update : nullptr;
}

int main() {
    DescriptorUpdateTemplateModule module;
    const auto a = reinterpret_cast<VkDevice>(uintptr_t(0x123));
    const auto b = reinterpret_cast<VkDevice>(uintptr_t(0x456));
    std::array<Element, 3> data{};
    for (size_t i = 0; i < data.size(); ++i) {
        data[i].buffer = {VkBuffer(0x100ULL + i), i * 64, 64};
        data[i].image = {VkSampler(0x200ULL + i), VkImageView(0x300ULL + i), VK_IMAGE_LAYOUT_GENERAL};
        data[i].view = VkBufferView(0x400ULL + i);
    }
    VkDescriptorUpdateTemplateEntry entries[] = {
        {0, 0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, offsetof(Element, buffer), sizeof(Element)},
        {2, 0, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, offsetof(Element, image), sizeof(Element)},
        {3, 0, 3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, offsetof(Element, view), sizeof(Element)}
    };
    VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    info.descriptorUpdateEntryCount = 3;
    info.pDescriptorUpdateEntries = entries;
    auto create = [&](VkDevice d) {
        VkDescriptorUpdateTemplate handle = VK_NULL_HANDLE;
        VkResult result = VK_ERROR_UNKNOWN;
        bool handled = module.on_create_descriptor_update_template(d, &info, nullptr, &handle, result);
        check(handled && result == VK_SUCCESS && handle != VK_NULL_HANDLE, "create emulated template");
        return handle;
    };
    auto apply = [&](VkDevice d, VkDescriptorUpdateTemplate handle) {
        calls = invalid = 0;
        values.clear();
        module.on_update_descriptor_set_with_template(d, VkDescriptorSet(0x789ULL), handle, data.data());
    };
    const auto ta = create(a), tb = create(b);
    apply(a, ta);
    check(invalid == 0, "template array spanning compatible bindings uses valid destination indices");
    check(calls == 3, "nine strided descriptors require three driver calls instead of nine");
    check(values == std::vector<uint64_t>{0x100, 0x101, 0x102, 0x300, 0x301, 0x302, 0x400, 0x401, 0x402},
          "gathered buffer, image and texel descriptors preserve stride and order");
    values.reserve(9);
    fail_allocation = true;
    apply(a, ta);
    fail_allocation = false;
    check(calls == 3 && !invalid && values.size() == 9,
          "strided host updates allocate no memory after successful template creation");
    module.set_update_callback(translated_update);
    translated_calls = 0; fail_allocation = true; apply(a,ta); fail_allocation = false;
    check(translated_calls == 3 && calls == 3 && !invalid && values.size() == 9,
          "emulated template updates use the configured native-layout translation callback without allocating");
    module.set_update_callback(nullptr);
    module.on_destroy_device(a);
    apply(b, tb);
    check(values.size() == 9 && calls == 3, "destroying one device preserves the other device's live template");
    apply(a, ta);
    check(calls == 0, "device destruction retires its own template");
    module.on_destroy_descriptor_update_template(a, tb, nullptr);
    apply(b, tb);
    check(values.size() == 9, "another device cannot retire this device's template");
    module.on_destroy_descriptor_update_template(b, tb, nullptr);
    apply(b, tb);
    check(calls == 0, "template destruction retires the stored metadata");
    entries[0].stride = 0;
    info.descriptorUpdateEntryCount = 1;
    const auto repeated = create(b);
    apply(b, repeated);
    check(values == std::vector<uint64_t>{0x100, 0x100, 0x100}, "zero stride repeats the same source descriptor");
    module.on_destroy_descriptor_update_template(b, repeated, nullptr);
    entries[0].stride = sizeof(Element);
    info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
    info.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    info.pipelineLayout = (VkPipelineLayout)(uintptr_t)0xabc; info.set = 2;
    auto push_template = create(b);
    calls = invalid = 0; values.clear();
    fail_allocation = true;
    bool pushed = module.push_emulated_template(b, {}, push_template, (VkPipelineLayout)(uintptr_t)0xdef,
                                               2, data.data(), push);
    fail_allocation = false;
    check(pushed && push_valid && calls == 1 && !invalid && values.size() == 3 && values[2] == 0x102,
          "an emulated template decodes into native push writes without exposing its virtual handle or allocating");
    check(!module.push_emulated_template(a, {}, push_template, {}, 2, data.data(), push),
          "push templates cannot cross device ownership");
    module.on_destroy_descriptor_update_template(b, push_template, nullptr);
    VkDescriptorUpdateTemplateEntry inline_entry{4, 4, 8, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 3, 777};
    info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    info.pDescriptorUpdateEntries = &inline_entry;
    auto inline_template = create(b);
    uint8_t inline_data[16]; for (unsigned i = 0; i < 16; ++i) inline_data[i] = uint8_t(i + 10);
    calls = invalid = 0; values.clear();
    module.on_update_descriptor_set_with_template(b, VkDescriptorSet(0x789ULL), inline_template, inline_data);
    check(calls == 1 && !invalid && values.size() == 8 && values[0] == 13 && values[7] == 20,
          "inline uniform template entries write byte counts through the required pNext payload and ignore stride");
    module.on_destroy_descriptor_update_template(b, inline_template, nullptr);
    // pData need not be naturally aligned. Copy strided 64-bit handles into
    // typed scratch, including the high bits that must survive 32-bit hosts.
    std::array<uint8_t, 40> acceleration_data{};
    const uint64_t acceleration_values[]{0x1234567800000101ULL, 0x8765432100000202ULL};
    std::memcpy(acceleration_data.data() + 1, &acceleration_values[0], 8);
    std::memcpy(acceleration_data.data() + 18, &acceleration_values[1], 8);
    VkDescriptorUpdateTemplateEntry acceleration_entry{5, 0, 2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, 17};
    info.pDescriptorUpdateEntries = &acceleration_entry;
    for (auto type : {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV}) {
        acceleration_entry.descriptorType = type;
        auto handle = create(b);
        calls = invalid = 0; values.clear();
        fail_allocation = true;
        module.on_update_descriptor_set_with_template(b, VkDescriptorSet(0x789ULL), handle, acceleration_data.data());
        fail_allocation = false;
        check(calls == 1 && !invalid && values.size() == 2 && values[0] == acceleration_values[0] &&
              values[1] == acceleration_values[1],
              type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR ?
              "KHR acceleration templates preserve unaligned strided 64-bit handles through pNext without allocating" :
              "NV acceleration templates preserve unaligned strided 64-bit handles through pNext without allocating");
        module.on_destroy_descriptor_update_template(b, handle, nullptr);
    }
    VkDescriptorUpdateTemplate failed = (VkDescriptorUpdateTemplate)(uintptr_t)0x123;
    VkResult failed_result = VK_ERROR_UNKNOWN;
    fail_allocation = true;
    bool failed_handled = module.on_create_descriptor_update_template(b, &info, nullptr, &failed, failed_result);
    fail_allocation = false;
    check(failed_handled && failed_result == VK_ERROR_OUT_OF_HOST_MEMORY && !failed,
          "template preparation OOM is reported before publishing any virtual handle");

    info.pDescriptorUpdateEntries = entries; info.descriptorUpdateEntryCount = 3;
    AllocationLog log;
    CallbackData createData{&log}, destroyData{&log};
    auto createAllocator = callbacks(&createData), destroyAllocator = callbacks(&destroyData);
    VkDescriptorUpdateTemplate allocated{};
    VkResult allocated_result = VK_ERROR_UNKNOWN;
    module.on_create_descriptor_update_template(b, &info, &createAllocator, &allocated, allocated_result);
    check(allocated_result == VK_SUCCESS && allocated && log.valid && log.successful && !log.freed,
          "application allocator owns the emulated template and descriptor scratch with OBJECT scope and valid alignment");
    const auto allocationRequests = log.requests;
    createAllocator = {}; // The application-owned callback structure need not outlive creation.
    log.failAt = log.requests + 1;
    fail_allocation = true;
    apply(b, allocated);
    fail_allocation = false;
    check(calls == 3 && !invalid && values.size() == 9 && log.requests == allocationRequests,
          "callback-backed templates copy allocator state and perform no allocations while updating");
    log.expectedFreeData = &destroyData;
    module.on_destroy_descriptor_update_template(b, allocated, &destroyAllocator);
    check(log.valid && log.freed == log.successful && log.lastFreeData == &destroyData,
          "compatible destruction uses the current callback pUserData and frees the object, control block, context and scratch");
    for (unsigned failurePoint = 1; failurePoint <= allocationRequests; ++failurePoint) {
        AllocationLog failing;
        failing.failAt = failurePoint;
        CallbackData failureData{&failing};
        auto failureAllocator = callbacks(&failureData);
        VkDescriptorUpdateTemplate handle = (VkDescriptorUpdateTemplate)(uintptr_t)0x123;
        VkResult result = VK_ERROR_UNKNOWN;
        module.on_create_descriptor_update_template(b, &info, &failureAllocator, &handle, result);
        const bool failedCleanly = result == VK_ERROR_OUT_OF_HOST_MEMORY && !handle && failing.valid &&
            failing.requests == failurePoint && failing.successful == failing.freed;
        check(failedCleanly, "each application allocation failure rolls back every preceding allocation and publishes a null template");
        if (result == VK_SUCCESS) module.on_destroy_descriptor_update_template(b, handle, &failureAllocator);
    }
    module.set_native_route(b, true);
    check(!module.on_create_descriptor_update_template(b, &info, nullptr, &failed, failed_result) &&
          !module.on_update_descriptor_set_with_template(b, {}, {}, data.data()) &&
          !module.on_destroy_descriptor_update_template(b, {}, nullptr),
          "the authorized native backend delegates the whole template lifecycle to the manager");
    module.on_destroy_device(b);
    std::printf("Descriptor template regressions: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}

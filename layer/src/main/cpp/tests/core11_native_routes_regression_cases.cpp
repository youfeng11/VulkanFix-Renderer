#include "core/layer_manager.h"
#include "core/native_instance_state.h"
#include "modules/vulkan_1_1/get_memory_requirements2.h"
#include "modules/vulkan_1_1/descriptor_update_template.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>

namespace {
bool active = false;
struct State {
    uint32_t api = VK_API_VERSION_1_0;
    bool requirements = false, templates = false, partial = false, push = false;
    bool args = true, captured_requirements = false, captured_templates = false;
    VkPhysicalDevice physical{}; VkDevice device{};
    unsigned req[2][3]{}, tmpl[2][3]{}, legacy_updates = 0, pushes = 0;
    VkBuffer written{};
    VkDescriptorUpdateTemplateEntry native_entry{};
    VkDescriptorUpdateTemplate native_handle{};
} state;
template<class Base> class Scoped : public Base { bool is_enabled() const override { return active; } };
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice p, VkPhysicalDeviceProperties* out) {
    state.args &= p == state.physical; *out = {}; out->apiVersion = state.api;
}
VKAPI_ATTR void VKAPI_CALL features(VkPhysicalDevice, VkPhysicalDeviceFeatures2* out) { out->features = {}; }
VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice, const char*, uint32_t* count, VkExtensionProperties* out) {
    const char* names[4]; uint32_t size = 0;
    if (state.requirements) names[size++] = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME;
    if (state.templates) names[size++] = VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME;
    if (state.push) names[size++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    // This matrix isolates requirements/template entry-point authorization.
    // A native maintenance1 route avoids the independent volume-descriptor
    // translation requirement on the complete native-template profile.
    if (state.templates) names[size++] = VK_KHR_MAINTENANCE1_EXTENSION_NAME;
    if (!out) { *count = size; return VK_SUCCESS; }
    const auto written = std::min(size, *count);
    for (uint32_t i = 0; i < written; ++i) { out[i] = {}; strcpy(out[i].extensionName, names[i]); out[i].specVersion = 1; }
    *count = written; return written < size ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create(VkPhysicalDevice p, const VkDeviceCreateInfo* info, const VkAllocationCallbacks*, VkDevice* out) {
    state.args &= p == state.physical;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        state.captured_requirements |= !strcmp(info->ppEnabledExtensionNames[i], VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
        state.captured_templates |= !strcmp(info->ppEnabledExtensionNames[i], VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME);
    }
    *out = state.device; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice d, const VkAllocationCallbacks*) { state.args &= d == state.device; }
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* out) {
    *out = (VkCommandBuffer)(uintptr_t)0xee3001; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL buffer1(VkDevice, VkBuffer, VkMemoryRequirements* out) { *out = {4096, 64, 1}; }
VKAPI_ATTR void VKAPI_CALL image1(VkDevice, VkImage, VkMemoryRequirements* out) { *out = {8192, 128, 1}; }
VKAPI_ATTR void VKAPI_CALL sparse1(VkDevice, VkImage, uint32_t* count, VkSparseImageMemoryRequirements* out) {
    if (!out) { *count = 1; return; }
    if (*count) { *out = {}; out->imageMipTailSize = 100; *count = 1; }
}
template<int Route> VKAPI_ATTR void VKAPI_CALL buffer2(VkDevice d, const VkBufferMemoryRequirementsInfo2* info, VkMemoryRequirements2* out) {
    ++state.req[Route][0]; state.args &= d == state.device && info->buffer == (VkBuffer)uint64_t(0xee4001);
    buffer1(d, info->buffer, &out->memoryRequirements);
}
template<int Route> VKAPI_ATTR void VKAPI_CALL image2(VkDevice d, const VkImageMemoryRequirementsInfo2* info, VkMemoryRequirements2* out) {
    ++state.req[Route][1]; state.args &= d == state.device; image1(d, info->image, &out->memoryRequirements);
}
template<int Route> VKAPI_ATTR void VKAPI_CALL sparse2(VkDevice d, const VkImageSparseMemoryRequirementsInfo2*, uint32_t* count, VkSparseImageMemoryRequirements2* out) {
    ++state.req[Route][2]; state.args &= d == state.device;
    if (!out) { *count = 1; return; }
    if (*count) { out->memoryRequirements = {}; out->memoryRequirements.imageMipTailSize = 200; *count = 1; }
}
VKAPI_ATTR void* VKAPI_CALL no_allocation(void*, size_t, size_t, VkSystemAllocationScope) { return nullptr; }
VKAPI_ATTR void* VKAPI_CALL no_reallocation(void*, void*, size_t, size_t, VkSystemAllocationScope) { return nullptr; }
VKAPI_ATTR void VKAPI_CALL no_free(void*, void*) {}
const VkAllocationCallbacks allocator{nullptr, no_allocation, no_reallocation, no_free, nullptr, nullptr};
struct ParentAllocationLog { unsigned allocated = 0, freed = 0; bool valid = true; };
VKAPI_ATTR void* VKAPI_CALL parent_allocate(void* data, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    auto& log = *static_cast<ParentAllocationLog*>(data);
    log.valid &= size && alignment && !(alignment & (alignment - 1)) && scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT;
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment < sizeof(void*) ? sizeof(void*) : alignment, size)) return nullptr;
    ++log.allocated; return memory;
}
VKAPI_ATTR void VKAPI_CALL parent_free(void* data, void* memory) {
    if (memory) { ++static_cast<ParentAllocationLog*>(data)->freed; std::free(memory); }
}
VkAllocationCallbacks parent_callbacks(ParentAllocationLog& log) {
    return {&log, parent_allocate, no_reallocation, parent_free, nullptr, nullptr};
}
template<int Route> VKAPI_ATTR VkResult VKAPI_CALL create_template(VkDevice d, const VkDescriptorUpdateTemplateCreateInfo* info, const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* out) {
    ++state.tmpl[Route][0]; state.args &= d == state.device && alloc == &allocator && info->descriptorUpdateEntryCount == 1;
    state.native_entry = info->pDescriptorUpdateEntries[0];
    state.native_handle = (VkDescriptorUpdateTemplate)uint64_t(0x123400ee5001 + Route);
    *out = state.native_handle; return VK_SUCCESS;
}
template<int Route> VKAPI_ATTR void VKAPI_CALL destroy_template(VkDevice d, VkDescriptorUpdateTemplate handle, const VkAllocationCallbacks* alloc) {
    ++state.tmpl[Route][2]; state.args &= d == state.device && handle == state.native_handle && alloc == &allocator;
}
template<int Route> VKAPI_ATTR void VKAPI_CALL update_template(VkDevice d, VkDescriptorSet, VkDescriptorUpdateTemplate handle, const void* data) {
    ++state.tmpl[Route][1]; state.args &= d == state.device && handle == state.native_handle;
    VkDescriptorBufferInfo buffer; memcpy(&buffer, (const char*)data + state.native_entry.offset, sizeof(buffer)); state.written = buffer.buffer;
}
VKAPI_ATTR void VKAPI_CALL update(VkDevice d, uint32_t count, const VkWriteDescriptorSet* writes, uint32_t, const VkCopyDescriptorSet*) {
    ++state.legacy_updates; state.args &= d == state.device && count == 1 && writes[0].descriptorCount == 1;
    state.written = writes[0].pBufferInfo[0].buffer;
}
VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer, VkPipelineBindPoint point, VkPipelineLayout, uint32_t set, uint32_t count, const VkWriteDescriptorSet* writes) {
    ++state.pushes; state.args &= point == VK_PIPELINE_BIND_POINT_COMPUTE && set == 0 && count == 1 && writes[0].dstSet == VK_NULL_HANDLE;
    state.written = writes[0].pBufferInfo[0].buffer;
}
}

void* core11_native_routes_mock_proc(const char* name) {
    if (!active) return nullptr;
#define BASE(command, function) if (!strcmp(name, #command)) return (void*)function
    BASE(vkCreateDevice, create); BASE(vkDestroyDevice, destroy); BASE(vkAllocateCommandBuffers, allocate);
    BASE(vkGetPhysicalDeviceProperties, properties); BASE(vkEnumerateDeviceExtensionProperties, extensions);
    BASE(vkGetPhysicalDeviceFeatures2, features); BASE(vkGetPhysicalDeviceFeatures2KHR, features);
    BASE(vkGetBufferMemoryRequirements, buffer1); BASE(vkGetImageMemoryRequirements, image1); BASE(vkGetImageSparseMemoryRequirements, sparse1);
    BASE(vkUpdateDescriptorSets, update); BASE(vkCmdPushDescriptorSetKHR, push);
#undef BASE
#define PAIR(command, function) if (!strcmp(name, #command)) return (void*)function<0>; if (!strcmp(name, #command "KHR")) return (void*)function<1>
    PAIR(vkGetBufferMemoryRequirements2, buffer2); PAIR(vkGetImageMemoryRequirements2, image2); PAIR(vkGetImageSparseMemoryRequirements2, sparse2);
    PAIR(vkCreateDescriptorUpdateTemplate, create_template); PAIR(vkUpdateDescriptorSetWithTemplate, update_template);
    if (!state.partial) { PAIR(vkDestroyDescriptorUpdateTemplate, destroy_template); }
#undef PAIR
    return nullptr;
}

void run_core11_native_routes_regressions(void (*check)(bool, const char*)) {
    active = true;
    auto& manager = LayerManager::get();
    manager.register_module(std::make_unique<Scoped<GetMemoryRequirements2Module>>());
    manager.register_module(std::make_unique<Scoped<DescriptorUpdateTemplateModule>>());
    for (unsigned mode = 0; mode < 8; ++mode) {
        state = {};
        state.api = mode >= 2 && mode <= 3 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
        state.requirements = state.templates = mode == 1;
        state.partial = mode == 3; state.push = mode == 4;
        state.physical = (VkPhysicalDevice)(uintptr_t)(0xee1001 + mode);
        state.device = (VkDevice)(uintptr_t)(0xee2001 + mode);
        const auto instance = (VkInstance)(uintptr_t)(0xee0001 + mode);
        std::array<ParentAllocationLog, 3> allocations{};
        auto instanceAllocator = parent_callbacks(allocations[0]);
        auto deviceAllocator = parent_callbacks(allocations[1]);
        auto objectAllocator = parent_callbacks(allocations[2]);
        const auto destroyDeviceAllocator = deviceAllocator;
        vkfix::remember_native_instance(instance, state.api, state.api, {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME}, {},
            VK_API_VERSION_1_1, mode >= 5 ? &instanceAllocator : nullptr);
        instanceAllocator = {}; // Only the registry's copied callback structure remains usable.
        vkfix::remember_physical_devices(instance, 1, &state.physical);
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        const char* push_name = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        if (state.push) { ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = &push_name; }
        VkDevice device{};
        auto result = manager.dispatch_create_device(state.physical, &ci, mode >= 6 ? &deviceAllocator : nullptr, &device);
        deviceAllocator = {}; // Device creation must also retain its own callback copy.
        check(result == VK_SUCCESS && device == state.device, "native route matrix creates a device with the actual dependency plan");
        if (result != VK_SUCCESS) { vkfix::forget_native_instance(instance); continue; }
        const bool native_req = mode == 1 || mode == 2 || mode == 3;
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        dedicated.prefersDedicatedAllocation = dedicated.requiresDedicatedAllocation = VK_TRUE;
        VkMemoryRequirements2 output{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
        VkBufferMemoryRequirementsInfo2 bi{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, nullptr, (VkBuffer)uint64_t(0xee4001)};
        manager.dispatch_get_buffer_memory_requirements2(device, &bi, &output);
        VkImageMemoryRequirementsInfo2 ii{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr, (VkImage)uint64_t(0xee4002)};
        manager.dispatch_get_image_memory_requirements2(device, &ii, &output);
        VkImageSparseMemoryRequirementsInfo2 si{VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2, nullptr, ii.image};
        VkSparseImageMemoryRequirements2 sparse{VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2};
        uint32_t count = 1; manager.dispatch_get_image_sparse_memory_requirements2(device, &si, &count, &sparse);
        const int route = mode == 1 ? 1 : 0;
        check(output.memoryRequirements.size == 8192 && output.pNext == &dedicated && !dedicated.pNext &&
              !dedicated.prefersDedicatedAllocation && !dedicated.requiresDedicatedAllocation && count == 1 &&
              sparse.memoryRequirements.imageMipTailSize == (native_req ? 200 : 100),
              "requirements queries preserve headers and use native sparse results or the actual legacy payload");
        check(state.req[route][0] == unsigned(native_req) && state.req[route][1] == unsigned(native_req) && state.req[route][2] == unsigned(native_req) &&
              !state.req[1-route][0] && !state.req[1-route][1] && !state.req[1-route][2],
              "requirements query counters reject nonnull unenabled Core and KHR stubs");
        VkDescriptorBufferInfo data{(VkBuffer)uint64_t(0xee6001), 0, 64};
        VkDescriptorUpdateTemplateEntry entry{7, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, sizeof(data)};
        VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET; ti.descriptorSetLayout = (VkDescriptorSetLayout)uint64_t(0xee7001);
        ti.descriptorUpdateEntryCount = 1; ti.pDescriptorUpdateEntries = &entry;
        VkDescriptorUpdateTemplate handle{};
        const bool native_template = mode == 1 || mode == 2;
        // The native fixture checks allocator pointer forwarding. Its allocator
        // deliberately rejects all allocations, so ordinary emulation uses NULL.
        const auto chosen_allocator = mode == 7 ? &objectAllocator : native_template ? &allocator : nullptr;
        auto template_result = manager.dispatch_create_descriptor_update_template(device, &ti, chosen_allocator, &handle);
        if (template_result == VK_SUCCESS) {
            manager.dispatch_update_descriptor_set_with_template(device, (VkDescriptorSet)uint64_t(0xee8001), handle, &data);
            manager.dispatch_destroy_descriptor_update_template(device, handle, chosen_allocator);
        }
        check(template_result == VK_SUCCESS && handle && state.written == data.buffer && state.args &&
              state.tmpl[route][0] == unsigned(native_template) && state.tmpl[route][1] == unsigned(native_template) && state.tmpl[route][2] == unsigned(native_template) &&
              !state.tmpl[1-route][0] && !state.tmpl[1-route][1] && !state.tmpl[1-route][2] && state.legacy_updates == unsigned(!native_template),
              "template lifecycle stays wholly native Core, native KHR, or emulated even when only one native entry is missing");
        if (mode == 1) check(state.captured_requirements && state.captured_templates && !ci.enabledExtensionCount,
              "Core1.1 requests enable available private KHR backends without changing the application's extension list");
        if (mode >= 5) {
            bool valid = true;
            for (unsigned i = 0; i < allocations.size(); ++i)
                valid &= allocations[i].valid && allocations[i].freed == allocations[i].allocated &&
                    (i == mode - 5 ? allocations[i].allocated > 0 : allocations[i].allocated == 0);
            check(valid, "template storage uses the most specific copied object, device or instance allocator and releases it on destruction");
        }
        if (state.push) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = (VkCommandPool)uint64_t(0xee9001); ai.commandBufferCount = 1;
            VkCommandBuffer command{}; manager.dispatch_allocate_command_buffers(device, &ai, &command);
            ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
            ti.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; ti.pipelineLayout = (VkPipelineLayout)uint64_t(0xeea001); ti.set = 0;
            auto pushed = manager.dispatch_create_descriptor_update_template(device, &ti, nullptr, &handle);
            if (pushed == VK_SUCCESS) {
                manager.dispatch_cmd_push_descriptor_set_with_template(command, handle, ti.pipelineLayout, 0, &data);
                manager.dispatch_destroy_descriptor_update_template(device, handle, nullptr);
            }
            check(pushed == VK_SUCCESS && state.pushes == 1 && state.args && state.written == data.buffer && !state.tmpl[0][0] && !state.tmpl[1][0],
                  "native push plus emulated template decodes writes before any native template handle boundary");
        }
        manager.dispatch_destroy_device(device, mode >= 6 ? &destroyDeviceAllocator : nullptr);
        vkfix::forget_native_instance(instance);
    }
    active = false;
}

#include "core/layer_manager.h"
#include "core/legacy_query_array.h"
#include "modules/vulkan_1_1/bind_memory2.h"
#include "modules/vulkan_1_1/device_group.h"
#include "modules/vulkan_1_1/get_memory_requirements2.h"
#include "modules/vulkan_1_1/maintenance.h"
#include <array>

namespace {
bool active = false;
const auto physical = (VkPhysicalDevice)(uintptr_t)0xc11001;
const auto device = (VkDevice)(uintptr_t)0xc12001;
const auto command = (VkCommandBuffer)(uintptr_t)0xc13001;
unsigned trims = 0, masks = 0, legacy_sparse_calls = 0;
unsigned queue2_calls = 0, queue1_calls = 0;
unsigned buffer_binds = 0, image_binds = 0;
unsigned layout_creates = 0, layout_destroys = 0;
VkResult layout_status = VK_SUCCESS;
bool native_layout_query = false;
bool native_reqs2 = false;
VkResult enumerate_status = VK_SUCCESS;
uint32_t native_device_count = 3;
VkResult VKAPI_CALL create(VkPhysicalDevice, const VkDeviceCreateInfo*,
                          const VkAllocationCallbacks*, VkDevice* out) {
    *out = device;
    return VK_SUCCESS;
}
void VKAPI_CALL destroy(VkDevice, const VkAllocationCallbacks*) {}
VkResult VKAPI_CALL allocate(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* out) {
    *out = command;
    return VK_SUCCESS;
}
void VKAPI_CALL trim(VkDevice, VkCommandPool, VkCommandPoolTrimFlags) { ++trims; }
void VKAPI_CALL mask(VkCommandBuffer, uint32_t) { ++masks; }
VkResult VKAPI_CALL create_layout(VkDevice, const VkDescriptorSetLayoutCreateInfo*,
                                 const VkAllocationCallbacks*, VkDescriptorSetLayout* out) {
    ++layout_creates;
    if (layout_status == VK_SUCCESS) *out = (VkDescriptorSetLayout)uint64_t(0xc15001);
    return layout_status;
}
void VKAPI_CALL destroy_layout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {
    ++layout_destroys;
}
const auto legacy_queue = (VkQueue)(uintptr_t)0xc16001;
VkQueue modern_queue = VK_NULL_HANDLE;
void VKAPI_CALL get_queue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue* out) {
    ++queue2_calls;
    // Deliberately broken by default: the layer must repair a null or foreign
    // flags==0 result.
    if (out) *out = modern_queue;
}
void VKAPI_CALL get_queue(VkDevice, uint32_t, uint32_t, VkQueue* out) {
    ++queue1_calls;
    if (out) *out = legacy_queue;
}
VkResult VKAPI_CALL bind_buffer(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
    ++buffer_binds;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL bind_image(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
    ++image_binds;
    return VK_SUCCESS;
}
void VKAPI_CALL buffer_requirements(VkDevice, VkBuffer, VkMemoryRequirements* out) {
    *out = {};
    out->size = 4096;
    out->alignment = 64;
    out->memoryTypeBits = 1;
}
void VKAPI_CALL image_requirements(VkDevice, VkImage, VkMemoryRequirements* out) {
    *out = {};
    out->size = 8192;
    out->alignment = 128;
    out->memoryTypeBits = 1;
}
void VKAPI_CALL buffer_requirements2(VkDevice, const VkBufferMemoryRequirementsInfo2*,
                                    VkMemoryRequirements2* out) {
    out->memoryRequirements = {};
    out->memoryRequirements.size = 1;
    out->memoryRequirements.alignment = 1;
    out->memoryRequirements.memoryTypeBits = 1;
}
void VKAPI_CALL image_requirements2(VkDevice, const VkImageMemoryRequirementsInfo2*,
                                    VkMemoryRequirements2* out) {
    out->memoryRequirements = {};
    out->memoryRequirements.size = 1;
    out->memoryRequirements.alignment = 1;
    out->memoryRequirements.memoryTypeBits = 1;
}
void VKAPI_CALL layout_query(VkDevice, const VkDescriptorSetLayoutCreateInfo*, VkDescriptorSetLayoutSupport* out) {
    out->supported = VK_FALSE;
    auto* variable = (VkDescriptorSetVariableDescriptorCountLayoutSupport*)out->pNext;
    variable->maxVariableDescriptorCount = 37;
}
VkResult VKAPI_CALL enumerate(VkInstance, uint32_t* count, VkPhysicalDevice* out) {
    if (enumerate_status != VK_SUCCESS) return enumerate_status;
    if (!out) {
        *count = native_device_count;
        return VK_SUCCESS;
    }
    const auto written = std::min(*count, native_device_count);
    for (uint32_t i = 0; i < written; ++i)
        out[i] = (VkPhysicalDevice)(uintptr_t(physical) + i);
    *count = written;
    return written < native_device_count ? VK_INCOMPLETE : VK_SUCCESS;
}
void VKAPI_CALL sparse(VkDevice, VkImage, uint32_t* count, VkSparseImageMemoryRequirements* out) {
    ++legacy_sparse_calls;
    if (!out) {
        *count = 2;
        return;
    }
    const auto written = std::min(*count, 2u);
    for (uint32_t i = 0; i < written; ++i) {
        out[i] = {};
        out[i].imageMipTailSize = 8192 + i;
    }
    *count = written;
}
template<class Base> class ScopedModule : public Base {
    bool is_enabled() const override { return active; }
};
} // namespace

void* core11_mock_proc(const char* name) {
    if (!active) return nullptr;
    if (!strcmp(name, "vkCreateDevice")) return (void*)create;
    if (!strcmp(name, "vkDestroyDevice")) return (void*)destroy;
    if (!strcmp(name, "vkAllocateCommandBuffers")) return (void*)allocate;
    if (!strcmp(name, "vkTrimCommandPoolKHR")) return (void*)trim;
    if (!strcmp(name, "vkCmdSetDeviceMaskKHR")) return (void*)mask;
    if (!strcmp(name, "vkGetDeviceQueue2")) return (void*)get_queue2;
    if (!strcmp(name, "vkGetDeviceQueue2KHR")) return (void*)get_queue2;
    if (!strcmp(name, "vkGetDeviceQueue")) return (void*)get_queue;
    if (!strcmp(name, "vkBindBufferMemory")) return (void*)bind_buffer;
    if (!strcmp(name, "vkBindImageMemory")) return (void*)bind_image;
    if (!strcmp(name, "vkGetBufferMemoryRequirements")) return (void*)buffer_requirements;
    if (!strcmp(name, "vkGetImageMemoryRequirements")) return (void*)image_requirements;
    if (native_reqs2 && !strcmp(name, "vkGetBufferMemoryRequirements2KHR"))
        return (void*)buffer_requirements2;
    if (native_reqs2 && !strcmp(name, "vkGetImageMemoryRequirements2KHR"))
        return (void*)image_requirements2;
    if (!strcmp(name, "vkEnumeratePhysicalDevices")) return (void*)enumerate;
    if (!strcmp(name, "vkGetImageSparseMemoryRequirements")) return (void*)sparse;
    if (!strcmp(name, "vkCreateDescriptorSetLayout")) return (void*)create_layout;
    if (!strcmp(name, "vkDestroyDescriptorSetLayout")) return (void*)destroy_layout;
    if (!strcmp(name, "vkGetDescriptorSetLayoutSupportKHR") && native_layout_query) return (void*)layout_query;
    return nullptr;
}

void run_core11_regressions(void (*check)(bool, const char*)) {
    active = true;
    auto& manager = LayerManager::get();
    auto test_groups = [&] {
        uint32_t count = 0;
        check(manager.dispatch_enumerate_physical_device_groups({}, &count, nullptr) == VK_SUCCESS && count == 3,
              "1.1 groups: count includes all three native physical devices");
        std::array<VkPhysicalDeviceGroupProperties, 4> groups{};
        VkBaseOutStructure sentinel{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        for (auto& g : groups) {
            g.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
            g.pNext = &sentinel;
            g.physicalDeviceCount = 99;
        }
        count = 0;
        check(manager.dispatch_enumerate_physical_device_groups({}, &count, groups.data()) == VK_INCOMPLETE &&
                  count == 0 && groups[0].physicalDeviceCount == 99,
              "1.1 groups: zero-capacity output writes no structures and returns INCOMPLETE");
        count = 2;
        auto result = manager.dispatch_enumerate_physical_device_groups({}, &count, groups.data());
        check(result == VK_INCOMPLETE && count == 2 && groups[0].physicalDevices[0] == physical &&
                  groups[1].physicalDevices[0] == (VkPhysicalDevice)(uintptr_t(physical) + 1) &&
                  groups[2].physicalDeviceCount == 99,
              "1.1 groups: truncated output has exact capacity and native order");
        count = 4;
        result = manager.dispatch_enumerate_physical_device_groups({}, &count, groups.data());
        check(result == VK_SUCCESS && count == 3 && groups[2].physicalDeviceCount == 1 &&
                  groups[2].physicalDevices[1] == VK_NULL_HANDLE && !groups[2].subsetAllocation &&
                  groups[0].pNext == &sentinel && groups[2].pNext == &sentinel &&
                  groups[0].sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES &&
                  groups[3].physicalDeviceCount == 99,
              "1.1 groups: singleton payloads preserve caller headers and unused output");
        for (auto error : {VK_ERROR_INITIALIZATION_FAILED, VK_ERROR_OUT_OF_HOST_MEMORY,
                           VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
            enumerate_status = error;
            count = 4;
            groups[0].physicalDeviceCount = 99;
            check(manager.dispatch_enumerate_physical_device_groups({}, &count, groups.data()) == error &&
                      groups[0].physicalDeviceCount == 99,
                  "1.1 groups: native enumeration errors are propagated without invented groups");
            check(manager.dispatch_enumerate_physical_device_groups({}, &count, nullptr) == error,
                  "1.1 groups: count-query errors are propagated");
        }
        enumerate_status = VK_SUCCESS;
        native_device_count = 0;
        count = 4;
        check(manager.dispatch_enumerate_physical_device_groups({}, &count, groups.data()) == VK_SUCCESS && !count,
              "1.1 groups: an empty native inventory has no synthetic GPU");
        native_device_count = 3;
    };
    auto test_sparse = [&] {
        VkImageSparseMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2};
        info.image = (VkImage)uint64_t(0xc14001);
        uint32_t count = 0;
        manager.dispatch_get_image_sparse_memory_requirements2(device, &info, &count, nullptr);
        check(count == 2, "1.1 sparse: count query reaches legacy sparse residency inventory");
        VkBaseOutStructure sentinel{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        std::array<VkSparseImageMemoryRequirements2, 3> output{};
        for (auto& o : output) {
            o.sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2;
            o.pNext = &sentinel;
            o.memoryRequirements.imageMipTailSize = 123;
        }
        count = 0;
        legacy_sparse_calls = 0;
        manager.dispatch_get_image_sparse_memory_requirements2(device, &info, &count, output.data());
        check(count == 0 && !legacy_sparse_calls && output[0].memoryRequirements.imageMipTailSize == 123,
              "1.1 sparse: zero capacity cannot become a count query or read an empty vector");
        count = 1;
        manager.dispatch_get_image_sparse_memory_requirements2(device, &info, &count, output.data());
        check(count == 1 && output[0].memoryRequirements.imageMipTailSize == 8192 &&
                  output[0].pNext == &sentinel && output[1].memoryRequirements.imageMipTailSize == 123,
              "1.1 sparse: truncated payload preserves pNext and the unwritten tail");
        count = 3;
        manager.dispatch_get_image_sparse_memory_requirements2(device, &info, &count, output.data());
        check(count == 2 && output[1].memoryRequirements.imageMipTailSize == 8193 &&
                  output[1].pNext == &sentinel && output[2].memoryRequirements.imageMipTailSize == 123,
              "1.1 sparse: full payload preserves caller-owned headers and output capacity");
    };
    // Exercise the central fallback first, then repeat with registered modules.
    test_groups();
    test_sparse();
    manager.register_module(std::make_unique<ScopedModule<BindMemory2Module>>());
    manager.register_module(std::make_unique<ScopedModule<DeviceGroupModule>>());
    manager.register_module(std::make_unique<ScopedModule<GetMemoryRequirements2Module>>());
    manager.register_module(std::make_unique<ScopedModule<MaintenanceModule>>());
    test_groups();
    test_sparse();
    VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice created{};
    check(manager.dispatch_create_device(physical, &create_info, nullptr, &created) == VK_SUCCESS && created == device,
          "1.1 routing: create a legacy device with the actual core modules enabled");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandBufferCount = 1;
    VkCommandBuffer allocated{};
    check(manager.dispatch_allocate_command_buffers(device, &allocation, &allocated) == VK_SUCCESS && allocated == command,
          "1.1 routing: establish command-buffer ownership for native alias forwarding");
    trims = masks = 0;
    manager.dispatch_trim_command_pool(device, {}, 0);
    manager.dispatch_cmd_set_device_mask(command, 1);
    check(trims == 1, "1.1 trim: native KHR alias is called exactly once through module and manager");
    check(masks == 1, "1.1 device mask: native KHR alias is called exactly once through module and manager");
    VkDeviceQueueInfo2 queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    queue_info.queueFamilyIndex = 0;
    queue_info.queueIndex = 0;
    VkQueue queue = VK_NULL_HANDLE;
    queue2_calls = queue1_calls = 0;
    manager.dispatch_get_device_queue2(device, &queue_info, &queue);
    check(queue == legacy_queue && queue2_calls == 1 && queue1_calls == 1,
          "1.1 device queue2: a null flags==0 result falls back to vkGetDeviceQueue");
    modern_queue = (VkQueue)(uintptr_t)0xc16002;
    queue = VK_NULL_HANDLE;
    queue2_calls = queue1_calls = 0;
    manager.dispatch_get_device_queue2(device, &queue_info, &queue);
    check(queue == legacy_queue && queue2_calls == 1 && queue1_calls == 1,
          "1.1 device queue2: a foreign flags==0 handle is normalized to vkGetDeviceQueue");
    modern_queue = VK_NULL_HANDLE;
    VkBindBufferMemoryInfo bind_buffer_info{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
    bind_buffer_info.buffer = (VkBuffer)(uintptr_t)0xc17001;
    bind_buffer_info.memory = (VkDeviceMemory)(uintptr_t)0xc17002;
    uint32_t remote_index = 1;
    VkBindBufferMemoryDeviceGroupInfo bind_buffer_group{
        VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO};
    bind_buffer_group.deviceIndexCount = 1;
    bind_buffer_group.pDeviceIndices = &remote_index;
    bind_buffer_info.pNext = &bind_buffer_group;
    buffer_binds = 0;
    check(manager.dispatch_bind_buffer_memory2(device, 1, &bind_buffer_info) ==
              VK_ERROR_FEATURE_NOT_PRESENT && buffer_binds == 0,
          "1.1 bind memory2: multi-device buffer bind is rejected before any partial bind");
    remote_index = 0;
    check(manager.dispatch_bind_buffer_memory2(device, 1, &bind_buffer_info) == VK_SUCCESS &&
              buffer_binds == 1,
          "1.1 bind memory2: local index zero still binds through the legacy entry point");
    VkBindImageMemoryInfo bind_image_info{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    bind_image_info.image = (VkImage)(uintptr_t)0xc17003;
    bind_image_info.memory = (VkDeviceMemory)(uintptr_t)0xc17004;
    VkBindImageMemoryDeviceGroupInfo bind_image_group{
        VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO};
    VkRect2D split{};
    split.extent = {1, 1};
    bind_image_group.splitInstanceBindRegionCount = 1;
    bind_image_group.pSplitInstanceBindRegions = &split;
    bind_image_info.pNext = &bind_image_group;
    image_binds = 0;
    check(manager.dispatch_bind_image_memory2(device, 1, &bind_image_info) ==
              VK_ERROR_FEATURE_NOT_PRESENT && image_binds == 0,
          "1.1 bind memory2: split image bind is rejected before any partial bind");
    bind_image_info.pNext = nullptr;
    check(manager.dispatch_bind_image_memory2(device, 1, &bind_image_info) == VK_SUCCESS &&
              image_binds == 1,
          "1.1 bind memory2: plain image bind uses the legacy entry point");
    VkMemoryDedicatedRequirements dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    dedicated.prefersDedicatedAllocation = VK_TRUE;
    dedicated.requiresDedicatedAllocation = VK_TRUE;
    VkMemoryRequirements2 buffer_requirements_out{
        VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
    VkBufferMemoryRequirementsInfo2 buffer_requirements_info{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
    buffer_requirements_info.buffer = (VkBuffer)(uintptr_t)0xc18001;
    manager.dispatch_get_buffer_memory_requirements2(device, &buffer_requirements_info,
                                                     &buffer_requirements_out);
    check(buffer_requirements_out.memoryRequirements.size == 4096 &&
              buffer_requirements_out.memoryRequirements.alignment == 64 &&
              !dedicated.prefersDedicatedAllocation &&
              !dedicated.requiresDedicatedAllocation,
          "1.1 memory requirements2: buffer fallback preserves the legacy payload and clears absent dedicated support");
    VkMemoryDedicatedRequirements image_dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    image_dedicated.prefersDedicatedAllocation = VK_TRUE;
    VkMemoryRequirements2 image_requirements_out{
        VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &image_dedicated};
    VkImageMemoryRequirementsInfo2 image_requirements_info{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    image_requirements_info.image = (VkImage)(uintptr_t)0xc18002;
    manager.dispatch_get_image_memory_requirements2(device, &image_requirements_info,
                                                    &image_requirements_out);
    check(image_requirements_out.memoryRequirements.size == 8192 &&
              image_requirements_out.memoryRequirements.alignment == 128 &&
              !image_dedicated.prefersDedicatedAllocation,
          "1.1 memory requirements2: image fallback preserves the legacy payload and clears absent dedicated support");
    native_reqs2 = true;
    VkMemoryRequirements2 repaired_buffer{
        VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    manager.dispatch_get_buffer_memory_requirements2(device, &buffer_requirements_info,
                                                     &repaired_buffer);
    check(repaired_buffer.memoryRequirements.size == 4096 &&
              repaired_buffer.memoryRequirements.alignment == 64,
          "1.1 memory requirements2: a wrong native buffer result is repaired from the legacy query");
    VkMemoryRequirements2 repaired_image{
        VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    manager.dispatch_get_image_memory_requirements2(device, &image_requirements_info,
                                                    &repaired_image);
    check(repaired_image.memoryRequirements.size == 8192 &&
              repaired_image.memoryRequirements.alignment == 128,
          "1.1 memory requirements2: a wrong native image result is repaired from the legacy query");
    native_reqs2 = false;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    VkDescriptorSetVariableDescriptorCountLayoutSupport variable{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT};
    VkDescriptorSetLayoutSupport support{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT, &variable};
    layout_creates = layout_destroys = 0;
    manager.dispatch_get_descriptor_set_layout_support(device, &layout_info, &support);
    check(support.supported && layout_creates == 1 && layout_destroys == 1 &&
              support.pNext == &variable && variable.maxVariableDescriptorCount == 0,
          "1.1 layout support: real native creation is evidence, without fabricated indexing limits");
    // The mock's physical limits are all zero. Pipeline-layout/per-stage limits
    // must not incorrectly reject a successfully represented descriptor layout.
    check(binding.descriptorCount == 64 && layout_info.pBindings == &binding,
          "1.1 layout support: query preserves the application's layout definition");
    layout_status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    support.supported = VK_TRUE;
    manager.dispatch_get_descriptor_set_layout_support(device, &layout_info, &support);
    check(!support.supported && layout_creates == 2 && layout_destroys == 1,
          "1.1 layout support: native rejection is reported without leaking or destroying a failed handle");
    layout_status = VK_SUCCESS;
    native_layout_query = true;
    manager.dispatch_get_descriptor_set_layout_support(device, &layout_info, &support);
    check(!support.supported && variable.maxVariableDescriptorCount == 37 && layout_creates == 2,
          "1.1 layout support: native KHR query and variable limit take precedence over allocation fallback");
    native_layout_query = false;
    VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &multiview};
    manager.dispatch_get_physical_device_features2(physical, &features);
    check(!multiview.multiview && features.pNext == &multiview,
          "1.1 multiview: singleton device groups cannot invent layered-rendering support");
    DeviceGroupModule independent_group;
    multiview.multiview = VK_TRUE;
    create_info.pNext = &multiview;
    std::vector<const char*> extensions{VK_KHR_MULTIVIEW_EXTENSION_NAME};
    void* user = nullptr;
    independent_group.on_pre_create_device(physical, &create_info, nullptr, extensions, user);
    check(create_info.pNext == &multiview && multiview.multiview &&
              extensions.size() == 1 && !strcmp(extensions[0], VK_KHR_MULTIVIEW_EXTENSION_NAME),
          "1.1 multiview: native multiview enablement survives device-group emulation");
    manager.dispatch_destroy_device(device, nullptr);
    active = false;
}

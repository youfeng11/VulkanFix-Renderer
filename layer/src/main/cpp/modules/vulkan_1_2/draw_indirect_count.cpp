#include "draw_indirect_count.h"

#include "layer_manager.h"
#include "driver_loader.h"
#include "indirect_count_shader.h"

#include <spirv-tools/libspirv.h>

#include <algorithm>
#include <cstring>

REGISTER_LAYER_MODULE(DrawIndirectCountModule);

namespace {

bool assemble_shader(const char* source, std::vector<uint32_t>& words) {
    if (source == nullptr) return false;
    spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_0);
    if (context == nullptr) return false;

    spv_binary binary = nullptr;
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t result = spvTextToBinary(
        context, source, strlen(source), &binary, &diagnostic);
    if (result != SPV_SUCCESS || binary == nullptr) {
        LOGE("DrawIndirectCount: shader assembly failed: %s",
             diagnostic && diagnostic->error ? diagnostic->error : "(no diagnostic)");
        if (diagnostic) spvDiagnosticDestroy(diagnostic);
        spvContextDestroy(context);
        return false;
    }

    words.assign(binary->code, binary->code + binary->wordCount);
    spvBinaryDestroy(binary);
    if (diagnostic) spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    return !words.empty();
}

const std::vector<uint32_t>& draw_shader_words() {
    static const std::vector<uint32_t> words = [] {
        std::vector<uint32_t> result;
        assemble_shader(kIndirectCountDrawShader, result);
        return result;
    }();
    return words;
}

const std::vector<uint32_t>& indexed_shader_words() {
    static const std::vector<uint32_t> words = [] {
        std::vector<uint32_t> result;
        assemble_shader(kIndirectCountIndexedShader, result);
        return result;
    }();
    return words;
}

}  // namespace

DrawIndirectCountModule::DrawIndirectCountModule()
    : ExtensionModuleBase(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME,
                          VK_KHR_DRAW_INDIRECT_COUNT_SPEC_VERSION,
                          VK_API_VERSION_1_2) {
    LOGI("DrawIndirectCountModule initialized");
}

void DrawIndirectCountModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    if (is_phys_device_native(physicalDevice)) return;
    // No valid emulation inside a render pass; do not advertise.
    LOGI("DrawIndirectCount: extension not advertised (no valid in-render-pass emulation)");
    (void)extensions;
}

void DrawIndirectCountModule::on_pre_create_device_custom(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    (void)pCreateInfo;
    (void)pEnabledFeatures;
    (void)pUserData;
    if (!is_phys_device_native(physicalDevice)) return;
    for (const char* extension : enabledExtensions) {
        if (extension != nullptr &&
            strcmp(extension, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME) == 0) {
            return;
        }
    }
    enabledExtensions.push_back(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
}

bool DrawIndirectCountModule::on_cmd_draw_indirect_count(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (device == VK_NULL_HANDLE) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (is_device_native(device)) {
        auto fn = (PFN_vkCmdDrawIndirectCountKHR)
            get_real_proc(get_last_instance(), device, "vkCmdDrawIndirectCountKHR");
        if (fn) {
            fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return true;
        }
        if (dt.CmdDrawIndirectCount) {
            dt.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset,
                                    maxDrawCount, stride);
            return true;
        }
        LOGW("vkCmdDrawIndirectCount: native entry point missing");
        return true;
    }
    if (dt.CmdDrawIndirectCount) {
        dt.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset,
                                maxDrawCount, stride);
        return true;
    }

    if (maxDrawCount == 0) return true;
    // Not advertised. Even if an application still calls it, the compute
    // expansion cannot be synchronized inside a render pass (graphics-stage
    // barrier rule), so drop the call instead of recording invalid commands.
    (void)buffer;
    (void)offset;
    (void)countBuffer;
    (void)countBufferOffset;
    (void)stride;
    LOGW("vkCmdDrawIndirectCount: no native support; call dropped");
    return false;
}

bool DrawIndirectCountModule::on_cmd_draw_indexed_indirect_count(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (device == VK_NULL_HANDLE) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (is_device_native(device)) {
        auto fn = (PFN_vkCmdDrawIndexedIndirectCountKHR)
            get_real_proc(get_last_instance(), device, "vkCmdDrawIndexedIndirectCountKHR");
        if (fn) {
            fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return true;
        }
        if (dt.CmdDrawIndexedIndirectCount) {
            dt.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset,
                                           maxDrawCount, stride);
            return true;
        }
        LOGW("vkCmdDrawIndexedIndirectCount: native entry point missing");
        return true;
    }
    if (dt.CmdDrawIndexedIndirectCount) {
        dt.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset,
                                       maxDrawCount, stride);
        return true;
    }

    if (maxDrawCount == 0) return true;
    (void)buffer;
    (void)offset;
    (void)countBuffer;
    (void)countBufferOffset;
    (void)stride;
    LOGW("vkCmdDrawIndexedIndirectCount: no native support; call dropped");
    return false;
}

DrawIndirectCountModule::DeviceResources* DrawIndirectCountModule::ensure_device_locked(VkDevice device) {
    if (device == VK_NULL_HANDLE) return nullptr;
    const uint64_t key = (uint64_t)(uintptr_t)device;
    auto it = m_devices.find(key);
    if (it != m_devices.end()) {
        return it->second.ready ? &it->second : nullptr;
    }

    DeviceResources& dr = m_devices[key];
    dr.device = device;
    VkInstance instance = get_last_instance();

#define VKFIX_LOAD(field, name) dr.field = (decltype(dr.field)) get_real_proc(instance, device, name)
    VKFIX_LOAD(CreateBuffer, "vkCreateBuffer");
    VKFIX_LOAD(DestroyBuffer, "vkDestroyBuffer");
    VKFIX_LOAD(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    VKFIX_LOAD(AllocateMemory, "vkAllocateMemory");
    VKFIX_LOAD(FreeMemory, "vkFreeMemory");
    VKFIX_LOAD(BindBufferMemory, "vkBindBufferMemory");
    VKFIX_LOAD(CreateDescriptorPool, "vkCreateDescriptorPool");
    VKFIX_LOAD(DestroyDescriptorPool, "vkDestroyDescriptorPool");
    VKFIX_LOAD(AllocateDescriptorSets, "vkAllocateDescriptorSets");
    VKFIX_LOAD(ResetDescriptorPool, "vkResetDescriptorPool");
    VKFIX_LOAD(UpdateDescriptorSets, "vkUpdateDescriptorSets");
    VKFIX_LOAD(CmdBindPipeline, "vkCmdBindPipeline");
    VKFIX_LOAD(CmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    VKFIX_LOAD(CmdPushConstants, "vkCmdPushConstants");
    VKFIX_LOAD(CmdDispatch, "vkCmdDispatch");
    VKFIX_LOAD(CmdPipelineBarrier, "vkCmdPipelineBarrier");
    VKFIX_LOAD(DestroyPipeline, "vkDestroyPipeline");
    VKFIX_LOAD(DestroyPipelineLayout, "vkDestroyPipelineLayout");
    VKFIX_LOAD(DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
#undef VKFIX_LOAD
    dr.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
        get_real_proc(instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties");
    dr.physical = LayerManager::get().get_physical_device_for(device);

    auto create_shader = (PFN_vkCreateShaderModule)
        get_real_proc(instance, device, "vkCreateShaderModule");
    auto destroy_shader = (PFN_vkDestroyShaderModule)
        get_real_proc(instance, device, "vkDestroyShaderModule");
    auto create_set_layout = (PFN_vkCreateDescriptorSetLayout)
        get_real_proc(instance, device, "vkCreateDescriptorSetLayout");
    auto create_pipeline_layout = (PFN_vkCreatePipelineLayout)
        get_real_proc(instance, device, "vkCreatePipelineLayout");
    auto create_pipelines = (PFN_vkCreateComputePipelines)
        get_real_proc(instance, device, "vkCreateComputePipelines");

    const std::vector<uint32_t>& draw_words = draw_shader_words();
    const std::vector<uint32_t>& indexed_words = indexed_shader_words();
    if (!dr.CreateBuffer || !dr.AllocateMemory || !dr.BindBufferMemory ||
        !dr.CreateDescriptorPool || !dr.AllocateDescriptorSets || !dr.UpdateDescriptorSets ||
        !dr.CmdBindPipeline || !dr.CmdBindDescriptorSets || !dr.CmdPushConstants ||
        !dr.CmdDispatch || !dr.CmdPipelineBarrier ||
        !create_shader || !create_set_layout || !create_pipeline_layout || !create_pipelines ||
        draw_words.empty() || indexed_words.empty()) {
        LOGE("DrawIndirectCount: required Vulkan functions or shaders unavailable");
        return nullptr;
    }

    VkShaderModule draw_module = VK_NULL_HANDLE;
    VkShaderModule indexed_module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = draw_words.size() * sizeof(uint32_t);
    module_info.pCode = draw_words.data();
    if (create_shader(device, &module_info, nullptr, &draw_module) != VK_SUCCESS) {
        LOGE("DrawIndirectCount: vkCreateShaderModule(draw) failed");
        return nullptr;
    }
    module_info.codeSize = indexed_words.size() * sizeof(uint32_t);
    module_info.pCode = indexed_words.data();
    if (create_shader(device, &module_info, nullptr, &indexed_module) != VK_SUCCESS) {
        LOGE("DrawIndirectCount: vkCreateShaderModule(indexed) failed");
        if (destroy_shader) destroy_shader(device, draw_module, nullptr);
        return nullptr;
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 3;
    set_layout_info.pBindings = bindings;
    if (create_set_layout(device, &set_layout_info, nullptr, &dr.set_layout) != VK_SUCCESS) {
        LOGE("DrawIndirectCount: vkCreateDescriptorSetLayout failed");
        if (destroy_shader) {
            destroy_shader(device, draw_module, nullptr);
            destroy_shader(device, indexed_module, nullptr);
        }
        return nullptr;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = 5 * sizeof(uint32_t);
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &dr.set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (create_pipeline_layout(device, &pipeline_layout_info, nullptr, &dr.pipeline_layout) != VK_SUCCESS) {
        LOGE("DrawIndirectCount: vkCreatePipelineLayout failed");
        if (destroy_shader) {
            destroy_shader(device, draw_module, nullptr);
            destroy_shader(device, indexed_module, nullptr);
        }
        if (dr.DestroyDescriptorSetLayout) dr.DestroyDescriptorSetLayout(device, dr.set_layout, nullptr);
        dr.set_layout = VK_NULL_HANDLE;
        return nullptr;
    }

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = dr.pipeline_layout;
    pipeline_info.stage.module = draw_module;
    if (create_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &dr.draw_pipeline) != VK_SUCCESS) {
        LOGE("DrawIndirectCount: draw compute pipeline creation failed");
        if (destroy_shader) {
            destroy_shader(device, draw_module, nullptr);
            destroy_shader(device, indexed_module, nullptr);
        }
        if (dr.DestroyPipelineLayout) dr.DestroyPipelineLayout(device, dr.pipeline_layout, nullptr);
        if (dr.DestroyDescriptorSetLayout) dr.DestroyDescriptorSetLayout(device, dr.set_layout, nullptr);
        dr.pipeline_layout = VK_NULL_HANDLE;
        dr.set_layout = VK_NULL_HANDLE;
        return nullptr;
    }
    pipeline_info.stage.module = indexed_module;
    if (create_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &dr.indexed_pipeline) != VK_SUCCESS) {
        LOGE("DrawIndirectCount: indexed compute pipeline creation failed");
        if (destroy_shader) {
            destroy_shader(device, draw_module, nullptr);
            destroy_shader(device, indexed_module, nullptr);
        }
        if (dr.DestroyPipeline) dr.DestroyPipeline(device, dr.draw_pipeline, nullptr);
        if (dr.DestroyPipelineLayout) dr.DestroyPipelineLayout(device, dr.pipeline_layout, nullptr);
        if (dr.DestroyDescriptorSetLayout) dr.DestroyDescriptorSetLayout(device, dr.set_layout, nullptr);
        dr.draw_pipeline = VK_NULL_HANDLE;
        dr.pipeline_layout = VK_NULL_HANDLE;
        dr.set_layout = VK_NULL_HANDLE;
        return nullptr;
    }
    if (destroy_shader) {
        destroy_shader(device, draw_module, nullptr);
        destroy_shader(device, indexed_module, nullptr);
    }

    dr.ready = true;
    LOGI("DrawIndirectCount: compute expansion resources ready for device %p", (void*)device);
    return &dr;
}

DrawIndirectCountModule::CmdResources& DrawIndirectCountModule::ensure_cmd_locked(
    VkDevice device, VkCommandBuffer commandBuffer) {
    CmdResources& cmd = m_cmds[(uint64_t)(uintptr_t)commandBuffer];
    cmd.device = device;
    return cmd;
}

bool DrawIndirectCountModule::ensure_scratch_locked(
    DeviceResources& device,
    CmdResources& cmd,
    VkDeviceSize size,
    VkBuffer& out_buffer,
    VkDeviceSize& out_offset
) {
    if (size == 0) return false;
    constexpr VkDeviceSize kChunkSize = 256 * 1024;
    if (cmd.scratch.empty() || cmd.current >= cmd.scratch.size() ||
        cmd.offset + size > cmd.scratch[cmd.current].capacity) {
        const VkDeviceSize capacity = std::max(kChunkSize, size);
        ScratchBuffer scratch{};
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = capacity;
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (device.CreateBuffer(cmd.device, &buffer_info, nullptr, &scratch.buffer) != VK_SUCCESS) {
            LOGE("DrawIndirectCount: scratch vkCreateBuffer failed (%llu bytes)",
                 (unsigned long long)capacity);
            return false;
        }

        VkMemoryRequirements requirements{};
        device.GetBufferMemoryRequirements(cmd.device, scratch.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        if (device.GetPhysicalDeviceMemoryProperties && device.physical != VK_NULL_HANDLE) {
            device.GetPhysicalDeviceMemoryProperties(device.physical, &memory_properties);
        }
        uint32_t memory_type = UINT32_MAX;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                memory_type = i;
                break;
            }
        }
        if (memory_type == UINT32_MAX) {
            for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
                if ((requirements.memoryTypeBits & (1u << i)) != 0) {
                    memory_type = i;
                    break;
                }
            }
        }
        if (memory_type == UINT32_MAX) memory_type = 0;

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = memory_type;
        if (device.AllocateMemory(cmd.device, &allocate_info, nullptr, &scratch.memory) != VK_SUCCESS) {
            LOGE("DrawIndirectCount: scratch vkAllocateMemory failed");
            if (device.DestroyBuffer) device.DestroyBuffer(cmd.device, scratch.buffer, nullptr);
            return false;
        }
        if (device.BindBufferMemory(cmd.device, scratch.buffer, scratch.memory, 0) != VK_SUCCESS) {
            LOGE("DrawIndirectCount: scratch vkBindBufferMemory failed");
            if (device.FreeMemory) device.FreeMemory(cmd.device, scratch.memory, nullptr);
            if (device.DestroyBuffer) device.DestroyBuffer(cmd.device, scratch.buffer, nullptr);
            return false;
        }
        scratch.capacity = capacity;
        cmd.scratch.push_back(scratch);
        cmd.current = cmd.scratch.size() - 1;
        cmd.offset = 0;
    }

    out_buffer = cmd.scratch[cmd.current].buffer;
    out_offset = cmd.offset;
    cmd.offset += size;
    return true;
}

VkDescriptorSet DrawIndirectCountModule::acquire_set_locked(
    DeviceResources& device, CmdResources& cmd) {
    constexpr uint32_t kSetsPerPool = 64;

    auto create_pool = [&]() -> bool {
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = 3 * kSetsPerPool;
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = kSetsPerPool;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (device.CreateDescriptorPool(cmd.device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
            return false;
        }
        cmd.pools.push_back(pool);
        cmd.pool_allocated = 0;
        return true;
    };

    if (cmd.pools.empty() || cmd.pool_allocated >= kSetsPerPool) {
        if (!create_pool()) {
            LOGE("DrawIndirectCount: vkCreateDescriptorPool failed");
            return VK_NULL_HANDLE;
        }
    }

    auto allocate_from_last = [&]() -> VkDescriptorSet {
        VkDescriptorSetAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = cmd.pools.back();
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts = &device.set_layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (device.AllocateDescriptorSets(cmd.device, &allocate_info, &set) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return set;
    };

    VkDescriptorSet set = allocate_from_last();
    if (set == VK_NULL_HANDLE) {
        if (!create_pool()) return VK_NULL_HANDLE;
        set = allocate_from_last();
        if (set == VK_NULL_HANDLE) {
            LOGE("DrawIndirectCount: vkAllocateDescriptorSets failed");
            return VK_NULL_HANDLE;
        }
    }
    ++cmd.pool_allocated;
    return set;
}

void DrawIndirectCountModule::emulate(
    VkDevice device,
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride,
    bool indexed
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    DeviceResources* resources = ensure_device_locked(device);
    if (resources == nullptr) {
        LOGW("DrawIndirectCount: emulation resources unavailable, call skipped");
        return;
    }
    CmdResources& cmd = ensure_cmd_locked(device, commandBuffer);

    const VkDeviceSize size = (VkDeviceSize)maxDrawCount * stride;
    VkBuffer scratch_buffer = VK_NULL_HANDLE;
    VkDeviceSize scratch_offset = 0;
    if (!ensure_scratch_locked(*resources, cmd, size, scratch_buffer, scratch_offset)) {
        LOGE("DrawIndirectCount: scratch allocation failed (%llu bytes)", (unsigned long long)size);
        return;
    }

    VkDescriptorSet set = acquire_set_locked(*resources, cmd);
    if (set == VK_NULL_HANDLE) return;

    VkDescriptorBufferInfo infos[3]{};
    infos[0].buffer = buffer;
    infos[0].range = VK_WHOLE_SIZE;
    infos[1].buffer = scratch_buffer;
    infos[1].range = VK_WHOLE_SIZE;
    infos[2].buffer = countBuffer;
    infos[2].range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    resources->UpdateDescriptorSets(device, 3, writes, 0, nullptr);

    const uint32_t push_constants[5] = {
        (uint32_t)(offset / 4),
        (uint32_t)(countBufferOffset / 4),
        (uint32_t)(scratch_offset / 4),
        maxDrawCount,
        (uint32_t)(stride / 4),
    };
    resources->CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                               indexed ? resources->indexed_pipeline : resources->draw_pipeline);
    resources->CmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     resources->pipeline_layout, 0, 1, &set, 0, nullptr);
    resources->CmdPushConstants(commandBuffer, resources->pipeline_layout,
                                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), push_constants);

    // The application's barriers target DRAW_INDIRECT, not COMPUTE_SHADER: make
    // earlier writes to the indirect/count buffers visible to the expansion
    // shader before it reads them.
    VkMemoryBarrier pre_barrier{};
    pre_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    pre_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    pre_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    resources->CmdPipelineBarrier(commandBuffer,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0, 1, &pre_barrier, 0, nullptr, 0, nullptr);

    resources->CmdDispatch(commandBuffer, (maxDrawCount + 63) / 64, 1, 1);

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    resources->CmdPipelineBarrier(commandBuffer,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                  0, 1, &barrier, 0, nullptr, 0, nullptr);

    if (indexed) {
        LayerManager::get().dispatch_cmd_draw_indexed_indirect(
            commandBuffer, scratch_buffer, scratch_offset, maxDrawCount, stride);
    } else {
        LayerManager::get().dispatch_cmd_draw_indirect(
            commandBuffer, scratch_buffer, scratch_offset, maxDrawCount, stride);
    }
}

void DrawIndirectCountModule::on_free_command_buffers(
    VkDevice device, uint32_t count, const VkCommandBuffer* pCommandBuffers) {
    if (pCommandBuffers == nullptr) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        auto it = m_cmds.find((uint64_t)(uintptr_t)pCommandBuffers[i]);
        if (it != m_cmds.end()) {
            destroy_cmd_locked(it->second);
            m_cmds.erase(it);
        }
    }
    (void)device;
}

void DrawIndirectCountModule::on_begin_command_buffer(
    VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo) {
    (void)pBeginInfo;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cmds.find((uint64_t)(uintptr_t)commandBuffer);
    if (it != m_cmds.end()) reset_cmd_locked(it->second);
}

void DrawIndirectCountModule::on_reset_command_buffer(
    VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cmds.find((uint64_t)(uintptr_t)commandBuffer);
    if (it != m_cmds.end()) reset_cmd_locked(it->second);
}

void DrawIndirectCountModule::reset_cmd_locked(CmdResources& cmd) {
    auto device_it = m_devices.find((uint64_t)(uintptr_t)cmd.device);
    if (device_it != m_devices.end() && device_it->second.ResetDescriptorPool) {
        for (VkDescriptorPool pool : cmd.pools) {
            device_it->second.ResetDescriptorPool(cmd.device, pool, 0);
        }
    }
    cmd.current = 0;
    cmd.offset = 0;
    cmd.pool_allocated = 0;
}

void DrawIndirectCountModule::destroy_cmd_locked(CmdResources& cmd) {
    auto device_it = m_devices.find((uint64_t)(uintptr_t)cmd.device);
    if (device_it != m_devices.end()) {
        DeviceResources& device = device_it->second;
        for (const ScratchBuffer& scratch : cmd.scratch) {
            if (device.FreeMemory && scratch.memory != VK_NULL_HANDLE) {
                device.FreeMemory(cmd.device, scratch.memory, nullptr);
            }
            if (device.DestroyBuffer && scratch.buffer != VK_NULL_HANDLE) {
                device.DestroyBuffer(cmd.device, scratch.buffer, nullptr);
            }
        }
        for (VkDescriptorPool pool : cmd.pools) {
            if (device.DestroyDescriptorPool && pool != VK_NULL_HANDLE) {
                device.DestroyDescriptorPool(cmd.device, pool, nullptr);
            }
        }
    }
    cmd.scratch.clear();
    cmd.pools.clear();
    cmd.current = 0;
    cmd.offset = 0;
    cmd.pool_allocated = 0;
}

void DrawIndirectCountModule::destroy_device_locked(DeviceResources& device) {
    if (device.DestroyPipeline && device.draw_pipeline != VK_NULL_HANDLE) {
        device.DestroyPipeline(device.device, device.draw_pipeline, nullptr);
    }
    if (device.DestroyPipeline && device.indexed_pipeline != VK_NULL_HANDLE) {
        device.DestroyPipeline(device.device, device.indexed_pipeline, nullptr);
    }
    if (device.DestroyPipelineLayout && device.pipeline_layout != VK_NULL_HANDLE) {
        device.DestroyPipelineLayout(device.device, device.pipeline_layout, nullptr);
    }
    if (device.DestroyDescriptorSetLayout && device.set_layout != VK_NULL_HANDLE) {
        device.DestroyDescriptorSetLayout(device.device, device.set_layout, nullptr);
    }
    device.draw_pipeline = VK_NULL_HANDLE;
    device.indexed_pipeline = VK_NULL_HANDLE;
    device.pipeline_layout = VK_NULL_HANDLE;
    device.set_layout = VK_NULL_HANDLE;
    device.ready = false;
}

void DrawIndirectCountModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_cmds.begin(); it != m_cmds.end();) {
        if (it->second.device == device) {
            destroy_cmd_locked(it->second);
            it = m_cmds.erase(it);
        } else {
            ++it;
        }
    }
    auto device_it = m_devices.find((uint64_t)(uintptr_t)device);
    if (device_it != m_devices.end()) {
        destroy_device_locked(device_it->second);
        m_devices.erase(device_it);
    }
    ExtensionModuleBase::on_destroy_device(device);
}

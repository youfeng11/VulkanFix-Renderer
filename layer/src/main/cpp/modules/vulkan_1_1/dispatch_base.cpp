#include "dispatch_base.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "native_instance_state.h"
#include "shader/shader_rewriter.h"
#include "shader_module_cache.h"
#include "vk_common.h"

#include <cstdlib>
#include <cstring>
#include <vector>

REGISTER_LAYER_MODULE(DispatchBaseModule);

namespace {
uint64_t pipeline_key(VkDevice device, VkPipeline pipeline) {
    return (uint64_t)(uintptr_t)device * 0x9e3779b97f4a7c15ull ^
           (uint64_t)(uintptr_t)pipeline;
}
uint64_t variant_key(VkDevice device, VkPipeline pipeline, const uint32_t base[3]) {
    uint64_t key = pipeline_key(device, pipeline);
    key ^= (uint64_t(base[0]) * 0x9e3779b97f4a7c15ull);
    key ^= (uint64_t(base[1]) << 21);
    key ^= (uint64_t(base[2]) << 42);
    return key;
}
} // namespace

bool DispatchBaseModule::native_support(VkPhysicalDevice physicalDevice) const {
    const char *force = getenv("VULKAN_FIX_EMULATE_DISPATCH_BASE");
    if (force && (!strcmp(force, "1") || strcasecmp(force, "true") == 0 ||
                  strcasecmp(force, "force") == 0))
        return false;
    VkInstance instance = vkfix::native_instance_for_physical(physicalDevice);
    if (instance == VK_NULL_HANDLE) instance = get_last_instance();
    auto properties = (PFN_vkGetPhysicalDeviceProperties)get_real_proc(
        instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (properties) {
        VkPhysicalDeviceProperties props{};
        properties(physicalDevice, &props);
        if (props.apiVersion >= VK_API_VERSION_1_1) return true;
    }
    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)get_real_proc(
        instance, VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    if (!enumerate) return false;
    uint32_t count = 0;
    if (enumerate(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS || !count)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (enumerate(physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    for (const auto &extension : extensions)
        if (!strcmp(extension.extensionName, "VK_KHR_device_group")) return true;
    return false;
}

void DispatchBaseModule::on_post_create_device(VkPhysicalDevice physicalDevice,
                                               VkDevice device, VkResult result,
                                               void *) {
    if (result != VK_SUCCESS || device == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_emulate[pipeline_key(device, VK_NULL_HANDLE)] = !native_support(physicalDevice);
}

bool DispatchBaseModule::is_emulating(VkDevice device) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_emulate.find(pipeline_key(device, VK_NULL_HANDLE));
    return it != m_emulate.end() && it->second;
}

void DispatchBaseModule::on_post_create_compute_pipelines(
    VkDevice device, uint32_t count, const VkComputePipelineCreateInfo *pCreateInfos,
    const VkPipeline *pPipelines) {
    if (!pCreateInfos || !pPipelines) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        if (!pPipelines[i]) continue;
        const auto &ci = pCreateInfos[i];
        ComputeInfo info;
        info.layout = ci.layout;
        info.flags = ci.flags;
        info.stage = ci.stage.stage;
        info.module = ci.stage.module;
        if (ci.stage.pName) info.entry = ci.stage.pName;
        if (ci.stage.pSpecializationInfo) {
            const auto *spec = ci.stage.pSpecializationInfo;
            info.entries.assign(spec->pMapEntries, spec->pMapEntries + spec->mapEntryCount);
            if (spec->pData && spec->dataSize)
                info.data.assign(static_cast<const uint8_t *>(spec->pData),
                                 static_cast<const uint8_t *>(spec->pData) + spec->dataSize);
        }
        m_infos[pipeline_key(device, pPipelines[i])] = std::move(info);
    }
}

void DispatchBaseModule::on_destroy_pipeline(VkDevice device, VkPipeline pipeline) {
    auto destroy = (PFN_vkDestroyPipeline)get_real_proc(
        get_last_instance(), device, "vkDestroyPipeline");
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t owner = pipeline_key(device, pipeline);
    for (auto it = m_variant_owner.begin(); it != m_variant_owner.end();) {
        if (it->second == owner) {
            auto found = m_variants.find(it->first);
            if (found != m_variants.end()) {
                if (destroy) destroy(device, found->second, nullptr);
                m_variants.erase(found);
                m_variant_device.erase(it->first);
            }
            it = m_variant_owner.erase(it);
        } else {
            ++it;
        }
    }
    m_infos.erase(owner);
}

void DispatchBaseModule::on_destroy_device(VkDevice device) {
    auto destroy = (PFN_vkDestroyPipeline)get_real_proc(
        get_last_instance(), device, "vkDestroyPipeline");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_emulate.erase(pipeline_key(device, VK_NULL_HANDLE));
    if (destroy)
        for (const auto &entry : m_variants)
            if (m_variant_device[entry.first] == device)
                destroy(device, entry.second, nullptr);
    m_variants.clear();
    m_variant_owner.clear();
    m_variant_device.clear();
    m_infos.clear();
    m_cmd_pipeline.clear();
}

void DispatchBaseModule::on_begin_command_buffer(VkCommandBuffer commandBuffer,
                                                  const VkCommandBufferBeginInfo *) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cmd_pipeline.erase((uint64_t)(uintptr_t)commandBuffer);
}

void DispatchBaseModule::on_reset_command_buffer(VkCommandBuffer commandBuffer,
                                                  VkCommandBufferResetFlags) {
    on_begin_command_buffer(commandBuffer, nullptr);
}

void DispatchBaseModule::on_cmd_bind_pipeline(VkCommandBuffer commandBuffer,
                                               VkPipelineBindPoint bindPoint,
                                               VkPipeline pipeline) {
    if (bindPoint != VK_PIPELINE_BIND_POINT_COMPUTE) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cmd_pipeline[(uint64_t)(uintptr_t)commandBuffer] = pipeline;
}

VkPipeline DispatchBaseModule::variant_for(VkDevice device, VkPipeline pipeline,
                                            const uint32_t base[3], bool *no_offset) {
    if (no_offset) *no_offset = false;
    const uint64_t key = variant_key(device, pipeline, base);
    ComputeInfo info;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_variants.find(key);
        if (found != m_variants.end()) return found->second;
        auto info_it = m_infos.find(pipeline_key(device, pipeline));
        if (info_it == m_infos.end()) return VK_NULL_HANDLE;
        info = info_it->second;
    }

    std::vector<uint32_t> code;
    if (!vkfix::ShaderModuleCache::get().copy(device, info.module, code)) return VK_NULL_HANDLE;
    if (!ShaderRewriter::get().offset_invocation_ids(code, base[0], base[1], base[2])) {
        if (no_offset && !ShaderRewriter::uses_invocation_ids(code)) *no_offset = true;
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = code.size() * sizeof(uint32_t);
    shader_ci.pCode = code.data();
    auto create_shader = (PFN_vkCreateShaderModule)get_real_proc(
        get_last_instance(), device, "vkCreateShaderModule");
    auto destroy_shader = (PFN_vkDestroyShaderModule)get_real_proc(
        get_last_instance(), device, "vkDestroyShaderModule");
    auto create_pipeline = (PFN_vkCreateComputePipelines)get_real_proc(
        get_last_instance(), device, "vkCreateComputePipelines");
    if (!create_shader || !create_pipeline) return VK_NULL_HANDLE;
    VkShaderModule patched = VK_NULL_HANDLE;
    if (create_shader(device, &shader_ci, nullptr, &patched) != VK_SUCCESS || !patched)
        return VK_NULL_HANDLE;

    VkSpecializationInfo spec{};
    if (!info.entries.empty() || !info.data.empty()) {
        spec.mapEntryCount = static_cast<uint32_t>(info.entries.size());
        spec.pMapEntries = info.entries.data();
        spec.dataSize = info.data.size();
        spec.pData = info.data.data();
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.flags = info.flags & ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = patched;
    ci.stage.pName = info.entry.c_str();
    if (spec.mapEntryCount || spec.dataSize) ci.stage.pSpecializationInfo = &spec;
    ci.layout = info.layout;
    ci.basePipelineHandle = VK_NULL_HANDLE;
    ci.basePipelineIndex = -1;
    VkPipeline variant = VK_NULL_HANDLE;
    const VkResult result = create_pipeline(device, VK_NULL_HANDLE, 1, &ci, nullptr, &variant);
    if (destroy_shader) destroy_shader(device, patched, nullptr);
    if (result != VK_SUCCESS || !variant) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_variants[key] = variant;
    m_variant_owner[key] = pipeline_key(device, pipeline);
    m_variant_device[key] = device;
    return variant;
}

bool DispatchBaseModule::on_cmd_dispatch_base(VkCommandBuffer commandBuffer,
                                               uint32_t baseGroupX, uint32_t baseGroupY,
                                               uint32_t baseGroupZ, uint32_t groupCountX,
                                               uint32_t groupCountY, uint32_t groupCountZ) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (!is_emulating(device)) return false;
    auto dispatch = (PFN_vkCmdDispatch)get_real_proc(
        get_last_instance(), device, "vkCmdDispatch");
    if (!dispatch) return false;
    if (baseGroupX == 0 && baseGroupY == 0 && baseGroupZ == 0) {
        dispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
        return true;
    }
    VkPipeline original = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cmd_pipeline.find((uint64_t)(uintptr_t)commandBuffer);
        if (it == m_cmd_pipeline.end()) return false;
        original = it->second;
    }
    const uint32_t base[3] = {baseGroupX, baseGroupY, baseGroupZ};
    bool no_offset = false;
    VkPipeline variant = variant_for(device, original, base, &no_offset);
    if (!variant) {
        if (no_offset) {
            dispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
            return true;
        }
        return false;
    }
    auto bind = (PFN_vkCmdBindPipeline)get_real_proc(
        get_last_instance(), device, "vkCmdBindPipeline");
    if (!bind) return false;
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, variant);
    dispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, original);
    return true;
}

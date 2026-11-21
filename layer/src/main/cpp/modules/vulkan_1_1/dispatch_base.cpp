#include "dispatch_base.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "shader/shader_rewriter.h"
#include "shader_module_cache.h"
#include "vk_common.h"

#include <cstdlib>
#include <cstring>
#include <vector>

REGISTER_LAYER_MODULE(DispatchBaseModule);

void DispatchBaseModule::set_native_route(VkDevice device, bool available) {
    const char *force = getenv("VULKAN_FIX_EMULATE_DISPATCH_BASE");
    if (force && (!strcmp(force, "1") || strcasecmp(force, "true") == 0 ||
                  strcasecmp(force, "force") == 0))
        available = false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_emulate[device] = !available;
}

void DispatchBaseModule::on_post_create_device(VkPhysicalDevice physicalDevice,
                                               VkDevice device, VkResult result,
                                               void *) {
    if (result != VK_SUCCESS || device == VK_NULL_HANDLE) return;
    // Conservative until LayerManager installs the actual enabled route.
    set_native_route(device, false);
}

bool DispatchBaseModule::is_emulating(VkDevice device) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_emulate.find(device);
    return it != m_emulate.end() && it->second;
}

bool DispatchBaseModule::is_device_native(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_emulate.find(device);
    return it != m_emulate.end() && !it->second;
}

DispatchBaseModule::RetainedLayout::~RetainedLayout() {
    if (handle)
        if (auto destroy = (PFN_vkDestroyPipelineLayout)get_real_proc(get_last_instance(), device, "vkDestroyPipelineLayout"))
            destroy(device, handle, nullptr);
}
void DispatchBaseModule::on_post_create_pipeline_layout(VkDevice device, const VkPipelineLayoutCreateInfo* info,
                                                        VkResult result, VkPipelineLayout handle) {
    if (result != VK_SUCCESS || !info || !handle || !is_emulating(device)) return;
    auto retained = std::make_shared<RetainedLayout>();
    retained->device = device;
    if (auto create = (PFN_vkCreatePipelineLayout)get_real_proc(get_last_instance(), device, "vkCreatePipelineLayout"))
        retained->result = create(device, info, nullptr, &retained->handle);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_layouts[Key::make(device, handle)] = std::move(retained);
}
void DispatchBaseModule::on_destroy_pipeline_layout(VkDevice device, VkPipelineLayout handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_layouts.erase(Key::make(device, handle));
}
void DispatchBaseModule::on_modify_compute_pipeline_create_info(VkDevice device, VkComputePipelineCreateInfo& info) {
    if (is_emulating(device)) info.flags &= ~VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
}
void DispatchBaseModule::on_post_create_compute_pipelines(VkDevice device, uint32_t count,
    const VkComputePipelineCreateInfo* infos, const VkPipeline* pipelines) {
    if (!infos || !pipelines || !is_emulating(device)) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        if (!pipelines[i]) continue;
        const auto& ci = infos[i];
        ComputeInfo info;
        auto layout = m_layouts.find(Key::make(device, ci.layout));
        if (layout != m_layouts.end()) info.layout = layout->second;
        info.flags = ci.flags;
        info.stage_flags = ci.stage.flags;
        // A shader module and its numeric handle may be destroyed/reused as
        // soon as the application has created its pipeline. Own these bytes.
        info.code = vkfix::ShaderModuleCache::get().acquire(device, ci.stage.module);
        if (ci.stage.pName) info.entry = ci.stage.pName;
        if (ci.stage.pSpecializationInfo) {
            const auto* spec = ci.stage.pSpecializationInfo;
            if (spec->mapEntryCount)
                info.entries.assign(spec->pMapEntries, spec->pMapEntries + spec->mapEntryCount);
            if (spec->dataSize)
                info.data.assign(static_cast<const uint8_t*>(spec->pData), static_cast<const uint8_t*>(spec->pData) + spec->dataSize);
        }
        m_infos[Key::make(device, pipelines[i])] = std::move(info);
    }
}
void DispatchBaseModule::on_destroy_pipeline(VkDevice device, VkPipeline pipeline) {
    auto destroy = (PFN_vkDestroyPipeline)get_real_proc(get_last_instance(), device, "vkDestroyPipeline");
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto owner = Key::make(device, pipeline);
    for (auto it = m_variants.begin(); it != m_variants.end();) {
        if (it->first.pipeline == owner) {
            if (destroy) destroy(device, it->second, nullptr);
            it = m_variants.erase(it);
        } else ++it;
    }
    m_infos.erase(owner);
}
void DispatchBaseModule::on_destroy_device(VkDevice device) {
    auto destroy = (PFN_vkDestroyPipeline)get_real_proc(get_last_instance(), device, "vkDestroyPipeline");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_emulate.erase(device);
    for (auto it = m_variants.begin(); it != m_variants.end();) {
        if (it->first.pipeline.device == device) {
            if (destroy) destroy(device, it->second, nullptr);
            it = m_variants.erase(it);
        } else ++it;
    }
    for (auto it = m_infos.begin(); it != m_infos.end();)
        if (it->first.device == device) it = m_infos.erase(it); else ++it;
    for (auto it = m_layouts.begin(); it != m_layouts.end();)
        if (it->first.device == device) it = m_layouts.erase(it); else ++it;
    for (auto it = m_cmd_pipeline.begin(); it != m_cmd_pipeline.end();) {
        if (it->second.device == device) { m_errors.erase(it->first); it = m_cmd_pipeline.erase(it); }
        else ++it;
    }
}
void DispatchBaseModule::on_begin_command_buffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo*) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cmd_pipeline.erase(commandBuffer);
    m_errors.erase(commandBuffer);
}
void DispatchBaseModule::on_reset_command_buffer(VkCommandBuffer cmd, VkCommandBufferResetFlags) {
    on_begin_command_buffer(cmd, nullptr);
}
void DispatchBaseModule::on_free_command_buffers(VkDevice, uint32_t count, const VkCommandBuffer* commands) {
    if (commands) for (uint32_t i = 0; i < count; ++i) on_begin_command_buffer(commands[i], nullptr);
}
VkResult DispatchBaseModule::command_recording_error(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_errors.find(cmd);
    return found == m_errors.end() ? VK_SUCCESS : found->second;
}
void DispatchBaseModule::on_cmd_bind_pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (point != VK_PIPELINE_BIND_POINT_COMPUTE) return;
    const auto device = LayerManager::get().get_device_for_cmd(cmd);
    if (!is_emulating(device)) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cmd_pipeline[cmd] = Key::make(device, pipeline);
}
VkPipeline DispatchBaseModule::variant_for(VkDevice device, VkPipeline pipeline,
    const uint32_t base[3], bool* no_offset, VkResult& result) {
    if (no_offset) *no_offset = false;
    result = VK_ERROR_UNKNOWN;
    const VariantKey key{Key::make(device, pipeline), {base[0], base[1], base[2]}};
    ComputeInfo info;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto cached = m_variants.find(key);
        if (cached != m_variants.end()) { result = VK_SUCCESS; return cached->second; }
        auto found = m_infos.find(key.pipeline);
        if (found == m_infos.end()) return VK_NULL_HANDLE;
        info = found->second;
    }
    if (!info.code || info.code->empty()) return VK_NULL_HANDLE;
    auto code = *info.code;
    if (!ShaderRewriter::get().offset_invocation_ids(code, base[0], base[1], base[2], info.entry.c_str(), no_offset)) {
        if (no_offset && *no_offset) result = VK_SUCCESS;
        return VK_NULL_HANDLE;
    }
    if (!info.layout || info.layout->result != VK_SUCCESS) {
        if (info.layout) result = info.layout->result;
        return VK_NULL_HANDLE;
    }
    auto create_shader = (PFN_vkCreateShaderModule)get_real_proc(get_last_instance(), device, "vkCreateShaderModule");
    auto destroy_shader = (PFN_vkDestroyShaderModule)get_real_proc(get_last_instance(), device, "vkDestroyShaderModule");
    auto create_pipeline = (PFN_vkCreateComputePipelines)get_real_proc(get_last_instance(), device, "vkCreateComputePipelines");
    auto destroy_pipeline = (PFN_vkDestroyPipeline)get_real_proc(get_last_instance(), device, "vkDestroyPipeline");
    if (!create_shader || !destroy_shader || !create_pipeline || !destroy_pipeline) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader.codeSize = code.size() * sizeof(uint32_t);
    shader.pCode = code.data();
    VkShaderModule module{};
    result = create_shader(device, &shader, nullptr, &module);
    if (result != VK_SUCCESS) return VK_NULL_HANDLE;
    VkSpecializationInfo spec{uint32_t(info.entries.size()), info.entries.data(), info.data.size(), info.data.data()};
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.flags = info.flags & ~(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_DISPATCH_BASE_BIT);
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.flags = info.stage_flags;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = info.entry.c_str();
    if (spec.mapEntryCount || spec.dataSize) ci.stage.pSpecializationInfo = &spec;
    ci.layout = info.layout->handle;
    ci.basePipelineIndex = -1;
    VkPipeline variant{};
    result = create_pipeline(device, VK_NULL_HANDLE, 1, &ci, nullptr, &variant);
    destroy_shader(device, module, nullptr);
    if (result != VK_SUCCESS) { if (variant) destroy_pipeline(device, variant, nullptr); return VK_NULL_HANDLE; }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto inserted = m_variants.emplace(key, variant);
    if (!inserted.second) { destroy_pipeline(device, variant, nullptr); return inserted.first->second; }
    return variant;
}
bool DispatchBaseModule::on_cmd_dispatch_base(VkCommandBuffer cmd, uint32_t bx, uint32_t by,
    uint32_t bz, uint32_t gx, uint32_t gy, uint32_t gz) {
    const auto device = LayerManager::get().get_device_for_cmd(cmd);
    if (!is_emulating(device)) return false;
    if (command_recording_error(cmd) != VK_SUCCESS) return true;
    auto dispatch = (PFN_vkCmdDispatch)get_real_proc(get_last_instance(), device, "vkCmdDispatch");
    auto bind = (PFN_vkCmdBindPipeline)get_real_proc(get_last_instance(), device, "vkCmdBindPipeline");
    if (!dispatch || !bind) return false;
    if (!(bx | by | bz)) { dispatch(cmd, gx, gy, gz); return true; }
    VkPipeline original{};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_cmd_pipeline.find(cmd);
        if (found == m_cmd_pipeline.end()) return false;
        original = (VkPipeline)found->second.handle;
    }
    const uint32_t base[]{bx, by, bz};
    bool no_offset = false;
    VkResult result;
    VkPipeline variant = variant_for(device, original, base, &no_offset, result);
    if (!variant && !no_offset) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_errors[cmd] = result;
        return true;
    }
    if (variant) bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, variant);
    dispatch(cmd, gx, gy, gz);
    if (variant) bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, original);
    return true;
}

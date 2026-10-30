#include "negative_viewport.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "shader/shader_rewriter.h"
#include "shader_module_cache.h"
#include "native_instance_state.h"
#include "vk_common.h"

#include <cstdlib>
#include <cstring>
#include <vector>

REGISTER_LAYER_MODULE(NegativeViewportModule);

namespace {
uint64_t pipeline_key(VkDevice device, VkPipeline pipeline) {
    return (uint64_t)(uintptr_t)device * 0x9e3779b97f4a7c15ull ^
           (uint64_t)(uintptr_t)pipeline;
}
bool uses_dynamic_viewport(const VkGraphicsPipelineCreateInfo &createInfo) {
    const auto *dynamic = createInfo.pDynamicState;
    if (!dynamic || !dynamic->pDynamicStates) return false;
    for (uint32_t i = 0; i < dynamic->dynamicStateCount; ++i) {
        if (dynamic->pDynamicStates[i] == VK_DYNAMIC_STATE_VIEWPORT ||
            dynamic->pDynamicStates[i] == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT)
            return true;
    }
    return false;
}
} // namespace

bool NegativeViewportModule::native_support(VkPhysicalDevice physicalDevice) const {
    const char *force = getenv("VULKAN_FIX_EMULATE_NEGATIVE_VIEWPORT");
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
    for (const auto &extension : extensions) {
        if (!strcmp(extension.extensionName, "VK_KHR_maintenance1") ||
            !strcmp(extension.extensionName, "VK_AMD_negative_viewport_height"))
            return true;
    }
    return false;
}

bool NegativeViewportModule::has_negative_static_viewport(
    const VkGraphicsPipelineCreateInfo &createInfo) const {
    if (!createInfo.pViewportState || !createInfo.pViewportState->pViewports ||
        createInfo.pViewportState->viewportCount == 0 || uses_dynamic_viewport(createInfo))
        return false;
    for (uint32_t i = 0; i < createInfo.pViewportState->viewportCount; ++i)
        if (createInfo.pViewportState->pViewports[i].height < 0.0f) return true;
    return false;
}

bool NegativeViewportModule::has_dynamic_viewport(
    const VkGraphicsPipelineCreateInfo &createInfo) const {
    return createInfo.pViewportState && uses_dynamic_viewport(createInfo) &&
           createInfo.pStages && createInfo.stageCount > 0;
}

void NegativeViewportModule::on_post_create_device(VkPhysicalDevice physicalDevice,
                                                   VkDevice device, VkResult result,
                                                   void *) {
    if (result != VK_SUCCESS || device == VK_NULL_HANDLE) return;
    // Use the layer's deterministic viewport path even on drivers that report
    // maintenance1: the self-check exercises negative viewport slots that some
    // drivers ignore, and the same path also covers 1.0 drivers.
    const bool emulate = true;
    LOGI("NegativeViewport: post device emulate=%d native=%d", int(emulate),
         int(native_support(physicalDevice)));
    std::lock_guard<std::mutex> lock(m_mutex);
    m_emulate[(uint64_t)(uintptr_t)device] = emulate;
}

bool NegativeViewportModule::is_device_emulating(VkDevice device) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_emulate.find((uint64_t)(uintptr_t)device);
    return it != m_emulate.end() && it->second;
}

void NegativeViewportModule::on_destroy_device(VkDevice device) {
    auto destroy = (PFN_vkDestroyShaderModule)get_real_proc(
        get_last_instance(), device, "vkDestroyShaderModule");
    auto destroy_pipeline = (PFN_vkDestroyPipeline)get_real_proc(
        get_last_instance(), device, "vkDestroyPipeline");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_emulate.erase((uint64_t)(uintptr_t)device);
    if (destroy_pipeline)
        for (const auto &entry : m_variants)
            destroy_pipeline(device, entry.second, nullptr);
    m_variants.clear();
    m_pipeline_uses_viewport_index.clear();
    m_pipeline_constant_viewport.clear();
    m_cmd_pipeline.clear();
    m_cmd_negative.clear();
    m_cmd_positive.clear();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->second.device == device) {
            if (destroy && it->second.module) destroy(device, it->second.module, nullptr);
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

bool NegativeViewportModule::needs_pipeline_interception(
    VkDevice device, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo *pCreateInfos) {
    if (!device || !pCreateInfos) return false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_emulate.find((uint64_t)(uintptr_t)device);
        if (it == m_emulate.end() || !it->second) return false;
    }
    for (uint32_t i = 0; i < createInfoCount; ++i)
        if (has_negative_static_viewport(pCreateInfos[i]) ||
            has_dynamic_viewport(pCreateInfos[i]))
            return true;
    return false;
}

void NegativeViewportModule::on_modify_pipeline_create_info(
    VkDevice device, uint32_t, VkGraphicsPipelineCreateInfo &createInfo,
    VkPipelineVertexInputStateCreateInfo &, std::vector<void *> &allocationsToFree) {
    const bool static_negative = has_negative_static_viewport(createInfo);
    const bool dynamic = has_dynamic_viewport(createInfo);
    if ((!static_negative && !dynamic) || !createInfo.pStages || createInfo.stageCount == 0)
        return;
    int vertex_stage = -1;
    for (uint32_t i = 0; i < createInfo.stageCount; ++i)
        if (createInfo.pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT) {
            vertex_stage = int(i);
            break;
        }
    if (vertex_stage < 0) return;

    std::vector<uint32_t> code;
    if (!vkfix::ShaderModuleCache::get().copy(
            device, createInfo.pStages[vertex_stage].module, code))
        return;

    auto adjust_negative_viewports = [&]() -> bool {
        auto *viewport_state = (VkPipelineViewportStateCreateInfo *)malloc(
            sizeof(VkPipelineViewportStateCreateInfo));
        auto *viewports = (VkViewport *)malloc(
            sizeof(VkViewport) * createInfo.pViewportState->viewportCount);
        if (!viewport_state || !viewports) {
            if (viewport_state) free(viewport_state);
            if (viewports) free(viewports);
            return false;
        }
        *viewport_state = *createInfo.pViewportState;
        memcpy(viewports, createInfo.pViewportState->pViewports,
               sizeof(VkViewport) * createInfo.pViewportState->viewportCount);
        for (uint32_t i = 0; i < createInfo.pViewportState->viewportCount; ++i) {
            if (viewports[i].height < 0.0f) {
                viewports[i].y += viewports[i].height;
                viewports[i].height = -viewports[i].height;
            }
        }
        viewport_state->pViewports = viewports;
        createInfo.pViewportState = viewport_state;
        allocationsToFree.push_back(viewport_state);
        allocationsToFree.push_back(viewports);
        return true;
    };

    if (!dynamic && static_negative) {
        bool all_negative = true;
        for (uint32_t i = 0; i < createInfo.pViewportState->viewportCount; ++i)
            if (createInfo.pViewportState->pViewports[i].height >= 0.0f)
                all_negative = false;
        const bool uses_index = ShaderRewriter::uses_viewport_index(code);
        if (uses_index && !all_negative) {
            uint32_t constant_index = 0;
            if (!ShaderRewriter::constant_viewport_index(code, constant_index) ||
                constant_index >= createInfo.pViewportState->viewportCount)
                return; // cannot split by a dynamic viewport index
            if (createInfo.pViewportState->pViewports[constant_index].height >= 0.0f) {
                adjust_negative_viewports();
                return;
            }
        }
        if (!uses_index &&
            createInfo.pViewportState->pViewports[0].height >= 0.0f) {
            // Without gl_ViewportIndex only viewport 0 is sampled. Keep the
            // shader untouched and make the unused negative slots valid.
            adjust_negative_viewports();
            return;
        }
    }

    if (!ShaderRewriter::get().flip_position_y(code)) return;

    auto *stages = (VkPipelineShaderStageCreateInfo *)malloc(
        sizeof(VkPipelineShaderStageCreateInfo) * createInfo.stageCount);
    if (!stages) return;
    memcpy(stages, createInfo.pStages,
           sizeof(VkPipelineShaderStageCreateInfo) * createInfo.stageCount);
    allocationsToFree.push_back(stages);
    createInfo.pStages = stages;

    VkShaderModuleCreateInfo module_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_ci.codeSize = code.size() * sizeof(uint32_t);
    module_ci.pCode = code.data();
    auto create = (PFN_vkCreateShaderModule)get_real_proc(
        get_last_instance(), device, "vkCreateShaderModule");
    if (!create) return;
    VkShaderModule patched = VK_NULL_HANDLE;
    if (create(device, &module_ci, nullptr, &patched) != VK_SUCCESS || !patched) return;
    if (dynamic) {
        const bool uses_index = ShaderRewriter::uses_viewport_index(code);
        uint32_t constant_viewport = UINT32_MAX;
        if (uses_index) ShaderRewriter::constant_viewport_index(code, constant_viewport);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending[&createInfo] = {device, patched, true, uses_index, constant_viewport};
        return;
    }

    auto *viewport_state = (VkPipelineViewportStateCreateInfo *)malloc(
        sizeof(VkPipelineViewportStateCreateInfo));
    auto *viewports = (VkViewport *)malloc(
        sizeof(VkViewport) * createInfo.pViewportState->viewportCount);
    if (!viewport_state || !viewports) {
        if (viewport_state) free(viewport_state);
        if (viewports) free(viewports);
        auto destroy = (PFN_vkDestroyShaderModule)get_real_proc(
            get_last_instance(), device, "vkDestroyShaderModule");
        if (destroy) destroy(device, patched, nullptr);
        return;
    }
    *viewport_state = *createInfo.pViewportState;
    memcpy(viewports, createInfo.pViewportState->pViewports,
           sizeof(VkViewport) * createInfo.pViewportState->viewportCount);
    for (uint32_t i = 0; i < createInfo.pViewportState->viewportCount; ++i) {
        if (viewports[i].height < 0.0f) {
            viewports[i].y += viewports[i].height;
            viewports[i].height = -viewports[i].height;
        }
    }
    viewport_state->pViewports = viewports;
    createInfo.pViewportState = viewport_state;
    stages[vertex_stage].module = patched;
    allocationsToFree.push_back(viewport_state);
    allocationsToFree.push_back(viewports);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending[&createInfo] = {device, patched};
}

void NegativeViewportModule::on_post_create_graphics_pipelines(
    VkDevice device, uint32_t count, const VkGraphicsPipelineCreateInfo *pCreateInfos,
    const VkPipeline *pPipelines) {
    if (!pCreateInfos) return;
    auto destroy = (PFN_vkDestroyShaderModule)get_real_proc(
        get_last_instance(), device, "vkDestroyShaderModule");
    auto create = (PFN_vkCreateGraphicsPipelines)get_real_proc(
        get_last_instance(), device, "vkCreateGraphicsPipelines");
    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        auto it = m_pending.find(&pCreateInfos[i]);
        if (it == m_pending.end()) continue;
        const Pending pending = it->second;
        m_pending.erase(it);
        if (pending.dynamic_variant && pPipelines && pPipelines[i] && create) {
            VkGraphicsPipelineCreateInfo ci = pCreateInfos[i];
            ci.basePipelineHandle = VK_NULL_HANDLE;
            ci.basePipelineIndex = -1;
            auto *stages = (VkPipelineShaderStageCreateInfo *)malloc(
                sizeof(VkPipelineShaderStageCreateInfo) * ci.stageCount);
            if (stages) {
                memcpy(stages, ci.pStages,
                       sizeof(VkPipelineShaderStageCreateInfo) * ci.stageCount);
                for (uint32_t s = 0; s < ci.stageCount; ++s)
                    if (stages[s].stage == VK_SHADER_STAGE_VERTEX_BIT) {
                        stages[s].module = pending.module;
                        break;
                    }
                ci.pStages = stages;
                VkPipeline variant = VK_NULL_HANDLE;
                const VkResult variant_result =
                    create(device, VK_NULL_HANDLE, 1, &ci, nullptr, &variant);
                if (variant_result != VK_SUCCESS)
                    LOGW("NegativeViewport: dynamic variant creation failed (%d)",
                         int(variant_result));
                if (variant_result == VK_SUCCESS && variant)
                    m_variants[pipeline_key(device, pPipelines[i])] = variant;
                m_pipeline_uses_viewport_index[pipeline_key(device, pPipelines[i])] =
                    pending.uses_viewport_index;
                m_pipeline_constant_viewport[pipeline_key(device, pPipelines[i])] =
                    pending.constant_viewport;
                free(stages);
            }
        }
        if (destroy && pending.module) destroy(device, pending.module, nullptr);
    }
}

void NegativeViewportModule::on_destroy_pipeline(VkDevice device, VkPipeline pipeline) {
    auto destroy = (PFN_vkDestroyPipeline)get_real_proc(
        get_last_instance(), device, "vkDestroyPipeline");
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto key = pipeline_key(device, pipeline);
    m_pipeline_uses_viewport_index.erase(key);
    m_pipeline_constant_viewport.erase(key);
    auto it = m_variants.find(key);
    if (it == m_variants.end()) return;
    if (destroy) destroy(device, it->second, nullptr);
    m_variants.erase(it);
}

void NegativeViewportModule::on_begin_command_buffer(VkCommandBuffer commandBuffer,
                                                      const VkCommandBufferBeginInfo *) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cmd_pipeline.erase((uint64_t)(uintptr_t)commandBuffer);
    m_cmd_negative.erase((uint64_t)(uintptr_t)commandBuffer);
    m_cmd_positive.erase((uint64_t)(uintptr_t)commandBuffer);
}

void NegativeViewportModule::on_reset_command_buffer(VkCommandBuffer commandBuffer,
                                                      VkCommandBufferResetFlags) {
    on_begin_command_buffer(commandBuffer, nullptr);
}

void NegativeViewportModule::on_cmd_bind_pipeline(VkCommandBuffer commandBuffer,
                                                   VkPipelineBindPoint bindPoint,
                                                   VkPipeline pipeline) {
    if (bindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cmd_pipeline[(uint64_t)(uintptr_t)commandBuffer] = pipeline;
}

bool NegativeViewportModule::on_cmd_set_viewport(VkCommandBuffer commandBuffer,
                                                  uint32_t firstViewport, uint32_t count,
                                                  const VkViewport *pViewports) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (!is_device_emulating(device) || !pViewports) return false;
    std::vector<VkViewport> adjusted(pViewports, pViewports + count);
    std::set<uint32_t> negative_slots;
    std::set<uint32_t> positive_slots;
    for (uint32_t i = 0; i < count; ++i) {
        auto &viewport = adjusted[i];
        if (viewport.height < 0.0f) {
            viewport.y += viewport.height;
            viewport.height = -viewport.height;
            negative_slots.insert(firstViewport + i);
        } else {
            positive_slots.insert(firstViewport + i);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto key = (uint64_t)(uintptr_t)commandBuffer;
        auto current = m_cmd_pipeline.find(key);
        if (current != m_cmd_pipeline.end()) {
            const auto pkey = pipeline_key(device, current->second);
            const bool uses_index = m_pipeline_uses_viewport_index[pkey];
            const uint32_t constant = m_pipeline_constant_viewport[pkey];
            if (uses_index && constant == UINT32_MAX) {
                auto negatives = m_cmd_negative[key];
                auto positives = m_cmd_positive[key];
                for (uint32_t i = 0; i < count; ++i) {
                    negatives.erase(firstViewport + i);
                    positives.erase(firstViewport + i);
                }
                negatives.insert(negative_slots.begin(), negative_slots.end());
                positives.insert(positive_slots.begin(), positive_slots.end());
                if (!negatives.empty() && !positives.empty()) return false;
            }
        }
    }
    auto real = (PFN_vkCmdSetViewport)get_real_proc(
        get_last_instance(), device, "vkCmdSetViewport");
    if (!real) return false;
    real(commandBuffer, firstViewport, count, adjusted.data());
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto key = (uint64_t)(uintptr_t)commandBuffer;
    auto &negatives = m_cmd_negative[key];
    auto &positives = m_cmd_positive[key];
    for (uint32_t i = 0; i < count; ++i) {
        negatives.erase(firstViewport + i);
        positives.erase(firstViewport + i);
    }
    negatives.insert(negative_slots.begin(), negative_slots.end());
    positives.insert(positive_slots.begin(), positive_slots.end());
    return true;
}

bool NegativeViewportModule::bind_variant(VkCommandBuffer commandBuffer, VkDevice &device,
                                           VkPipeline &original, VkPipeline &variant) {
    device = LayerManager::get().get_device_for_cmd(commandBuffer);
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto key = (uint64_t)(uintptr_t)commandBuffer;
    auto neg = m_cmd_negative.find(key);
    if (neg == m_cmd_negative.end() || neg->second.empty()) return false;
    auto current = m_cmd_pipeline.find(key);
    if (current == m_cmd_pipeline.end()) return false;
    original = current->second;
    auto found = m_variants.find(pipeline_key(device, original));
    if (found == m_variants.end()) return false;
    const bool uses_index = m_pipeline_uses_viewport_index[pipeline_key(device, original)];
    if (uses_index) {
        const uint32_t constant = m_pipeline_constant_viewport[pipeline_key(device, original)];
        if (constant != UINT32_MAX) {
            if (!neg->second.count(constant)) return false;
        } else {
            auto pos = m_cmd_positive.find(key);
            if (pos != m_cmd_positive.end() && !pos->second.empty()) return false;
        }
    } else if (!neg->second.count(0)) {
        return false; // only viewport 0 is sampled without gl_ViewportIndex
    }
    variant = found->second;
    return true;
}

bool NegativeViewportModule::on_cmd_draw(VkCommandBuffer commandBuffer,
                                          uint32_t vertexCount, uint32_t instanceCount,
                                          uint32_t firstVertex, uint32_t firstInstance) {
    VkDevice device = VK_NULL_HANDLE;
    VkPipeline original = VK_NULL_HANDLE;
    VkPipeline variant = VK_NULL_HANDLE;
    if (!bind_variant(commandBuffer, device, original, variant)) return false;
    auto bind = (PFN_vkCmdBindPipeline)get_real_proc(
        get_last_instance(), device, "vkCmdBindPipeline");
    auto draw = (PFN_vkCmdDraw)get_real_proc(
        get_last_instance(), device, "vkCmdDraw");
    if (!bind || !draw) return false;
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, variant);
    draw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, original);
    return true;
}

bool NegativeViewportModule::on_cmd_draw_indexed(
    VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    VkDevice device = VK_NULL_HANDLE;
    VkPipeline original = VK_NULL_HANDLE;
    VkPipeline variant = VK_NULL_HANDLE;
    if (!bind_variant(commandBuffer, device, original, variant)) return false;
    auto bind = (PFN_vkCmdBindPipeline)get_real_proc(
        get_last_instance(), device, "vkCmdBindPipeline");
    auto draw = (PFN_vkCmdDrawIndexed)get_real_proc(
        get_last_instance(), device, "vkCmdDrawIndexed");
    if (!bind || !draw) return false;
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, variant);
    draw(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, original);
    return true;
}

bool NegativeViewportModule::on_cmd_draw_indirect(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t drawCount, uint32_t stride) {
    VkDevice device = VK_NULL_HANDLE;
    VkPipeline original = VK_NULL_HANDLE, variant = VK_NULL_HANDLE;
    if (!bind_variant(commandBuffer, device, original, variant)) return false;
    auto bind = (PFN_vkCmdBindPipeline)get_real_proc(
        get_last_instance(), device, "vkCmdBindPipeline");
    auto draw = (PFN_vkCmdDrawIndirect)get_real_proc(
        get_last_instance(), device, "vkCmdDrawIndirect");
    if (!bind || !draw) return false;
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, variant);
    draw(commandBuffer, buffer, offset, drawCount, stride);
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, original);
    return true;
}

bool NegativeViewportModule::on_cmd_draw_indexed_indirect(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t drawCount, uint32_t stride) {
    VkDevice device = VK_NULL_HANDLE;
    VkPipeline original = VK_NULL_HANDLE, variant = VK_NULL_HANDLE;
    if (!bind_variant(commandBuffer, device, original, variant)) return false;
    auto bind = (PFN_vkCmdBindPipeline)get_real_proc(
        get_last_instance(), device, "vkCmdBindPipeline");
    auto draw = (PFN_vkCmdDrawIndexedIndirect)get_real_proc(
        get_last_instance(), device, "vkCmdDrawIndexedIndirect");
    if (!bind || !draw) return false;
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, variant);
    draw(commandBuffer, buffer, offset, drawCount, stride);
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, original);
    return true;
}

bool NegativeViewportModule::on_cmd_draw_indirect_count(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
    uint32_t stride) {
    VkDevice device = VK_NULL_HANDLE;
    VkPipeline original = VK_NULL_HANDLE, variant = VK_NULL_HANDLE;
    if (!bind_variant(commandBuffer, device, original, variant)) return false;
    auto bind = (PFN_vkCmdBindPipeline)get_real_proc(
        get_last_instance(), device, "vkCmdBindPipeline");
    auto draw = (PFN_vkCmdDrawIndirectCount)get_real_proc(
        get_last_instance(), device, "vkCmdDrawIndirectCount");
    if (!draw)
        draw = (PFN_vkCmdDrawIndirectCount)get_real_proc(
            get_last_instance(), device, "vkCmdDrawIndirectCountKHR");
    if (!bind || !draw) return false;
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, variant);
    draw(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, original);
    return true;
}

bool NegativeViewportModule::on_cmd_draw_indexed_indirect_count(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
    uint32_t stride) {
    VkDevice device = VK_NULL_HANDLE;
    VkPipeline original = VK_NULL_HANDLE, variant = VK_NULL_HANDLE;
    if (!bind_variant(commandBuffer, device, original, variant)) return false;
    auto bind = (PFN_vkCmdBindPipeline)get_real_proc(
        get_last_instance(), device, "vkCmdBindPipeline");
    auto draw = (PFN_vkCmdDrawIndexedIndirectCount)get_real_proc(
        get_last_instance(), device, "vkCmdDrawIndexedIndirectCount");
    if (!draw)
        draw = (PFN_vkCmdDrawIndexedIndirectCount)get_real_proc(
            get_last_instance(), device, "vkCmdDrawIndexedIndirectCountKHR");
    if (!bind || !draw) return false;
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, variant);
    draw(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    bind(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, original);
    return true;
}

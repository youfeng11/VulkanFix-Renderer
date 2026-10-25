#include "push_descriptor.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_debug_utils.h"
#include "vk_pnext.h"
#include <algorithm>
#include <numeric>
#include <stdlib.h>
#include <string.h>

REGISTER_LAYER_MODULE(PushDescriptorModule);

PushDescriptorModule::PushDescriptorModule() {
    LOGI("Initialized Vulkan VK_KHR_push_descriptor module (incremental snapshots)");
}

bool PushDescriptorModule::is_phys_device_native(VkPhysicalDevice physDev) {
    {
        std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
        auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
        if (it != m_phys_native_support.end()) {
            return it->second;
        }
    }

    EmulationMode mode =
        parse_emulation_mode("VULKAN_FIX_EMULATE_PUSH_DESCRIPTOR", "FORCE_EMULATE_PUSH_DESCRIPTOR",
                             "emulate_push_descriptor_ext");
    if (mode == EmulationMode::ForceEmulate) {
        LOGI("Push Descriptor: FORCE EMULATE enabled for physical device %p", physDev);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }
    if (mode == EmulationMode::Skip) {
        LOGI("Push Descriptor: SKIP/BYPASS enabled for physical device %p", physDev);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = true;
        return true;
    }

    PFN_vkEnumerateDeviceExtensionProperties real_fn =
        (PFN_vkEnumerateDeviceExtensionProperties)get_real_proc(
            get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    bool native = false;
    if (real_fn) {
        uint32_t count = 0;
        if (real_fn(physDev, NULL, &count, NULL) == VK_SUCCESS && count > 0) {
            std::vector<VkExtensionProperties> exts(count);
            if (real_fn(physDev, NULL, &count, exts.data()) == VK_SUCCESS) {
                for (const auto &e : exts) {
                    if (strcmp(e.extensionName, "VK_KHR_push_descriptor") == 0) {
                        native = true;
                        break;
                    }
                }
            }
        }
    }

    // Workaround for ARM Mali GPU driver crash:
    // ARM Mali proprietary Vulkan drivers (vendor ID 0x13B5) advertise VK_KHR_push_descriptor,
    // but suffer from a fatal driver crash (SIGSEGV at offset 0x64) in libGLES_mali.so during
    // vkCmdPushDescriptorSetKHR when pushing to a descriptor set index that is not defined in
    // the command buffer's previously bound pipeline layout (e.g. multi-pass shader pipelines in
    // Iris). Unless the user explicitly forced Skip/Bypass, automatically force emulation on Mali.
    if (native) {
        PFN_vkGetPhysicalDeviceProperties real_props =
            (PFN_vkGetPhysicalDeviceProperties)get_real_proc(get_last_instance(), VK_NULL_HANDLE,
                                                             "vkGetPhysicalDeviceProperties");
        if (real_props) {
            VkPhysicalDeviceProperties props{};
            real_props(physDev, &props);
            if (props.vendorID == 0x13B5 || strstr(props.deviceName, "Mali") != nullptr) {
                LOGI("Physical device %p (%s, vendor 0x%x) has known Mali driver crash bug in "
                     "native VK_KHR_push_descriptor; forcing emulation layer!",
                     physDev, props.deviceName, props.vendorID);
                native = false;
            }
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    }
    if (!native) {
        LOGI("Physical device %p lacks native VK_KHR_push_descriptor, enabling emulation layer!",
             physDev);
    } else {
        LOGI("Physical device %p natively supports VK_KHR_push_descriptor", physDev);
    }
    return native;
}

bool PushDescriptorModule::is_device_native(VkDevice device) {
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    if (__builtin_expect(device == m_primary_dev.load(std::memory_order_relaxed), 1)) {
        return m_primary_native.load(std::memory_order_relaxed);
    }
    std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
    auto it = m_device_native_support.find((uint64_t)(uintptr_t)device);
    if (it != m_device_native_support.end()) {
        return it->second;
    }
    return false;
}

void PushDescriptorModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice, std::vector<VkExtensionProperties> &extensions) {
    bool has_ext = vku::has_extension(extensions, "VK_KHR_push_descriptor");

    if (!has_ext) {
        VkExtensionProperties prop{};
        memset(&prop, 0, sizeof(prop));
        strncpy(prop.extensionName, "VK_KHR_push_descriptor", VK_MAX_EXTENSION_NAME_SIZE - 1);
        prop.specVersion = VK_KHR_PUSH_DESCRIPTOR_SPEC_VERSION;
        extensions.push_back(prop);
        LOGI("Injected extension: VK_KHR_push_descriptor (spec version %u)", prop.specVersion);
    }
}

void PushDescriptorModule::on_pre_get_properties2(VkPhysicalDevice physicalDevice,
                                                  VkPhysicalDeviceProperties2 *pProperties,
                                                  void *&pUserData) {
    pUserData = nullptr;
    if (!pProperties || is_phys_device_native(physicalDevice))
        return;

    pUserData = vku::unlink_pnext(pProperties->pNext,
                                  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR);
}

void PushDescriptorModule::on_post_get_properties2(VkPhysicalDevice physicalDevice,
                                                   VkPhysicalDeviceProperties2 *pProperties,
                                                   void *pUserData) {
    if (!pProperties)
        return;

    if (pUserData) {
        auto *props = vku::relink_pnext<VkPhysicalDevicePushDescriptorPropertiesKHR>(
            pProperties->pNext, pUserData);
        props->maxPushDescriptors = 32;
        LOG_OPT_DEBUG("PushDescriptor: supplied maxPushDescriptors = 32 in "
                      "VkPhysicalDevicePushDescriptorPropertiesKHR");
    } else {
        auto *props = vku::find_pnext_mut<VkPhysicalDevicePushDescriptorPropertiesKHR>(
            pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR);
        if (props && props->maxPushDescriptors == 0) {
            props->maxPushDescriptors = 32;
        }
    }
}

void PushDescriptorModule::on_pre_create_device(VkPhysicalDevice physicalDevice,
                                                VkDeviceCreateInfo *pCreateInfo,
                                                VkPhysicalDeviceFeatures *pEnabledFeatures,
                                                std::vector<const char *> &enabledExtensions,
                                                void *&pUserData) {
    if (is_phys_device_native(physicalDevice) || !pCreateInfo)
        return;

    if (vku::strip_extension(enabledExtensions, "VK_KHR_push_descriptor")) {
        LOGI("vkCreateDevice: stripped VK_KHR_push_descriptor from enabledExtensions for physical "
             "device %p",
             physicalDevice);
    }
}

void PushDescriptorModule::on_post_create_device(VkPhysicalDevice physicalDevice, VkDevice device,
                                                 VkResult result, void *pUserData) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_device_native_support[(uint64_t)(uintptr_t)device] = native;
        if (m_primary_dev.load(std::memory_order_relaxed) == VK_NULL_HANDLE) {
            m_primary_native.store(native, std::memory_order_relaxed);
            m_primary_dev.store(device, std::memory_order_release);
        }
        LOGI("Device %p created: push descriptor %s", device, native ? "NATIVE" : "EMULATED");
    }
}

void PushDescriptorModule::on_destroy_device(VkDevice device) {
    if (m_primary_dev.load(std::memory_order_relaxed) == device) {
        m_primary_dev.store(VK_NULL_HANDLE, std::memory_order_relaxed);
        m_primary_native.store(false, std::memory_order_relaxed);
    }
    std::unique_lock<std::shared_mutex> rw_lock(m_rw_mutex);
    std::lock_guard<std::mutex> cmd_lock(m_cmd_mutex);
    m_device_native_support.erase((uint64_t)(uintptr_t)device);

    PFN_vkDestroyDescriptorPool real_destroy_pool = (PFN_vkDestroyDescriptorPool)get_real_proc(
        get_last_instance(), device, "vkDestroyDescriptorPool");

    for (auto it = m_cmd_states.begin(); it != m_cmd_states.end();) {
        if (it->second.device == device) {
            if (real_destroy_pool) {
                for (VkDescriptorPool pool : it->second.pools) {
                    real_destroy_pool(device, pool, NULL);
                }
            }
            it = m_cmd_states.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_pipeline_layouts.begin(); it != m_pipeline_layouts.end();)
        if (it->second->device == device)
            it = m_pipeline_layouts.erase(it);
        else
            ++it;
    for (auto it = m_set_layouts.begin(); it != m_set_layouts.end();)
        if (it->second->device == device)
            it = m_set_layouts.erase(it);
        else
            ++it;
    for (auto it = m_templates.begin(); it != m_templates.end();)
        if (it->second.device == device)
            it = m_templates.erase(it);
        else
            ++it;
}

void PushDescriptorModule::on_pre_create_descriptor_set_layout(
    VkDevice device, VkDescriptorSetLayoutCreateInfo &createInfo) {
    if (is_device_native(device))
        return;

    if (createInfo.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
        createInfo.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        LOG_OPT_DEBUG(
            "PushDescriptor: stripped PUSH_DESCRIPTOR_BIT from VkDescriptorSetLayoutCreateInfo");
    }
}

PushDescriptorModule::ObjectKey PushDescriptorModule::object_key(VkDevice device, uint64_t handle) {
    return {(uint64_t)(uintptr_t)device, handle};
}

PushDescriptorModule::SetLayout::~SetLayout() {
    if (allocation_layout) {
        auto destroy = (PFN_vkDestroyDescriptorSetLayout)get_real_proc(
            get_last_instance(), device, "vkDestroyDescriptorSetLayout");
        if (destroy)
            destroy(device, allocation_layout, nullptr);
    }
}

void PushDescriptorModule::on_post_create_descriptor_set_layout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *info, VkResult result,
    VkDescriptorSetLayout handle) {
    if (is_device_native(device) || result != VK_SUCCESS || !info || !handle)
        return;
    auto metadata = std::make_shared<SetLayout>();
    metadata->device = device;
    metadata->definition.push_back(info->flags);
    std::vector<uint32_t> order(info->bindingCount);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return info->pBindings[a].binding < info->pBindings[b].binding;
    });
    const auto *binding_flags = vku::find_pnext<VkDescriptorSetLayoutBindingFlagsCreateInfo>(
        info->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    metadata->samplers.resize(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        auto binding = info->pBindings[order[i]];
        const bool immutable =
            binding.pImmutableSamplers &&
            (binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
             binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        if (immutable)
            metadata->samplers[i].assign(binding.pImmutableSamplers,
                                         binding.pImmutableSamplers + binding.descriptorCount);
        binding.pImmutableSamplers = immutable ? metadata->samplers[i].data() : nullptr;
        metadata->bindings.push_back(binding);
        auto &d = metadata->definition;
        d.insert(d.end(), {binding.binding, uint64_t(binding.descriptorType),
                           binding.descriptorCount, binding.stageFlags, uint64_t(immutable),
                           binding_flags && order[i] < binding_flags->bindingCount
                               ? binding_flags->pBindingFlags[order[i]]
                               : 0});
        for (auto sampler : metadata->samplers[i])
            d.push_back((uint64_t)sampler);
    }
    // Unknown layout extensions cannot establish compatibility between distinct objects.
    for (auto *next = (const VkBaseInStructure *)info->pNext; next; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            static std::atomic<uint64_t> generation{1};
            metadata->definition.push_back(generation.fetch_add(1));
        }
    }
    if (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
        // Pipeline layouts may outlive the application's set-layout handle. Keep
        // a private allocation layout alive with pipeline/command metadata.
        auto ci = *info;
        ci.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        auto create = (PFN_vkCreateDescriptorSetLayout)get_real_proc(get_last_instance(), device,
                                                                     "vkCreateDescriptorSetLayout");
        metadata->allocation_error =
            create ? create(device, &ci, nullptr, &metadata->allocation_layout)
                   : VK_ERROR_FEATURE_NOT_PRESENT;
        if (metadata->allocation_error != VK_SUCCESS)
            LOGE("PushDescriptor: failed to create persistent allocation layout");
    }
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_set_layouts[object_key(device, (uint64_t)handle)] = std::move(metadata);
}

void PushDescriptorModule::on_destroy_descriptor_set_layout(VkDevice device,
                                                            VkDescriptorSetLayout handle) {
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_set_layouts.erase(object_key(device, (uint64_t)handle));
}

void PushDescriptorModule::on_post_create_pipeline_layout(VkDevice device,
                                                          const VkPipelineLayoutCreateInfo *info,
                                                          VkResult result,
                                                          VkPipelineLayout handle) {
    if (is_device_native(device) || result != VK_SUCCESS || !info || !handle)
        return;
    auto metadata = std::make_shared<PipelineLayout>();
    metadata->device = device;
    metadata->independent_sets =
        (info->flags & VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) != 0;
    std::vector<VkPushConstantRange> ranges;
    if (info->pushConstantRangeCount)
        ranges.assign(info->pPushConstantRanges,
                      info->pPushConstantRanges + info->pushConstantRangeCount);
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) {
        if (a.offset != b.offset)
            return a.offset < b.offset;
        if (a.size != b.size)
            return a.size < b.size;
        return a.stageFlags < b.stageFlags;
    });
    for (const auto &r : ranges)
        metadata->push_ranges.insert(metadata->push_ranges.end(), {r.offset, r.size, r.stageFlags});
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    for (uint32_t i = 0; i < info->setLayoutCount; ++i) {
        auto it = m_set_layouts.find(object_key(device, (uint64_t)info->pSetLayouts[i]));
        metadata->sets.push_back(it == m_set_layouts.end() ? nullptr : it->second);
    }
    m_pipeline_layouts[object_key(device, (uint64_t)handle)] = std::move(metadata);
}

void PushDescriptorModule::on_destroy_pipeline_layout(VkDevice device, VkPipelineLayout handle) {
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_pipeline_layouts.erase(object_key(device, (uint64_t)handle));
}

void PushDescriptorModule::on_post_allocate_command_buffers(
    VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo, VkResult result,
    VkCommandBuffer *pCommandBuffers) {
    if (result == VK_SUCCESS && pAllocateInfo && pCommandBuffers) {
        std::lock_guard<std::mutex> lock(m_cmd_mutex);
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            auto &state = m_cmd_states[(uint64_t)(uintptr_t)pCommandBuffers[i]];
            state.device = device;
            state.command_pool = pAllocateInfo->commandPool;
        }
    }
}

void PushDescriptorModule::on_free_command_buffers(VkDevice device, uint32_t count,
                                                   const VkCommandBuffer *pCommandBuffers) {
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    PFN_vkDestroyDescriptorPool real_destroy_pool = (PFN_vkDestroyDescriptorPool)get_real_proc(
        get_last_instance(), device, "vkDestroyDescriptorPool");

    for (uint32_t i = 0; i < count; ++i) {
        auto it = m_cmd_states.find((uint64_t)(uintptr_t)pCommandBuffers[i]);
        if (it != m_cmd_states.end()) {
            if (real_destroy_pool) {
                for (VkDescriptorPool pool : it->second.pools) {
                    real_destroy_pool(device, pool, NULL);
                }
            }
            m_cmd_states.erase(it);
        }
    }
}

void PushDescriptorModule::on_begin_command_buffer(VkCommandBuffer commandBuffer,
                                                   const VkCommandBufferBeginInfo *pBeginInfo) {
    reset_cmd_pools(commandBuffer);
}

void PushDescriptorModule::on_reset_command_buffer(VkCommandBuffer commandBuffer,
                                                   VkCommandBufferResetFlags flags) {
    reset_cmd_pools(commandBuffer);
}

void PushDescriptorModule::reset_cmd_pools(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    auto it = m_cmd_states.find((uint64_t)(uintptr_t)cmd);
    if (it == m_cmd_states.end())
        return;

    const auto &dt = LayerManager::get().get_dispatch_table(it->second.device);
    PFN_vkResetDescriptorPool real_reset_pool = dt.ResetDescriptorPool;
    if (!real_reset_pool) {
        real_reset_pool = (PFN_vkResetDescriptorPool)get_real_proc(
            get_last_instance(), it->second.device, "vkResetDescriptorPool");
    }
    if (real_reset_pool) {
        for (VkDescriptorPool pool : it->second.pools) {
            real_reset_pool(it->second.device, pool, 0);
        }
    }
    it->second.current_pool_idx = 0;
    it->second.current_pool_allocated = 0;
    it->second.reset_batch();
    it->second.bindings.clear();
    it->second.error = VK_SUCCESS;
}

void PushDescriptorModule::on_pre_create_descriptor_update_template(
    VkDevice device, VkDescriptorUpdateTemplateCreateInfo &ci) {
    if (is_device_native(device) ||
        ci.templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR)
        return;
    auto layout = get_layout(device, ci.pipelineLayout);
    if (!layout || ci.set >= layout->sets.size() || !layout->sets[ci.set])
        return;
    ci.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    ci.descriptorSetLayout = layout->sets[ci.set]->allocation_layout;
}

void PushDescriptorModule::on_post_create_descriptor_update_template(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci, VkResult result,
    VkDescriptorUpdateTemplate handle) {
    if (is_device_native(device) || result != VK_SUCCESS || !ci || !handle ||
        ci->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR)
        return;
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    TemplateInfo metadata;
    metadata.device = device;
    metadata.point = ci->pipelineBindPoint;
    for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; ++i) {
        const auto &entry = ci->pDescriptorUpdateEntries[i];
        VkWriteDescriptorSet coverage{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        coverage.dstBinding = entry.dstBinding;
        coverage.dstArrayElement = entry.dstArrayElement;
        coverage.descriptorCount = entry.descriptorCount;
        metadata.coverage.push_back(coverage);
    }
    m_templates[object_key(device, (uint64_t)handle)] = std::move(metadata);
}

bool PushDescriptorModule::on_destroy_descriptor_update_template(VkDevice device,
                                                                 VkDescriptorUpdateTemplate handle,
                                                                 const VkAllocationCallbacks *) {
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_templates.erase(object_key(device, (uint64_t)handle));
    return false;
}

VkDevice PushDescriptorModule::get_device_for_cmd(VkCommandBuffer cmd) {
    {
        std::lock_guard<std::mutex> lock(m_cmd_mutex);
        auto it = m_cmd_states.find((uint64_t)(uintptr_t)cmd);
        if (it != m_cmd_states.end())
            return it->second.device;
    }
    return LayerManager::get().get_device_for_cmd(cmd);
}

std::shared_ptr<PushDescriptorModule::PipelineLayout>
PushDescriptorModule::get_layout(VkDevice device, VkPipelineLayout handle) {
    std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
    auto it = m_pipeline_layouts.find(object_key(device, (uint64_t)handle));
    return it == m_pipeline_layouts.end() ? nullptr : it->second;
}

bool PushDescriptorModule::compatible(const std::shared_ptr<PipelineLayout> &a,
                                      const std::shared_ptr<PipelineLayout> &b, uint32_t set) {
    if (!a || !b || a->device != b->device || set >= a->sets.size() || set >= b->sets.size())
        return false;
    if (a == b)
        return true;
    if (a->independent_sets != b->independent_sets || a->push_ranges != b->push_ranges)
        return false;
    for (uint32_t i = 0; i <= set; ++i) {
        if (a->sets[i] == b->sets[i])
            continue;
        if (!a->sets[i] || !b->sets[i] || a->sets[i]->definition != b->sets[i]->definition)
            return false;
    }
    return true;
}

void PushDescriptorModule::bind_state(CmdPushState &state, VkPipelineBindPoint point,
                                      const std::shared_ptr<PipelineLayout> &layout, uint32_t set,
                                      VkDescriptorSet snapshot) {
    auto &slots = state.bindings[point];
    auto previous = slots.find(set);
    const bool disturb_higher =
        previous != slots.end() && !compatible(previous->second.layout, layout, set);
    for (auto &slot : slots) {
        if ((slot.first < set && !compatible(slot.second.layout, layout, slot.first)) ||
            (slot.first > set && disturb_higher))
            slot.second = {layout, VK_NULL_HANDLE};
    }
    slots[set] = {layout, snapshot};
}

void PushDescriptorModule::on_cmd_bind_descriptor_sets(VkCommandBuffer cmd,
                                                       VkPipelineBindPoint point,
                                                       VkPipelineLayout handle, uint32_t first,
                                                       uint32_t count) {
    auto device = get_device_for_cmd(cmd);
    if (is_device_native(device) || !count)
        return;
    auto layout = get_layout(device, handle);
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    auto &state = m_cmd_states[(uint64_t)(uintptr_t)cmd];
    state.device = device;
    for (uint32_t i = 0; i < count; ++i)
        bind_state(state, point, layout, first + i, VK_NULL_HANDLE);
}

void PushDescriptorModule::on_reset_command_pool(VkDevice device, VkCommandPool pool) {
    std::vector<VkCommandBuffer> commands;
    {
        std::lock_guard<std::mutex> lock(m_cmd_mutex);
        for (const auto &item : m_cmd_states)
            if (item.second.device == device && item.second.command_pool == pool)
                commands.push_back((VkCommandBuffer)(uintptr_t)item.first);
    }
    for (auto cmd : commands)
        reset_cmd_pools(cmd);
}

void PushDescriptorModule::on_destroy_command_pool(VkDevice device, VkCommandPool pool) {
    std::vector<VkCommandBuffer> commands;
    {
        std::lock_guard<std::mutex> lock(m_cmd_mutex);
        for (const auto &item : m_cmd_states)
            if (item.second.device == device && item.second.command_pool == pool)
                commands.push_back((VkCommandBuffer)(uintptr_t)item.first);
    }
    on_free_command_buffers(device, uint32_t(commands.size()), commands.data());
}

VkDescriptorSet PushDescriptorModule::fail_recording(VkCommandBuffer cmd, VkResult error) {
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    auto &state = m_cmd_states[(uint64_t)(uintptr_t)cmd];
    if (state.error == VK_SUCCESS)
        state.error = error;
    return VK_NULL_HANDLE;
}

VkResult PushDescriptorModule::command_recording_error(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    auto it = m_cmd_states.find((uint64_t)(uintptr_t)cmd);
    return it == m_cmd_states.end() ? VK_SUCCESS : it->second.error;
}

VkDescriptorSet
PushDescriptorModule::prepare_snapshot(VkDevice device, VkCommandBuffer cmd,
                                       VkPipelineBindPoint point,
                                       const std::shared_ptr<PipelineLayout> &layout, uint32_t set,
                                       uint32_t write_count, const VkWriteDescriptorSet *writes) {
    if (!layout || set >= layout->sets.size() || !layout->sets[set])
        return fail_recording(cmd, VK_ERROR_FEATURE_NOT_PRESENT);
    auto metadata = layout->sets[set];
    if (!metadata->allocation_layout)
        return fail_recording(cmd, metadata->allocation_error != VK_SUCCESS
                                       ? metadata->allocation_error
                                       : VK_ERROR_FEATURE_NOT_PRESENT);
    auto next = allocate_push_set(device, cmd, metadata);
    if (!next)
        return fail_recording(cmd, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    std::vector<VkCopyDescriptorSet> copies;
    {
        std::lock_guard<std::mutex> lock(m_cmd_mutex);
        auto &state = m_cmd_states[(uint64_t)(uintptr_t)cmd];
        auto &slots = state.bindings[point];
        auto previous = slots.find(set);
        if (previous != slots.end() && previous->second.snapshot &&
            compatible(previous->second.layout, layout, set)) {
            for (const auto &binding : metadata->bindings) {
                if (!binding.descriptorCount ||
                    (binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER &&
                     binding.pImmutableSamplers))
                    continue;
                bool overwritten = false;
                for (uint32_t i = 0; i < write_count; ++i)
                    if (writes[i].dstBinding == binding.binding && writes[i].dstArrayElement == 0 &&
                        writes[i].descriptorCount >= binding.descriptorCount)
                        overwritten = true;
                if (overwritten)
                    continue;
                VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
                copy.srcSet = previous->second.snapshot;
                copy.dstSet = next;
                copy.srcBinding = copy.dstBinding = binding.binding;
                copy.descriptorCount = binding.descriptorCount;
                copies.push_back(copy);
            }
        }
        bind_state(state, point, layout, set, next);
    }
    // A descriptor copy preserves even undefined descriptors without using the
    // referenced resources. Copy first, then overwrite this call's updates in
    // a separate vkUpdateDescriptorSets call (writes precede copies otherwise).
    if (!copies.empty()) {
        auto update = (PFN_vkUpdateDescriptorSets)get_real_proc(get_last_instance(), device,
                                                                "vkUpdateDescriptorSets");
        if (!update)
            return fail_recording(cmd, VK_ERROR_FEATURE_NOT_PRESENT);
        update(device, 0, nullptr, uint32_t(copies.size()), copies.data());
        m_inherited_snapshots.fetch_add(1, std::memory_order_relaxed);
    }
    return next;
}

VkDescriptorPool PushDescriptorModule::create_pool(VkDevice device, uint32_t maxSets) {
    PFN_vkCreateDescriptorPool real_create_pool = (PFN_vkCreateDescriptorPool)get_real_proc(
        get_last_instance(), device, "vkCreateDescriptorPool");
    if (!real_create_pool)
        return VK_NULL_HANDLE;

    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, maxSets * 16},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxSets * 32},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxSets * 32},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxSets * 16},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, maxSets * 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, maxSets * 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxSets * 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxSets * 16},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, maxSets * 8},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = sizeof(poolSizes) / sizeof(poolSizes[0]);
    poolInfo.pPoolSizes = poolSizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult res = real_create_pool(device, &poolInfo, NULL, &pool);
    if (res != VK_SUCCESS) {
        LOGE("PushDescriptor: failed to create internal descriptor pool: %d", res);
        return VK_NULL_HANDLE;
    }
    vku::set_object_name(device, VK_OBJECT_TYPE_DESCRIPTOR_POOL, (uint64_t)pool,
                         "[VulkanFix] PushDescriptor Pool");
    return pool;
}

VkDescriptorSet
PushDescriptorModule::allocate_push_set(VkDevice device, VkCommandBuffer cmd,
                                        const std::shared_ptr<SetLayout> &setLayout) {
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    CmdPushState &state = m_cmd_states[(uint64_t)(uintptr_t)cmd];
    state.device = device;

    // Fast-path: return from pre-allocated batch if matching setLayout
    if (__builtin_expect(
            state.cached_layout == setLayout && state.cached_index < state.cached_count, 1)) {
        return state.cached_sets[state.cached_index++];
    }

    state.reset_batch();

    const auto &dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkAllocateDescriptorSets real_alloc = dt.AllocateDescriptorSets;
    if (!real_alloc) {
        real_alloc = (PFN_vkAllocateDescriptorSets)get_real_proc(get_last_instance(), device,
                                                                 "vkAllocateDescriptorSets");
    }
    if (!real_alloc)
        return VK_NULL_HANDLE;

    constexpr uint32_t MAX_SETS_PER_POOL = 256;

    if (state.pools.empty()) {
        VkDescriptorPool pool = create_pool(device, MAX_SETS_PER_POOL);
        if (pool == VK_NULL_HANDLE)
            return VK_NULL_HANDLE;
        state.pools.push_back(pool);
        state.current_pool_idx = 0;
        state.current_pool_allocated = 0;
    }

    auto try_alloc_batch = [&](size_t pool_idx) -> VkDescriptorSet {
        if (state.current_pool_allocated >= MAX_SETS_PER_POOL)
            return VK_NULL_HANDLE;
        uint32_t remaining = MAX_SETS_PER_POOL - state.current_pool_allocated;
        uint32_t batch_size = std::min((uint32_t)CmdPushState::BATCH_CAPACITY, remaining);

        VkDescriptorSetLayout layouts[CmdPushState::BATCH_CAPACITY];
        for (uint32_t i = 0; i < batch_size; ++i) {
            layouts[i] = setLayout->allocation_layout;
        }

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = state.pools[pool_idx];
        allocInfo.descriptorSetCount = batch_size;
        allocInfo.pSetLayouts = layouts;

        VkResult res = real_alloc(device, &allocInfo, state.cached_sets);
        if (res == VK_SUCCESS) {
            state.cached_layout = setLayout;
            state.cached_count = batch_size;
            state.cached_index = 1;
            state.current_pool_allocated += batch_size;
            return state.cached_sets[0];
        }

        // Fallback to single set if batch allocation failed
        if (batch_size > 1) {
            allocInfo.descriptorSetCount = 1;
            VkDescriptorSet singleSet = VK_NULL_HANDLE;
            if (real_alloc(device, &allocInfo, &singleSet) == VK_SUCCESS &&
                singleSet != VK_NULL_HANDLE) {
                state.current_pool_allocated++;
                return singleSet;
            }
        }
        return VK_NULL_HANDLE;
    };

    VkDescriptorSet set = try_alloc_batch(state.current_pool_idx);
    if (set != VK_NULL_HANDLE) {
        return set;
    }

    while (state.current_pool_idx + 1 < state.pools.size()) {
        state.current_pool_idx++;
        state.current_pool_allocated = 0;
        set = try_alloc_batch(state.current_pool_idx);
        if (set != VK_NULL_HANDLE) {
            return set;
        }
    }

    VkDescriptorPool new_pool = create_pool(device, MAX_SETS_PER_POOL);
    if (new_pool == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;
    state.pools.push_back(new_pool);
    state.current_pool_idx = state.pools.size() - 1;
    state.current_pool_allocated = 0;

    set = try_alloc_batch(state.current_pool_idx);
    if (set != VK_NULL_HANDLE) {
        return set;
    }

    LOGE("PushDescriptor: failed to allocate descriptor set");
    return VK_NULL_HANDLE;
}

bool PushDescriptorModule::on_cmd_push_descriptor_set(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    if (device == VK_NULL_HANDLE || is_device_native(device))
        return false;

    static std::atomic<uint64_t> emulated_pushes{0};
    const uint64_t push_index = emulated_pushes.fetch_add(1, std::memory_order_relaxed) + 1;
    m_pushes_since_present.fetch_add(1, std::memory_order_relaxed);
    if (push_index == 1 || (push_index & 255u) == 0) {
        LOGI("push descriptor: emulated %llu vkCmdPushDescriptorSet call(s)",
             static_cast<unsigned long long>(push_index));
    }

    auto pipeline = get_layout(device, layout);
    if (!pipeline || set >= pipeline->sets.size() || !pipeline->sets[set]) {
        LOGE("PushDescriptor: no descriptor set layout registered for pipeline layout %p set %u",
             (void *)(uintptr_t)layout, set);
        fail_recording(commandBuffer, VK_ERROR_FEATURE_NOT_PRESENT);
        return true;
    }

    VkDescriptorSet descSet = prepare_snapshot(device, commandBuffer, pipelineBindPoint, pipeline,
                                               set, descriptorWriteCount, pDescriptorWrites);
    if (descSet == VK_NULL_HANDLE) {
        LOGE("PushDescriptor: failed to allocate internal descriptor set!");
        return true;
    }

    constexpr uint32_t SBO_LIMIT = 16;
    VkWriteDescriptorSet stackWrites[SBO_LIMIT];
    VkWriteDescriptorSet *modWrites =
        (descriptorWriteCount <= SBO_LIMIT)
            ? stackWrites
            : (VkWriteDescriptorSet *)malloc(sizeof(VkWriteDescriptorSet) * descriptorWriteCount);
    if (!modWrites) {
        fail_recording(commandBuffer, VK_ERROR_OUT_OF_HOST_MEMORY);
        return true;
    }

    memcpy(modWrites, pDescriptorWrites, sizeof(VkWriteDescriptorSet) * descriptorWriteCount);
    for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
        modWrites[i].dstSet = descSet;
    }

    const auto &dt = LayerManager::get().get_dispatch_table(device);
    if (dt.UpdateDescriptorSets) {
        dt.UpdateDescriptorSets(device, descriptorWriteCount, modWrites, 0, NULL);
    } else {
        PFN_vkUpdateDescriptorSets real_update = (PFN_vkUpdateDescriptorSets)get_real_proc(
            get_last_instance(), device, "vkUpdateDescriptorSets");
        if (real_update) {
            real_update(device, descriptorWriteCount, modWrites, 0, NULL);
        }
    }

    if (modWrites != stackWrites)
        free(modWrites);

    if (dt.CmdBindDescriptorSets) {
        dt.CmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, set, 1, &descSet, 0,
                                 NULL);
    } else {
        PFN_vkCmdBindDescriptorSets real_bind = (PFN_vkCmdBindDescriptorSets)get_real_proc(
            get_last_instance(), device, "vkCmdBindDescriptorSets");
        if (real_bind) {
            real_bind(commandBuffer, pipelineBindPoint, layout, set, 1, &descSet, 0, NULL);
        }
    }

    LOG_OPT_DEBUG("PushDescriptor: emulated vkCmdPushDescriptorSetKHR for cmd %p set %u",
                  commandBuffer, set);
    return true;
}

bool PushDescriptorModule::on_cmd_push_descriptor_set_with_template(
    VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    VkPipelineLayout layout, uint32_t set, const void *pData) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    if (device == VK_NULL_HANDLE || is_device_native(device))
        return false;

    static std::atomic<uint64_t> emulated_template_pushes{0};
    const uint64_t push_index =
        emulated_template_pushes.fetch_add(1, std::memory_order_relaxed) + 1;
    m_pushes_since_present.fetch_add(1, std::memory_order_relaxed);
    if (push_index == 1 || (push_index & 255u) == 0) {
        LOGI("push descriptor: emulated %llu vkCmdPushDescriptorSetWithTemplate call(s)",
             static_cast<unsigned long long>(push_index));
    }

    VkPipelineBindPoint pipelineBindPoint;
    std::vector<VkWriteDescriptorSet> coverage;
    {
        std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
        auto it = m_templates.find(object_key(device, (uint64_t)descriptorUpdateTemplate));
        if (it == m_templates.end()) {
            LOGE("PushDescriptor: push template metadata is unavailable");
            lock.unlock();
            fail_recording(commandBuffer, VK_ERROR_FEATURE_NOT_PRESENT);
            return true;
        }
        pipelineBindPoint = it->second.point;
        coverage = it->second.coverage;
    }
    auto pipeline = get_layout(device, layout);
    if (!pipeline || set >= pipeline->sets.size() || !pipeline->sets[set]) {
        LOGE("PushDescriptor: no descriptor set layout registered for pipeline layout %p set %u "
             "(template)",
             (void *)(uintptr_t)layout, set);
        fail_recording(commandBuffer, VK_ERROR_FEATURE_NOT_PRESENT);
        return true;
    }

    VkDescriptorSet descSet = prepare_snapshot(device, commandBuffer, pipelineBindPoint, pipeline,
                                               set, uint32_t(coverage.size()), coverage.data());
    if (descSet == VK_NULL_HANDLE)
        return true;

    const auto &dt = LayerManager::get().get_dispatch_table(device);
    LayerManager::get().dispatch_update_descriptor_set_with_template(
        device, descSet, descriptorUpdateTemplate, pData);

    if (dt.CmdBindDescriptorSets) {
        dt.CmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, set, 1, &descSet, 0,
                                 NULL);
    } else {
        PFN_vkCmdBindDescriptorSets real_bind = (PFN_vkCmdBindDescriptorSets)get_real_proc(
            get_last_instance(), device, "vkCmdBindDescriptorSets");
        if (real_bind) {
            real_bind(commandBuffer, pipelineBindPoint, layout, set, 1, &descSet, 0, NULL);
        }
    }

    LOG_OPT_DEBUG(
        "PushDescriptor: emulated vkCmdPushDescriptorSetWithTemplateKHR for cmd %p set %u",
        commandBuffer, set);
    return true;
}

bool PushDescriptorModule::on_queue_present(VkQueue queue, const VkPresentInfoKHR *pPresentInfo,
                                            VkResult &outResult) {
    (void)queue;
    (void)pPresentInfo;
    (void)outResult;
    const uint64_t pushes = m_pushes_since_present.exchange(0, std::memory_order_relaxed);
    const uint64_t frame = m_present_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t total =
        m_present_pushes_total.fetch_add(pushes, std::memory_order_relaxed) + pushes;
    if (frame == 60 || frame == 300 || (frame % 600u) == 0) {
        LOGI(
            "push descriptor: %llu frames, %llu pushes total, %llu avg pushes/frame, %llu "
            "inherited snapshots",
            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(total),
            static_cast<unsigned long long>(total / frame),
            static_cast<unsigned long long>(m_inherited_snapshots.load(std::memory_order_relaxed)));
    }
    return false;
}

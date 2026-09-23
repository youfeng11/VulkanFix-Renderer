#include "vertex_attribute_divisor.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"
#include <string.h>
#include <stdlib.h>
#include <algorithm>

REGISTER_LAYER_MODULE(VertexAttributeDivisorModule);

VertexAttributeDivisorModule::VertexAttributeDivisorModule() {
    LOGI("Initialized Vulkan Vertex Attribute Divisor emulation module (with full instance rate divisor support)");
}

VertexAttributeDivisorModule::PhysDeviceInfo VertexAttributeDivisorModule::probe_phys_device(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_devices.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_devices.end() && it->second.probed) {
        return it->second;
    }

    PhysDeviceInfo info{};
    if (it != m_phys_devices.end()) {
        info = it->second;
    }

    // 1. Probe native device extensions if not already known
    if (!info.probed) {
        PFN_vkEnumerateDeviceExtensionProperties real_ext_fn =
            (PFN_vkEnumerateDeviceExtensionProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
        if (real_ext_fn) {
            uint32_t count = 0;
            if (real_ext_fn(physDev, NULL, &count, NULL) == VK_SUCCESS && count > 0) {
                std::vector<VkExtensionProperties> exts(count);
                if (real_ext_fn(physDev, NULL, &count, exts.data()) == VK_SUCCESS) {
                    for (const auto& e : exts) {
                        if (strcmp(e.extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0) {
                            info.native_has_ext = true;
                        }
                        if (strcmp(e.extensionName, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0) {
                            info.native_has_khr = true;
                        }
                    }
                }
            }
        }
    }

    // 2. Probe native feature support (vertexAttributeInstanceRateDivisor and zeroDivisor)
    const char* force_emu = getenv("FORCE_EMULATE_DIVISOR");
    bool forced = (force_emu && (strcmp(force_emu, "1") == 0 || strcasecmp(force_emu, "true") == 0));

    if (!forced && (info.native_has_ext || info.native_has_khr)) {
        PFN_vkGetPhysicalDeviceFeatures2 real_gpf2 = (PFN_vkGetPhysicalDeviceFeatures2)
            get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2");
        if (!real_gpf2) {
            real_gpf2 = (PFN_vkGetPhysicalDeviceFeatures2)
                get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2KHR");
        }

        if (real_gpf2) {
            VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisorFeatures{};
            divisorFeatures.sType = info.native_has_khr
                ? VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR
                : VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT;
            divisorFeatures.pNext = nullptr;

            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &divisorFeatures;

            real_gpf2(physDev, &features2);

            info.native_has_rate_divisor = (divisorFeatures.vertexAttributeInstanceRateDivisor == VK_TRUE);
            info.native_has_zero_divisor = (divisorFeatures.vertexAttributeInstanceRateZeroDivisor == VK_TRUE);
        }
    }

    info.probed = true;
    m_phys_devices[(uint64_t)(uintptr_t)physDev] = info;

    LOGI("Physical device %p: native EXT=%d, native KHR=%d, native rateDivisor=%d, native zeroDivisor=%d",
         physDev, info.native_has_ext, info.native_has_khr, info.native_has_rate_divisor, info.native_has_zero_divisor);

    return info;
}

bool VertexAttributeDivisorModule::is_device_native(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_needs_emulation.find((uint64_t)(uintptr_t)device);
    if (it != m_device_needs_emulation.end()) {
        return !it->second;
    }
    return false;
}

VkDevice VertexAttributeDivisorModule::get_device_for_cmd(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cmd_devices.find((uint64_t)(uintptr_t)cmd);
    if (it != m_cmd_devices.end()) {
        return it->second;
    }
    return m_last_device.load();
}

VertexAttributeDivisorModule::CmdBufferState* VertexAttributeDivisorModule::get_or_create_cmd_state(VkCommandBuffer cmd) {
    thread_local VkCommandBuffer s_cached_cmd = VK_NULL_HANDLE;
    thread_local CmdBufferState* s_cached_state = nullptr;
    if (__builtin_expect(cmd == s_cached_cmd && s_cached_state != nullptr, 1)) {
        return s_cached_state;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cmd_states.find((uint64_t)(uintptr_t)cmd);
    if (it != m_cmd_states.end()) {
        s_cached_cmd = cmd;
        s_cached_state = it->second.get();
        return s_cached_state;
    }
    auto state = std::make_unique<CmdBufferState>();
    CmdBufferState* ptr = state.get();
    m_cmd_states[(uint64_t)(uintptr_t)cmd] = std::move(state);
    s_cached_cmd = cmd;
    s_cached_state = ptr;
    return ptr;
}

void VertexAttributeDivisorModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    bool has_ext = vku::has_extension(extensions, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    bool has_khr = vku::has_extension(extensions, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);

    // Save native driver extension presence
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& info = m_phys_devices[(uint64_t)(uintptr_t)physicalDevice];
        info.native_has_ext = has_ext;
        info.native_has_khr = has_khr;
    }

    // ALWAYS inject VK_EXT_vertex_attribute_divisor if not natively present
    if (!has_ext) {
        VkExtensionProperties extProps{};
        memset(&extProps, 0, sizeof(extProps));
        strncpy(extProps.extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
        extProps.specVersion = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION;
        extensions.push_back(extProps);
        LOGI("Injected extension: %s (v%u)", VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION);
    }

    // ALWAYS inject VK_KHR_vertex_attribute_divisor if not natively present
    if (!has_khr) {
        VkExtensionProperties khrProps{};
        memset(&khrProps, 0, sizeof(khrProps));
        strncpy(khrProps.extensionName, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
        khrProps.specVersion = VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION;
        extensions.push_back(khrProps);
        LOGI("Injected extension: %s (v%u)", VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION);
    }
}

void VertexAttributeDivisorModule::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (!pFeatures) return;

    PhysDeviceInfo info = probe_phys_device(physicalDevice);

    // If native driver does not support either extension natively, unlink divisor feature structs
    // so the driver does not fail on unrecognized sType
    if (!info.native_has_ext && !info.native_has_khr) {
        void* unlinked = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT);
        if (!unlinked) {
            unlinked = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR);
        }
        pUserData = unlinked;
    }
}

void VertexAttributeDivisorModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures) return;

    if (pUserData) {
        auto* feat = vku::relink_pnext<VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT>(
            pFeatures->pNext, pUserData);
        feat->vertexAttributeInstanceRateDivisor = VK_TRUE;
        feat->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
        LOG_OPT_DEBUG("Supplied vertexAttributeInstanceRateDivisor = VK_TRUE, vertexAttributeInstanceRateZeroDivisor = VK_TRUE");
    } else {
        auto* featExt = vku::find_pnext_mut<VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT>(
            pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT);
        if (featExt) {
            featExt->vertexAttributeInstanceRateDivisor = VK_TRUE;
            featExt->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
            LOG_OPT_DEBUG("Ensured vertexAttributeInstanceRateDivisor = VK_TRUE in EXT features");
        }
        auto* featKhr = vku::find_pnext_mut<VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR>(
            pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR);
        if (featKhr) {
            featKhr->vertexAttributeInstanceRateDivisor = VK_TRUE;
            featKhr->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
            LOG_OPT_DEBUG("Ensured vertexAttributeInstanceRateDivisor = VK_TRUE in KHR features");
        }
    }

#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
    auto* v14 = vku::find_pnext_mut<VkPhysicalDeviceVulkan14Features>(
        pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES);
    if (v14) {
        v14->vertexAttributeInstanceRateDivisor = VK_TRUE;
        v14->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
        LOG_OPT_DEBUG("Supplied vertexAttributeInstanceRateDivisor = VK_TRUE in VkPhysicalDeviceVulkan14Features");
    }
#endif
}

void VertexAttributeDivisorModule::on_pre_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void*& pUserData
) {
    pUserData = nullptr;
    if (!pProperties) return;

    PhysDeviceInfo info = probe_phys_device(physicalDevice);

    if (!info.native_has_ext && !info.native_has_khr) {
        void* unlinked = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT);
        if (!unlinked) {
            unlinked = vku::unlink_pnext(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR);
        }
        pUserData = unlinked;
    }
}

void VertexAttributeDivisorModule::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties) return;

    if (pUserData) {
        auto* props = vku::relink_pnext<VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT>(
            pProperties->pNext, pUserData);
        props->maxVertexAttribDivisor = UINT32_MAX;
        LOG_OPT_DEBUG("Supplied maxVertexAttribDivisor = UINT32_MAX");
    } else {
        auto* extProps = vku::find_pnext_mut<VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT>(
            pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT);
        if (extProps && extProps->maxVertexAttribDivisor == 0) {
            extProps->maxVertexAttribDivisor = UINT32_MAX;
        }
        auto* khrProps = vku::find_pnext_mut<VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR>(
            pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR);
        if (khrProps && khrProps->maxVertexAttribDivisor == 0) {
            khrProps->maxVertexAttribDivisor = UINT32_MAX;
        }
    }

#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES
    auto* v14Props = vku::find_pnext_mut<VkPhysicalDeviceVulkan14Properties>(
        pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES);
    if (v14Props && v14Props->maxVertexAttribDivisor == 0) {
        v14Props->maxVertexAttribDivisor = UINT32_MAX;
        LOG_OPT_DEBUG("Supplied maxVertexAttribDivisor = UINT32_MAX in VkPhysicalDeviceVulkan14Properties");
    }
#endif
}

void VertexAttributeDivisorModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (!pCreateInfo) return;

    PhysDeviceInfo info = probe_phys_device(physicalDevice);

    // 1. Only strip extensions if native driver does not support them!
    if (!info.native_has_ext) {
        if (vku::strip_extension(enabledExtensions, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) {
            LOGI("vkCreateDevice: stripped %s (native driver lacks support)", VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        }
    }
    if (!info.native_has_khr) {
        if (vku::strip_extension(enabledExtensions, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) {
            LOGI("vkCreateDevice: stripped %s (native driver lacks support)", VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        }
    }

    // 2. Features handling:
    if (!info.native_has_ext && !info.native_has_khr) {
        void* unlinked = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT);
        if (!unlinked) {
            unlinked = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR);
        }
        pUserData = unlinked;
        if (pUserData) {
            LOGI("vkCreateDevice: safely unlinked divisor features struct from pNext");
        }
    } else {
        // Driver supports extension, but may not support divisor > 1 or zero divisor natively
        if (!info.native_has_rate_divisor) {
            auto* extFeat = vku::find_pnext_mut<VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT>(
                pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT);
            if (extFeat) {
                extFeat->vertexAttributeInstanceRateDivisor = VK_FALSE;
                if (!info.native_has_zero_divisor) {
                    extFeat->vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
                }
                LOGI("vkCreateDevice: disabled unsupported rateDivisor in EXT features for native driver");
            }
            auto* khrFeat = vku::find_pnext_mut<VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR>(
                pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR);
            if (khrFeat) {
                khrFeat->vertexAttributeInstanceRateDivisor = VK_FALSE;
                if (!info.native_has_zero_divisor) {
                    khrFeat->vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
                }
                LOGI("vkCreateDevice: disabled unsupported rateDivisor in KHR features for native driver");
            }
        }
    }

    // 3. Disable divisor features in Vulkan 1.4 features if chained and driver lacks hardware support
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
    if (!info.native_has_rate_divisor) {
        auto* v14 = vku::find_pnext_mut<VkPhysicalDeviceVulkan14Features>(
            pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES);
        if (v14) {
            v14->vertexAttributeInstanceRateDivisor = VK_FALSE;
            if (!info.native_has_zero_divisor) {
                v14->vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
            }
            LOGI("vkCreateDevice: disabled vertexAttributeInstanceRateDivisor in VkPhysicalDeviceVulkan14Features");
        }
    }
#endif
}

void VertexAttributeDivisorModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        PhysDeviceInfo info = probe_phys_device(physicalDevice);

        // A device needs emulation if native driver lacks rate divisor (> 1) or zero divisor (== 0),
        // or lacks the extension entirely
        bool needs_emu = (!info.native_has_rate_divisor || !info.native_has_zero_divisor ||
                          (!info.native_has_ext && !info.native_has_khr));

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_device_needs_emulation[(uint64_t)(uintptr_t)device] = needs_emu;
            m_last_device = device;
        }

        LOGI("Device %p created: divisor emulation %s", device,
             needs_emu ? "ENABLED (sub-draw batching & stride rewrite)" : "DISABLED (driver native)");
    }
}

void VertexAttributeDivisorModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_needs_emulation.erase((uint64_t)(uintptr_t)device);
    for (auto it = m_cmd_devices.begin(); it != m_cmd_devices.end(); ) {
        if (it->second == device) {
            m_cmd_states.erase(it->first);
            it = m_cmd_devices.erase(it);
        } else {
            ++it;
        }
    }
    if (m_last_device.load() == device) {
        m_last_device = VK_NULL_HANDLE;
    }
}

bool VertexAttributeDivisorModule::needs_pipeline_interception(
    VkDevice device,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos
) {
    if (is_device_native(device) || !pCreateInfos || createInfoCount == 0) return false;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        const VkPipelineVertexInputStateCreateInfo* vi = pCreateInfos[i].pVertexInputState;
        if (vi != NULL && vi->pNext != NULL) {
            if (vku::has_pnext(vi->pNext, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) ||
                vku::has_pnext(vi->pNext, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR)) {
                return true;
            }
        }
    }

    return false;
}

void VertexAttributeDivisorModule::on_modify_pipeline_create_info(
    VkDevice device,
    uint32_t index,
    VkGraphicsPipelineCreateInfo& createInfo,
    VkPipelineVertexInputStateCreateInfo& viState,
    std::vector<void*>& allocationsToFree
) {
    if (is_device_native(device) || viState.pNext == NULL) return;

    const auto* divState = vku::find_pnext<VkPipelineVertexInputDivisorStateCreateInfoEXT>(
        viState.pNext, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT);
    if (!divState) {
        divState = vku::find_pnext<VkPipelineVertexInputDivisorStateCreateInfoEXT>(
            viState.pNext, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR);
    }

    if (!divState || divState->vertexBindingDivisorCount == 0 || !divState->pVertexBindingDivisors) return;

    PipelineDivisorInfo info;

    // Collect divisor descriptions
    for (uint32_t d = 0; d < divState->vertexBindingDivisorCount; d++) {
        uint32_t b = divState->pVertexBindingDivisors[d].binding;
        uint32_t div = divState->pVertexBindingDivisors[d].divisor;

        uint32_t orig_stride = 0;
        if (viState.vertexBindingDescriptionCount > 0 && viState.pVertexBindingDescriptions != NULL) {
            for (uint32_t vb = 0; vb < viState.vertexBindingDescriptionCount; vb++) {
                if (viState.pVertexBindingDescriptions[vb].binding == b) {
                    orig_stride = viState.pVertexBindingDescriptions[vb].stride;
                    break;
                }
            }
        }

        BindingDivisorInfo bInfo;
        bInfo.binding = b;
        bInfo.divisor = div;
        bInfo.original_stride = orig_stride;
        info.divisors.push_back(bInfo);

        if (div > 1) {
            info.has_divisor_greater_than_one = true;
            LOG_OPT_DEBUG("Pipeline %u: binding %u configured with divisor %u (stride %u)", index, b, div, orig_stride);
        }
    }

    // Set stride = 0 for all bindings with divisor == 0 or divisor > 1
    // (stride = 0 ensures the GPU fixed-function vertex fetcher reads from the exact offset we bind per sub-draw)
    if (viState.vertexBindingDescriptionCount > 0 && viState.pVertexBindingDescriptions != NULL) {
        VkVertexInputBindingDescription* modBindings = (VkVertexInputBindingDescription*)
            malloc(sizeof(VkVertexInputBindingDescription) * viState.vertexBindingDescriptionCount);
        if (modBindings) {
            memcpy(modBindings, viState.pVertexBindingDescriptions, sizeof(VkVertexInputBindingDescription) * viState.vertexBindingDescriptionCount);
            for (const auto& b : info.divisors) {
                if (b.divisor == 0 || b.divisor > 1) {
                    for (uint32_t vb = 0; vb < viState.vertexBindingDescriptionCount; vb++) {
                        if (modBindings[vb].binding == b.binding) {
                            modBindings[vb].stride = 0;
                            LOG_OPT_DEBUG("Pipeline %u: binding %u set stride=0 for emulation (original stride %u, divisor %u)",
                                index, b.binding, b.original_stride, b.divisor);
                            break;
                        }
                    }
                }
            }
            viState.pVertexBindingDescriptions = modBindings;
            allocationsToFree.push_back(modBindings);
        }
    }

    // Unlink divisor state struct from pNext so native driver does not receive it
    vku::remove_pnext(viState.pNext, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT);
    vku::remove_pnext(viState.pNext, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR);

    createInfo.pVertexInputState = &viState;

    // Save pending info
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending_divisors[&createInfo][index] = info;
}

void VertexAttributeDivisorModule::on_post_create_graphics_pipelines(
    VkDevice device,
    uint32_t count,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkPipeline* pPipelines
) {
    if (is_device_native(device) || !pCreateInfos || !pPipelines || count == 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pending_divisors.find(pCreateInfos);
    if (it == m_pending_divisors.end()) return;

    for (uint32_t i = 0; i < count; i++) {
        auto pIt = it->second.find(i);
        if (pIt != it->second.end() && pPipelines[i] != VK_NULL_HANDLE) {
            m_pipeline_divisors[(uint64_t)(uintptr_t)pPipelines[i]] =
                std::make_shared<PipelineDivisorInfo>(pIt->second);
            if (pIt->second.has_divisor_greater_than_one) {
                m_has_divisor_pipelines.store(true, std::memory_order_release);
                LOGI("Pipeline %p registered with vertexAttributeInstanceRateDivisor emulation", (void*)pPipelines[i]);
            }
        }
    }

    m_pending_divisors.erase(it);
}

void VertexAttributeDivisorModule::on_destroy_pipeline(
    VkDevice device,
    VkPipeline pipeline
) {
    if (pipeline == VK_NULL_HANDLE) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pipeline_divisors.erase((uint64_t)(uintptr_t)pipeline);
}

void VertexAttributeDivisorModule::on_post_allocate_command_buffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkResult result,
    VkCommandBuffer* pCommandBuffers
) {
    if (result == VK_SUCCESS && pAllocateInfo && pCommandBuffers) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
            m_cmd_devices[(uint64_t)(uintptr_t)pCommandBuffers[i]] = device;
        }
    }
}

void VertexAttributeDivisorModule::on_free_command_buffers(
    VkDevice device,
    uint32_t count,
    const VkCommandBuffer* pCommandBuffers
) {
    if (!pCommandBuffers) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0; i < count; i++) {
        m_cmd_devices.erase((uint64_t)(uintptr_t)pCommandBuffers[i]);
        m_cmd_states.erase((uint64_t)(uintptr_t)pCommandBuffers[i]);
    }
}

void VertexAttributeDivisorModule::on_reset_command_buffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cmd_states.find((uint64_t)(uintptr_t)commandBuffer);
    if (it != m_cmd_states.end()) {
        it->second->current_pipeline = VK_NULL_HANDLE;
        it->second->active_divisor_info = nullptr;
    }
}

void VertexAttributeDivisorModule::on_cmd_bind_pipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline
) {
    if (!m_has_divisor_pipelines.load(std::memory_order_relaxed)) return;
    if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) return;

    CmdBufferState* state = get_or_create_cmd_state(commandBuffer);
    if (!state) return;

    state->current_pipeline = pipeline;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pipeline_divisors.find((uint64_t)(uintptr_t)pipeline);
    if (it != m_pipeline_divisors.end()) {
        state->active_divisor_info = it->second;
    } else {
        state->active_divisor_info = nullptr;
    }
}

void VertexAttributeDivisorModule::on_cmd_bind_vertex_buffers(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets
) {
    if (!m_has_divisor_pipelines.load(std::memory_order_relaxed)) return;
    if (!pBuffers || !pOffsets || bindingCount == 0) return;

    CmdBufferState* state = get_or_create_cmd_state(commandBuffer);
    if (!state) return;

    for (uint32_t i = 0; i < bindingCount; i++) {
        uint32_t b = firstBinding + i;
        if (b < CmdBufferState::MAX_VERTEX_BINDINGS) {
            state->bound_buffers[b] = pBuffers[i];
            state->bound_offsets[b] = pOffsets[i];
        }
    }
}

bool VertexAttributeDivisorModule::on_cmd_draw(
    VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance
) {
    if (!m_has_divisor_pipelines.load(std::memory_order_relaxed) || instanceCount == 0) return false;

    CmdBufferState* state = get_or_create_cmd_state(commandBuffer);
    if (!state || !state->active_divisor_info || !state->active_divisor_info->has_divisor_greater_than_one) {
        return false;
    }

    const auto& div_info = *state->active_divisor_info;
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkCmdBindVertexBuffers real_bind_vb = dt.CmdBindVertexBuffers;
    PFN_vkCmdDraw real_draw = dt.CmdDraw;
    if (!real_bind_vb || !real_draw) return false;

    uint32_t inst = firstInstance;
    uint32_t remaining = instanceCount;

    while (remaining > 0) {
        // Calculate max instances in this chunk such that all divisor element indices remain constant
        uint32_t chunk_size = remaining;
        for (const auto& b : div_info.divisors) {
            if (b.divisor > 1) {
                uint32_t elem = inst / b.divisor;
                uint32_t next_boundary = (elem + 1) * b.divisor;
                uint32_t can_draw = next_boundary - inst;
                if (can_draw < chunk_size) {
                    chunk_size = can_draw;
                }
            }
        }

        // For each divisor binding, temporarily rebind with the element offset
        for (const auto& b : div_info.divisors) {
            if (b.divisor > 1 && b.binding < CmdBufferState::MAX_VERTEX_BINDINGS && state->bound_buffers[b.binding] != VK_NULL_HANDLE) {
                uint32_t elem = inst / b.divisor;
                VkDeviceSize orig_offset = state->bound_offsets[b.binding];
                VkDeviceSize new_offset = orig_offset + (VkDeviceSize)elem * b.original_stride;
                VkBuffer buf = state->bound_buffers[b.binding];
                real_bind_vb(commandBuffer, b.binding, 1, &buf, &new_offset);
            }
        }

        real_draw(commandBuffer, vertexCount, chunk_size, firstVertex, inst);

        inst += chunk_size;
        remaining -= chunk_size;
    }

    // Restore original offsets on the command buffer
    for (const auto& b : div_info.divisors) {
        if (b.divisor > 1 && b.binding < CmdBufferState::MAX_VERTEX_BINDINGS && state->bound_buffers[b.binding] != VK_NULL_HANDLE) {
            VkDeviceSize orig_offset = state->bound_offsets[b.binding];
            VkBuffer buf = state->bound_buffers[b.binding];
            real_bind_vb(commandBuffer, b.binding, 1, &buf, &orig_offset);
        }
    }

    return true;
}

bool VertexAttributeDivisorModule::on_cmd_draw_indexed(
    VkCommandBuffer commandBuffer,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance
) {
    if (!m_has_divisor_pipelines.load(std::memory_order_relaxed) || instanceCount == 0) return false;

    CmdBufferState* state = get_or_create_cmd_state(commandBuffer);
    if (!state || !state->active_divisor_info || !state->active_divisor_info->has_divisor_greater_than_one) {
        return false;
    }

    const auto& div_info = *state->active_divisor_info;
    VkDevice device = get_device_for_cmd(commandBuffer);
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkCmdBindVertexBuffers real_bind_vb = dt.CmdBindVertexBuffers;
    PFN_vkCmdDrawIndexed real_draw_indexed = dt.CmdDrawIndexed;
    if (!real_bind_vb || !real_draw_indexed) return false;

    uint32_t inst = firstInstance;
    uint32_t remaining = instanceCount;

    while (remaining > 0) {
        uint32_t chunk_size = remaining;
        for (const auto& b : div_info.divisors) {
            if (b.divisor > 1) {
                uint32_t elem = inst / b.divisor;
                uint32_t next_boundary = (elem + 1) * b.divisor;
                uint32_t can_draw = next_boundary - inst;
                if (can_draw < chunk_size) {
                    chunk_size = can_draw;
                }
            }
        }

        for (const auto& b : div_info.divisors) {
            if (b.divisor > 1 && b.binding < CmdBufferState::MAX_VERTEX_BINDINGS && state->bound_buffers[b.binding] != VK_NULL_HANDLE) {
                uint32_t elem = inst / b.divisor;
                VkDeviceSize orig_offset = state->bound_offsets[b.binding];
                VkDeviceSize new_offset = orig_offset + (VkDeviceSize)elem * b.original_stride;
                VkBuffer buf = state->bound_buffers[b.binding];
                real_bind_vb(commandBuffer, b.binding, 1, &buf, &new_offset);
            }
        }

        real_draw_indexed(commandBuffer, indexCount, chunk_size, firstIndex, vertexOffset, inst);

        inst += chunk_size;
        remaining -= chunk_size;
    }

    // Restore original offsets on the command buffer
    for (const auto& b : div_info.divisors) {
        if (b.divisor > 1 && b.binding < CmdBufferState::MAX_VERTEX_BINDINGS && state->bound_buffers[b.binding] != VK_NULL_HANDLE) {
            VkDeviceSize orig_offset = state->bound_offsets[b.binding];
            VkBuffer buf = state->bound_buffers[b.binding];
            real_bind_vb(commandBuffer, b.binding, 1, &buf, &orig_offset);
        }
    }

    return true;
}

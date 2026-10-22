#include "multi_draw_indirect.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"

REGISTER_LAYER_MODULE(MultiDrawIndirectModule);

MultiDrawIndirectModule::MultiDrawIndirectModule() {
    LOGI("Initialized Vulkan multiDrawIndirect emulation module");
}

bool MultiDrawIndirectModule::is_phys_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    EmulationMode mode = parse_emulation_mode("VULKAN_FIX_EMULATE_MULTI_DRAW_INDIRECT", "FORCE_EMULATE_MULTI_DRAW_INDIRECT");
    if (mode == EmulationMode::ForceEmulate) {
        LOGI("Multi Draw Indirect: FORCE EMULATE enabled for physical device %p", physDev);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }
    if (mode == EmulationMode::Skip) {
        LOGI("Multi Draw Indirect: SKIP/BYPASS enabled for physical device %p", physDev);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = true;
        return true;
    }

    PFN_vkGetPhysicalDeviceFeatures real_fn =
        (PFN_vkGetPhysicalDeviceFeatures) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures");
    bool native = false;
    if (real_fn) {
        VkPhysicalDeviceFeatures feat{};
        real_fn(physDev, &feat);
        native = (feat.multiDrawIndirect == VK_TRUE);
    }
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native multiDrawIndirect, enabling automatic emulation!", physDev);
    } else {
        LOGI("Physical device %p natively supports multiDrawIndirect", physDev);
    }
    return native;
}

bool MultiDrawIndirectModule::is_device_native(VkDevice device) {
    if (device == VK_NULL_HANDLE) return false;
    if (__builtin_expect(device == m_cached_device.load(std::memory_order_relaxed), 1)) {
        return m_cached_device_native.load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_native_support.find((uint64_t)(uintptr_t)device);
    if (it != m_device_native_support.end()) {
        m_cached_device.store(device, std::memory_order_relaxed);
        m_cached_device_native.store(it->second, std::memory_order_relaxed);
        return it->second;
    }
    if (!m_phys_native_support.empty()) {
        bool all_native = true;
        for (const auto& kv : m_phys_native_support) {
            if (!kv.second) {
                all_native = false;
                break;
            }
        }
        return all_native;
    }
    return false;
}

void MultiDrawIndirectModule::on_get_features(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures
) {
    if (!pFeatures) return;
    if (!is_phys_device_native(physicalDevice)) {
        pFeatures->multiDrawIndirect = VK_TRUE;
        LOG_OPT_DEBUG("Emulated multiDrawIndirect = VK_TRUE in vkGetPhysicalDeviceFeatures");
    }
}

void MultiDrawIndirectModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures) return;
    if (!is_phys_device_native(physicalDevice)) {
        pFeatures->features.multiDrawIndirect = VK_TRUE;
        LOG_OPT_DEBUG("Emulated multiDrawIndirect = VK_TRUE in vkGetPhysicalDeviceFeatures2");
    }
}

void MultiDrawIndirectModule::on_get_properties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties
) {
    if (!pProperties) return;
    if (!is_phys_device_native(physicalDevice)) {
        if (pProperties->limits.maxDrawIndirectCount < 65536) {
            pProperties->limits.maxDrawIndirectCount = 65536;
            LOG_OPT_DEBUG("Emulated multiDrawIndirect: bumped maxDrawIndirectCount to 65536 in vkGetPhysicalDeviceProperties");
        }
    }
}

void MultiDrawIndirectModule::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties) return;
    if (!is_phys_device_native(physicalDevice)) {
        if (pProperties->properties.limits.maxDrawIndirectCount < 65536) {
            pProperties->properties.limits.maxDrawIndirectCount = 65536;
            LOG_OPT_DEBUG("Emulated multiDrawIndirect: bumped maxDrawIndirectCount to 65536 in vkGetPhysicalDeviceProperties2");
        }
    }
}

void MultiDrawIndirectModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // 1. If application enabled multiDrawIndirect in pEnabledFeatures, strip it for real driver
    if (pEnabledFeatures && pEnabledFeatures->multiDrawIndirect) {
        pEnabledFeatures->multiDrawIndirect = VK_FALSE;
        LOGI("vkCreateDevice: stripped multiDrawIndirect from pEnabledFeatures to prevent hardware driver failure");
    }

    // 2. If application chained VkPhysicalDeviceFeatures2 in pCreateInfo->pNext, strip it as well
    void* curr = (void*) pCreateInfo->pNext;
    while (curr != NULL) {
        VkBaseOutStructure* h = (VkBaseOutStructure*) curr;
        if (h->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            VkPhysicalDeviceFeatures2* f2 = (VkPhysicalDeviceFeatures2*) h;
            if (f2->features.multiDrawIndirect) {
                f2->features.multiDrawIndirect = VK_FALSE;
                LOGI("vkCreateDevice: stripped multiDrawIndirect from VkPhysicalDeviceFeatures2 in pNext");
            }
        }
        curr = (void*) h->pNext;
    }
}

void MultiDrawIndirectModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_native_support[(uint64_t)(uintptr_t)device] = native;
        m_cached_device.store(device, std::memory_order_release);
        m_cached_device_native.store(native, std::memory_order_release);
        LOGI("Device %p created: multiDrawIndirect %s", device, native ? "NATIVE" : "EMULATED");
    }
}

void MultiDrawIndirectModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_native_support.erase((uint64_t)(uintptr_t)device);
    if (m_cached_device.load(std::memory_order_relaxed) == device) {
        m_cached_device.store(VK_NULL_HANDLE, std::memory_order_release);
    }
}

bool MultiDrawIndirectModule::on_cmd_draw_indirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride
) {
    if (drawCount == 0) return true;

    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    if (stride == 0) {
        stride = sizeof(VkDrawIndirectCommand);
    }

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkCmdDrawIndirect real_fn = dt.CmdDrawIndirect;
    if (!real_fn) {
        real_fn = (PFN_vkCmdDrawIndirect) get_real_proc(get_last_instance(), device, "vkCmdDrawIndirect");
    }

    if (real_fn) {
        for (uint32_t i = 0; i < drawCount; ++i) {
            VkDeviceSize drawOffset = offset + static_cast<VkDeviceSize>(i) * stride;
            real_fn(commandBuffer, buffer, drawOffset, 1, stride);
        }
    }
    return true;
}

bool MultiDrawIndirectModule::on_cmd_draw_indexed_indirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride
) {
    if (drawCount == 0) return true;

    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    if (stride == 0) {
        stride = sizeof(VkDrawIndexedIndirectCommand);
    }

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkCmdDrawIndexedIndirect real_fn = dt.CmdDrawIndexedIndirect;
    if (!real_fn) {
        real_fn = (PFN_vkCmdDrawIndexedIndirect) get_real_proc(get_last_instance(), device, "vkCmdDrawIndexedIndirect");
    }

    if (real_fn) {
        for (uint32_t i = 0; i < drawCount; ++i) {
            VkDeviceSize drawOffset = offset + static_cast<VkDeviceSize>(i) * stride;
            real_fn(commandBuffer, buffer, drawOffset, 1, stride);
        }
    }
    return true;
}

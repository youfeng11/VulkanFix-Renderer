#include "draw_indirect_first_instance.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"

REGISTER_LAYER_MODULE(DrawIndirectFirstInstanceModule);

DrawIndirectFirstInstanceModule::DrawIndirectFirstInstanceModule() {
    LOGI("Initialized Vulkan drawIndirectFirstInstance emulation module");
}

bool DrawIndirectFirstInstanceModule::is_phys_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    const char* force_emu = getenv("FORCE_EMULATE_DRAW_INDIRECT_FIRST_INSTANCE");
    if (force_emu && (strcmp(force_emu, "1") == 0 || strcasecmp(force_emu, "true") == 0)) {
        LOGI("FORCE_EMULATE_DRAW_INDIRECT_FIRST_INSTANCE set, enabling emulation for physical device %p", physDev);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }

    PFN_vkGetPhysicalDeviceFeatures real_fn =
        (PFN_vkGetPhysicalDeviceFeatures) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures");
    bool native = false;
    if (real_fn) {
        VkPhysicalDeviceFeatures feat{};
        real_fn(physDev, &feat);
        native = (feat.drawIndirectFirstInstance == VK_TRUE);
    }
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native drawIndirectFirstInstance, enabling automatic emulation!", physDev);
    } else {
        LOGI("Physical device %p natively supports drawIndirectFirstInstance", physDev);
    }
    return native;
}

bool DrawIndirectFirstInstanceModule::is_device_native(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_native_support.find((uint64_t)(uintptr_t)device);
    if (it != m_device_native_support.end()) {
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

void DrawIndirectFirstInstanceModule::on_get_features(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures
) {
    if (!pFeatures) return;
    if (!is_phys_device_native(physicalDevice)) {
        pFeatures->drawIndirectFirstInstance = VK_TRUE;
        LOG_OPT_DEBUG("Emulated drawIndirectFirstInstance = VK_TRUE in vkGetPhysicalDeviceFeatures");
    }
}

void DrawIndirectFirstInstanceModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures) return;
    if (!is_phys_device_native(physicalDevice)) {
        pFeatures->features.drawIndirectFirstInstance = VK_TRUE;
        LOG_OPT_DEBUG("Emulated drawIndirectFirstInstance = VK_TRUE in vkGetPhysicalDeviceFeatures2");
    }
}

void DrawIndirectFirstInstanceModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // 1. If application enabled drawIndirectFirstInstance in pEnabledFeatures, strip it for real driver
    if (pEnabledFeatures && pEnabledFeatures->drawIndirectFirstInstance) {
        pEnabledFeatures->drawIndirectFirstInstance = VK_FALSE;
        LOGI("vkCreateDevice: stripped drawIndirectFirstInstance from pEnabledFeatures to prevent hardware driver failure");
    }

    // 2. If application chained VkPhysicalDeviceFeatures2 in pCreateInfo->pNext, strip it as well
    void* curr = (void*) pCreateInfo->pNext;
    while (curr != NULL) {
        VkBaseOutStructure* h = (VkBaseOutStructure*) curr;
        if (h->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            VkPhysicalDeviceFeatures2* f2 = (VkPhysicalDeviceFeatures2*) h;
            if (f2->features.drawIndirectFirstInstance) {
                f2->features.drawIndirectFirstInstance = VK_FALSE;
                LOGI("vkCreateDevice: stripped drawIndirectFirstInstance from VkPhysicalDeviceFeatures2 in pNext");
            }
        }
        curr = (void*) h->pNext;
    }
}

void DrawIndirectFirstInstanceModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_native_support[(uint64_t)(uintptr_t)device] = native;
        LOGI("Device %p created: drawIndirectFirstInstance %s", device, native ? "NATIVE" : "EMULATED");
    }
}

void DrawIndirectFirstInstanceModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_native_support.erase((uint64_t)(uintptr_t)device);
}

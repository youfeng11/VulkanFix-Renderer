#include "vertex_pipeline_stores_and_atomics.h"
#include "core/vertex_store_emulator.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"

// Automatic compute/replay fallback. Unsupported pipeline and command forms fail
// explicitly rather than executing vertex stores on an unsupported driver.
static EmulationMode compute_emulation_mode() {
    const auto feature =
        parse_emulation_mode("VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE",
                             "FORCE_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE");
    if (feature != EmulationMode::Auto)
        return feature;
    return parse_emulation_mode("VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS",
                                "FORCE_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS");
}

REGISTER_LAYER_MODULE(VertexPipelineStoresAndAtomicsModule);

VertexPipelineStoresAndAtomicsModule::VertexPipelineStoresAndAtomicsModule() {
    LOGI("Initialized vertexPipelineStoresAndAtomics capability module");
    if (compute_emulation_mode() == EmulationMode::ForceEmulate) {
        LOGW("vertexPipelineStoresAndAtomics: compute/replay fallback selected; unsupported draws "
             "will fail explicitly");
    }
}

bool VertexPipelineStoresAndAtomicsModule::is_phys_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    PFN_vkGetPhysicalDeviceFeatures real_fn = (PFN_vkGetPhysicalDeviceFeatures)get_real_proc(
        get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures");
    bool native = false;
    if (real_fn) {
        VkPhysicalDeviceFeatures feat{};
        real_fn(physDev, &feat);
        native = (feat.vertexPipelineStoresAndAtomics == VK_TRUE);
    }
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native vertexPipelineStoresAndAtomics; compute/replay "
             "fallback available",
             physDev);
    } else {
        LOGI("Physical device %p natively supports vertexPipelineStoresAndAtomics", physDev);
    }
    return native;
}

bool VertexPipelineStoresAndAtomicsModule::should_emulate(VkPhysicalDevice physicalDevice) {
    const auto mode = compute_emulation_mode();
    if (mode == EmulationMode::Skip)
        return false;
    return mode == EmulationMode::ForceEmulate || !is_phys_device_native(physicalDevice);
}

bool VertexPipelineStoresAndAtomicsModule::is_device_native(VkDevice device) {
    if (device == VK_NULL_HANDLE)
        return false;
    bool cached_native;
    if (m_native_cache.lookup(device, cached_native)) return cached_native;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_native_support.find((uint64_t)(uintptr_t)device);
    if (it != m_device_native_support.end()) {
        m_native_cache.remember(device, it->second);
        return it->second;
    }
    if (!m_phys_native_support.empty()) {
        bool all_native = true;
        for (const auto &kv : m_phys_native_support) {
            if (!kv.second) {
                all_native = false;
                break;
            }
        }
        return all_native;
    }
    return false;
}

void VertexPipelineStoresAndAtomicsModule::on_get_features(VkPhysicalDevice physicalDevice,
                                                           VkPhysicalDeviceFeatures *pFeatures) {
    if (!pFeatures)
        return;
    if (should_emulate(physicalDevice)) {
        pFeatures->vertexPipelineStoresAndAtomics = VK_TRUE;
        LOG_OPT_DEBUG(
            "Emulated vertexPipelineStoresAndAtomics = VK_TRUE in vkGetPhysicalDeviceFeatures");
    }
}

void VertexPipelineStoresAndAtomicsModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures, void *pUserData) {
    if (!pFeatures)
        return;
    if (should_emulate(physicalDevice)) {
        pFeatures->features.vertexPipelineStoresAndAtomics = VK_TRUE;
        void *curr = pFeatures->pNext;
        while (curr != nullptr) {
            auto *h = static_cast<VkBaseOutStructure *>(curr);
            if (h->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                auto *f2 = reinterpret_cast<VkPhysicalDeviceFeatures2 *>(h);
                f2->features.vertexPipelineStoresAndAtomics = VK_TRUE;
            }
            curr = h->pNext;
        }
        LOG_OPT_DEBUG(
            "Emulated vertexPipelineStoresAndAtomics = VK_TRUE in vkGetPhysicalDeviceFeatures2");
    }
}

void VertexPipelineStoresAndAtomicsModule::on_get_properties(VkPhysicalDevice,
                                                             VkPhysicalDeviceProperties *) {}
void VertexPipelineStoresAndAtomicsModule::on_post_get_properties2(VkPhysicalDevice,
                                                                   VkPhysicalDeviceProperties2 *,
                                                                   void *) {}

void VertexPipelineStoresAndAtomicsModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice, VkDeviceCreateInfo *pCreateInfo,
    VkPhysicalDeviceFeatures *pEnabledFeatures, std::vector<const char *> &enabledExtensions,
    void *&pUserData) {
    pUserData = nullptr;
    if (!pCreateInfo || !should_emulate(physicalDevice))
        return;

    // 1. If application enabled vertexPipelineStoresAndAtomics in pEnabledFeatures, strip it for
    // real driver
    if (pEnabledFeatures && pEnabledFeatures->vertexPipelineStoresAndAtomics) {
        pUserData = reinterpret_cast<void *>(uintptr_t(1));
        pEnabledFeatures->vertexPipelineStoresAndAtomics = VK_FALSE;
        LOGI("vkCreateDevice: stripped vertexPipelineStoresAndAtomics from pEnabledFeatures to "
             "prevent hardware driver failure");
    }

    // 2. If application chained VkPhysicalDeviceFeatures2 in pCreateInfo->pNext, strip it as well
    void *curr = (void *)pCreateInfo->pNext;
    while (curr != NULL) {
        VkBaseOutStructure *h = (VkBaseOutStructure *)curr;
        if (h->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            VkPhysicalDeviceFeatures2 *f2 = (VkPhysicalDeviceFeatures2 *)h;
            if (f2->features.vertexPipelineStoresAndAtomics) {
                pUserData = reinterpret_cast<void *>(uintptr_t(1));
                f2->features.vertexPipelineStoresAndAtomics = VK_FALSE;
                LOGI("vkCreateDevice: stripped vertexPipelineStoresAndAtomics from "
                     "VkPhysicalDeviceFeatures2 in pNext");
            }
        }
        curr = (void *)h->pNext;
    }
}

void VertexPipelineStoresAndAtomicsModule::on_post_create_device(VkPhysicalDevice physicalDevice,
                                                                 VkDevice device, VkResult result,
                                                                 void *pUserData) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        // Remember this create call's decision, including whether the feature
        // was requested. Environment changes cannot reroute an existing device.
        const bool emulate = pUserData != nullptr;
        bool native = is_phys_device_native(physicalDevice) && !emulate;
        if (emulate)
            VertexStoreEmulator::get().enable(device, physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_native_support[(uint64_t)(uintptr_t)device] = native;
        m_native_cache.invalidate();
        LOGI("Device %p created: vertexPipelineStoresAndAtomics %s", device,
             native ? "NATIVE" : (emulate ? "EMULATED" : "DISABLED"));
    }
}

void VertexPipelineStoresAndAtomicsModule::on_destroy_device(VkDevice device) {
    VertexStoreEmulator::get().destroy_device(device);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_native_support.erase((uint64_t)(uintptr_t)device);
    m_native_cache.invalidate();
}

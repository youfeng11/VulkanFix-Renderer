#include "vertex_pipeline_stores_and_atomics.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"

REGISTER_LAYER_MODULE(VertexPipelineStoresAndAtomicsModule);

VertexPipelineStoresAndAtomicsModule::VertexPipelineStoresAndAtomicsModule() {
    LOGI("Initialized Vulkan vertexPipelineStoresAndAtomics emulation module");
}

bool VertexPipelineStoresAndAtomicsModule::is_phys_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    EmulationMode mode = parse_emulation_mode(
        "VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS",
        "FORCE_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS");
    if (mode == EmulationMode::ForceEmulate) {
        LOGI("Vertex Pipeline Stores And Atomics: FORCE EMULATE enabled for physical device %p", physDev);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }
    if (mode == EmulationMode::Skip) {
        LOGI("Vertex Pipeline Stores And Atomics: SKIP/BYPASS enabled for physical device %p", physDev);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = true;
        return true;
    }

    PFN_vkGetPhysicalDeviceFeatures real_fn =
        (PFN_vkGetPhysicalDeviceFeatures) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures");
    bool native = false;
    if (real_fn) {
        VkPhysicalDeviceFeatures feat{};
        real_fn(physDev, &feat);
        native = (feat.vertexPipelineStoresAndAtomics == VK_TRUE);
    }
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native vertexPipelineStoresAndAtomics, enabling automatic emulation!", physDev);
    } else {
        LOGI("Physical device %p natively supports vertexPipelineStoresAndAtomics", physDev);
    }
    return native;
}

bool VertexPipelineStoresAndAtomicsModule::is_device_native(VkDevice device) {
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

void VertexPipelineStoresAndAtomicsModule::on_get_features(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures
) {
    if (!pFeatures) return;
    EmulationMode featMode = parse_emulation_mode(
        "VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE",
        "FORCE_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE");
    bool should_inject = false;
    if (featMode == EmulationMode::ForceEmulate) {
        should_inject = true;
    } else if (featMode == EmulationMode::Skip) {
        should_inject = false;
    } else {
        should_inject = !is_phys_device_native(physicalDevice);
    }

    if (should_inject) {
        pFeatures->vertexPipelineStoresAndAtomics = VK_TRUE;
        LOG_OPT_DEBUG("Emulated vertexPipelineStoresAndAtomics = VK_TRUE in vkGetPhysicalDeviceFeatures");
    }
}

void VertexPipelineStoresAndAtomicsModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures) return;
    EmulationMode featMode = parse_emulation_mode(
        "VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE",
        "FORCE_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE");
    bool should_inject = false;
    if (featMode == EmulationMode::ForceEmulate) {
        should_inject = true;
    } else if (featMode == EmulationMode::Skip) {
        should_inject = false;
    } else {
        should_inject = !is_phys_device_native(physicalDevice);
    }

    if (should_inject) {
        pFeatures->features.vertexPipelineStoresAndAtomics = VK_TRUE;
        void* curr = pFeatures->pNext;
        while (curr != nullptr) {
            auto* h = static_cast<VkBaseOutStructure*>(curr);
            if (h->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                auto* f2 = reinterpret_cast<VkPhysicalDeviceFeatures2*>(h);
                f2->features.vertexPipelineStoresAndAtomics = VK_TRUE;
            }
            curr = h->pNext;
        }
        LOG_OPT_DEBUG("Emulated vertexPipelineStoresAndAtomics = VK_TRUE in vkGetPhysicalDeviceFeatures2");
    }
}

void VertexPipelineStoresAndAtomicsModule::on_get_properties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties
) {
    if (!pProperties) return;
    if (!is_phys_device_native(physicalDevice)) {
        if (pProperties->limits.maxPerStageDescriptorStorageBuffers < 4) {
            pProperties->limits.maxPerStageDescriptorStorageBuffers = 4;
            LOG_OPT_DEBUG("Emulated vertexPipelineStoresAndAtomics: bumped maxPerStageDescriptorStorageBuffers to 4 in vkGetPhysicalDeviceProperties");
        }
        if (pProperties->limits.maxPerStageDescriptorStorageImages < 4) {
            pProperties->limits.maxPerStageDescriptorStorageImages = 4;
            LOG_OPT_DEBUG("Emulated vertexPipelineStoresAndAtomics: bumped maxPerStageDescriptorStorageImages to 4 in vkGetPhysicalDeviceProperties");
        }
    }
}

void VertexPipelineStoresAndAtomicsModule::on_post_get_properties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties,
    void* pUserData
) {
    if (!pProperties) return;
    if (!is_phys_device_native(physicalDevice)) {
        if (pProperties->properties.limits.maxPerStageDescriptorStorageBuffers < 4) {
            pProperties->properties.limits.maxPerStageDescriptorStorageBuffers = 4;
            LOG_OPT_DEBUG("Emulated vertexPipelineStoresAndAtomics: bumped maxPerStageDescriptorStorageBuffers to 4 in vkGetPhysicalDeviceProperties2");
        }
        if (pProperties->properties.limits.maxPerStageDescriptorStorageImages < 4) {
            pProperties->properties.limits.maxPerStageDescriptorStorageImages = 4;
            LOG_OPT_DEBUG("Emulated vertexPipelineStoresAndAtomics: bumped maxPerStageDescriptorStorageImages to 4 in vkGetPhysicalDeviceProperties2");
        }
    }
}

void VertexPipelineStoresAndAtomicsModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // 1. If application enabled vertexPipelineStoresAndAtomics in pEnabledFeatures, strip it for real driver
    if (pEnabledFeatures && pEnabledFeatures->vertexPipelineStoresAndAtomics) {
        pEnabledFeatures->vertexPipelineStoresAndAtomics = VK_FALSE;
        LOGI("vkCreateDevice: stripped vertexPipelineStoresAndAtomics from pEnabledFeatures to prevent hardware driver failure");
    }

    // 2. If application chained VkPhysicalDeviceFeatures2 in pCreateInfo->pNext, strip it as well
    void* curr = (void*) pCreateInfo->pNext;
    while (curr != NULL) {
        VkBaseOutStructure* h = (VkBaseOutStructure*) curr;
        if (h->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            VkPhysicalDeviceFeatures2* f2 = (VkPhysicalDeviceFeatures2*) h;
            if (f2->features.vertexPipelineStoresAndAtomics) {
                f2->features.vertexPipelineStoresAndAtomics = VK_FALSE;
                LOGI("vkCreateDevice: stripped vertexPipelineStoresAndAtomics from VkPhysicalDeviceFeatures2 in pNext");
            }
        }
        curr = (void*) h->pNext;
    }
}

void VertexPipelineStoresAndAtomicsModule::on_post_create_device(
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
        LOGI("Device %p created: vertexPipelineStoresAndAtomics %s", device, native ? "NATIVE" : "EMULATED");
    }
}

void VertexPipelineStoresAndAtomicsModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_native_support.erase((uint64_t)(uintptr_t)device);
    if (m_cached_device.load(std::memory_order_relaxed) == device) {
        m_cached_device.store(VK_NULL_HANDLE, std::memory_order_release);
    }
}

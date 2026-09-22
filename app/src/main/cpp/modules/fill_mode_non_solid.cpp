#include "fill_mode_non_solid.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"

REGISTER_LAYER_MODULE(FillModeNonSolidModule);

FillModeNonSolidModule::FillModeNonSolidModule() {
    LOGI("Initialized Vulkan fillModeNonSolid emulation module");
}

bool FillModeNonSolidModule::is_phys_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    const char* force_emu = getenv("FORCE_EMULATE_FILL_MODE_NON_SOLID");
    if (force_emu && (strcmp(force_emu, "1") == 0 || strcasecmp(force_emu, "true") == 0)) {
        LOGI("FORCE_EMULATE_FILL_MODE_NON_SOLID set, enabling emulation for physical device %p", physDev);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }

    PFN_vkGetPhysicalDeviceFeatures real_fn =
        (PFN_vkGetPhysicalDeviceFeatures) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures");
    bool native = false;
    if (real_fn) {
        VkPhysicalDeviceFeatures feat{};
        real_fn(physDev, &feat);
        native = (feat.fillModeNonSolid == VK_TRUE);
    }
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native fillModeNonSolid, enabling automatic emulation!", physDev);
    } else {
        LOGI("Physical device %p natively supports fillModeNonSolid", physDev);
    }
    return native;
}

bool FillModeNonSolidModule::is_device_native(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_native_support.find((uint64_t)(uintptr_t)device);
    if (it != m_device_native_support.end()) {
        return it->second;
    }
    return false;
}

void FillModeNonSolidModule::on_get_features(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures
) {
    if (!pFeatures) return;
    if (!is_phys_device_native(physicalDevice)) {
        pFeatures->fillModeNonSolid = VK_TRUE;
        LOG_OPT_DEBUG("Emulated fillModeNonSolid = VK_TRUE in vkGetPhysicalDeviceFeatures");
    }
}

void FillModeNonSolidModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures) return;
    if (!is_phys_device_native(physicalDevice)) {
        pFeatures->features.fillModeNonSolid = VK_TRUE;
        LOG_OPT_DEBUG("Emulated fillModeNonSolid = VK_TRUE in vkGetPhysicalDeviceFeatures2");
    }
}

void FillModeNonSolidModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // 1. If application enabled fillModeNonSolid in pEnabledFeatures, strip it for real driver
    if (pEnabledFeatures && pEnabledFeatures->fillModeNonSolid) {
        pEnabledFeatures->fillModeNonSolid = VK_FALSE;
        LOGI("vkCreateDevice: stripped fillModeNonSolid from pEnabledFeatures to prevent hardware driver failure");
    }

    // 2. If application chained VkPhysicalDeviceFeatures2 in pCreateInfo->pNext, strip it as well
    void* curr = (void*) pCreateInfo->pNext;
    while (curr != NULL) {
        VkBaseOutStructure* h = (VkBaseOutStructure*) curr;
        if (h->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            VkPhysicalDeviceFeatures2* f2 = (VkPhysicalDeviceFeatures2*) h;
            if (f2->features.fillModeNonSolid) {
                f2->features.fillModeNonSolid = VK_FALSE;
                LOGI("vkCreateDevice: stripped fillModeNonSolid from VkPhysicalDeviceFeatures2 in pNext");
            }
        }
        curr = (void*) h->pNext;
    }
}

void FillModeNonSolidModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_native_support[(uint64_t)(uintptr_t)device] = native;
        LOGI("Device %p created: fillModeNonSolid %s", device, native ? "NATIVE" : "EMULATED");
    }
}

void FillModeNonSolidModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_native_support.erase((uint64_t)(uintptr_t)device);
}

bool FillModeNonSolidModule::needs_pipeline_interception(
    VkDevice device,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos
) {
    if (is_device_native(device) || !pCreateInfos || createInfoCount == 0) return false;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        const VkPipelineRasterizationStateCreateInfo* r = pCreateInfos[i].pRasterizationState;
        if (r != NULL && r->polygonMode != VK_POLYGON_MODE_FILL) {
            return true;
        }
    }
    return false;
}

void FillModeNonSolidModule::on_modify_pipeline_create_info(
    VkDevice device,
    uint32_t index,
    VkGraphicsPipelineCreateInfo& createInfo,
    VkPipelineVertexInputStateCreateInfo& viState,
    std::vector<void*>& allocationsToFree
) {
    if (is_device_native(device) || createInfo.pRasterizationState == NULL) return;

    if (createInfo.pRasterizationState->polygonMode != VK_POLYGON_MODE_FILL) {
        VkPipelineRasterizationStateCreateInfo* modRaster = (VkPipelineRasterizationStateCreateInfo*)
            malloc(sizeof(VkPipelineRasterizationStateCreateInfo));
        if (modRaster) {
            *modRaster = *createInfo.pRasterizationState;
            LOG_OPT_DEBUG("Pipeline %u: emulating fillModeNonSolid: substituted polygonMode %d with VK_POLYGON_MODE_FILL",
                          index, modRaster->polygonMode);
            modRaster->polygonMode = VK_POLYGON_MODE_FILL;
            createInfo.pRasterizationState = modRaster;
            allocationsToFree.push_back(modRaster);
        }
    }
}

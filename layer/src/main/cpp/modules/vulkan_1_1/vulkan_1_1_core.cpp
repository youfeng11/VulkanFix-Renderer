#include "vulkan_1_1_core.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"
#include <cstring>
#include <algorithm>
#include <inttypes.h>

REGISTER_LAYER_MODULE(Vulkan11CoreModule);

static const char* const s_vulkan_1_1_core_extensions[] = {
    VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
    VK_KHR_RELAXED_BLOCK_LAYOUT_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME,
    VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
    VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
};

Vulkan11CoreModule::Vulkan11CoreModule() {
    LOGI("Vulkan11CoreModule initialized");
}

bool Vulkan11CoreModule::is_phys_device_native(VkPhysicalDevice physDev) {
    if (!physDev) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    uint32_t realApiVer = VK_API_VERSION_1_0;
    PFN_vkGetPhysicalDeviceProperties real_fn =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_fn) {
        VkPhysicalDeviceProperties props{};
        real_fn(physDev, &props);
        realApiVer = props.apiVersion;
    }
    m_phys_real_api_version[(uint64_t)(uintptr_t)physDev] = realApiVer;

    bool native = (realApiVer >= VK_API_VERSION_1_1);
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    LOGI("Vulkan11Core: PhysicalDevice %p native Vulkan 1.1 support: %d (api: 0x%x)",
         physDev, native ? 1 : 0, realApiVer);
    return native;
}

uint32_t Vulkan11CoreModule::get_phys_real_api_version(VkPhysicalDevice physDev) {
    if (!physDev) return VK_API_VERSION_1_0;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_real_api_version.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_real_api_version.end()) {
        return it->second;
    }
    PFN_vkGetPhysicalDeviceProperties real_props =
        (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (real_props) {
        VkPhysicalDeviceProperties props{};
        real_props(physDev, &props);
        m_phys_real_api_version[(uint64_t)(uintptr_t)physDev] = props.apiVersion;
        return props.apiVersion;
    }
    return VK_API_VERSION_1_0;
}

bool Vulkan11CoreModule::is_device_native(VkDevice device) {
    if (!device) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_needs_emulation.find((uint64_t)(uintptr_t)device);
    if (it != m_device_needs_emulation.end()) {
        return !it->second;
    }
    return false;
}

void Vulkan11CoreModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    if (is_phys_device_native(physicalDevice)) return;

    for (const char* extName : s_vulkan_1_1_core_extensions) {
        if (!vku::has_extension(extensions, extName)) {
            VkExtensionProperties prop{};
            memset(&prop, 0, sizeof(prop));
            strncpy(prop.extensionName, extName, VK_MAX_EXTENSION_NAME_SIZE - 1);
            prop.specVersion = 1;
            extensions.push_back(prop);
            LOG_OPT_DEBUG("Vulkan11Core: injected extension %s", extName);
        }
    }
}

void Vulkan11CoreModule::on_get_properties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties
) {
    if (!pProperties) return;
    if (claim_vulkan_1_2() && pProperties->apiVersion < VK_API_VERSION_1_1) {
        pProperties->apiVersion = VK_API_VERSION_1_1;
        LOG_OPT_DEBUG("Vulkan11Core: promoted device apiVersion to 1.1");
    }
    (void)physicalDevice;
}

// Native properties and aggregate conversion are owned by PromotedPropertyQuery.
void Vulkan11CoreModule::on_pre_get_properties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*, void*& data) { data = nullptr; }
void Vulkan11CoreModule::on_post_get_properties2(VkPhysicalDevice, VkPhysicalDeviceProperties2* properties, void*) {
    if (properties && claim_vulkan_1_2() && properties->properties.apiVersion < VK_API_VERSION_1_1)
        properties->properties.apiVersion = VK_API_VERSION_1_1;
}

// Aggregate feature queries and device-input ownership are centralized in
// PromotedFeatureQuery / PromotedDeviceFeatures. Optional bits come from the
// actual individual feature structures, never from an extension name.
void Vulkan11CoreModule::on_get_features(VkPhysicalDevice, VkPhysicalDeviceFeatures*) {}
void Vulkan11CoreModule::on_pre_get_features2(VkPhysicalDevice, VkPhysicalDeviceFeatures2*, void*& data) { data = nullptr; }
void Vulkan11CoreModule::on_post_get_features2(VkPhysicalDevice, VkPhysicalDeviceFeatures2*, void*) {}

void Vulkan11CoreModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // Filter out core 1.1 extensions if not supported natively
    std::vector<VkExtensionProperties> nativeExts;
    PFN_vkEnumerateDeviceExtensionProperties real_enum =
        (PFN_vkEnumerateDeviceExtensionProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    if (real_enum) {
        uint32_t count = 0;
        real_enum(physicalDevice, nullptr, &count, nullptr);
        if (count > 0) {
            nativeExts.resize(count);
            real_enum(physicalDevice, nullptr, &count, nativeExts.data());
        }
    }

    for (auto it = enabledExtensions.begin(); it != enabledExtensions.end();) {
        const char* name = *it;
        bool isPromoted11 = false;
        for (const char* ext : s_vulkan_1_1_core_extensions) {
            if (strcmp(name, ext) == 0) {
                isPromoted11 = true;
                break;
            }
        }
        if (isPromoted11 && !vku::has_extension(nativeExts, name)) {
            LOGI("Vulkan11Core: stripped emulated extension %s from vkCreateDevice", name);
            it = enabledExtensions.erase(it);
        } else {
            ++it;
        }
    }

    // Native extension feature structures are already expanded and validated.
    // Do not strip them: doing so disables the feature the application enabled.

}

void Vulkan11CoreModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_needs_emulation[(uint64_t)(uintptr_t)device] = !native;
        LOGI("Device %p created: Vulkan 1.1 core native=%d", device, native);
    }


}

void Vulkan11CoreModule::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_needs_emulation.erase((uint64_t)(uintptr_t)device);
}

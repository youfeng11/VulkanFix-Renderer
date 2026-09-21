#ifndef EXTENSION_MODULE_BASE_H
#define EXTENSION_MODULE_BASE_H

#include "layer_module.h"
#include "driver_loader.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstring>

/**
 * Base class for modules that emulate or supplement Vulkan device extensions.
 * Handles automatic native capability detection, extension enumeration injection,
 * and enabledExtensions filtering during vkCreateDevice.
 */
class ExtensionModuleBase : public IVulkanLayerModule {
public:
    explicit ExtensionModuleBase(const char* extensionName, uint32_t specVersion = 1)
        : m_extension_name(extensionName), m_spec_version(specVersion) {}

    virtual ~ExtensionModuleBase() = default;

    const char* get_name() const override { return m_extension_name.c_str(); }

    const char* get_extension_name() const { return m_extension_name.c_str(); }
    uint32_t get_spec_version() const { return m_spec_version; }

    bool is_phys_device_native(VkPhysicalDevice physDev) {
        std::lock_guard<std::mutex> lock(m_ext_mutex);
        auto it = m_phys_native.find((uint64_t)(uintptr_t)physDev);
        if (it != m_phys_native.end()) {
            return it->second;
        }

        bool native = query_native_support(physDev);
        m_phys_native[(uint64_t)(uintptr_t)physDev] = native;
        if (!native) {
            LOGI("[%s] Device %p lacks native support, enabling emulation!", m_extension_name.c_str(), physDev);
        } else {
            LOGI("[%s] Device %p natively supports extension", m_extension_name.c_str(), physDev);
        }
        return native;
    }

    bool is_device_native(VkDevice device) {
        std::lock_guard<std::mutex> lock(m_ext_mutex);
        auto it = m_device_native.find((uint64_t)(uintptr_t)device);
        if (it != m_device_native.end()) {
            return it->second;
        }
        return false;
    }

    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions
    ) override {
        if (is_phys_device_native(physicalDevice)) return;

        for (const auto& ext : extensions) {
            if (strcmp(ext.extensionName, m_extension_name.c_str()) == 0) {
                return;
            }
        }

        VkExtensionProperties prop{};
        strncpy(prop.extensionName, m_extension_name.c_str(), VK_MAX_EXTENSION_NAME_SIZE - 1);
        prop.specVersion = m_spec_version;
        extensions.push_back(prop);
        LOGI("[%s] Injected extension into enumeration (spec version %u)", m_extension_name.c_str(), prop.specVersion);
    }

    void on_pre_create_device(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData
    ) override {
        pUserData = nullptr;
        if (is_phys_device_native(physicalDevice)) return;

        // Auto-strip extension from enabledExtensions for the native driver
        for (auto it = enabledExtensions.begin(); it != enabledExtensions.end(); ) {
            if (strcmp(*it, m_extension_name.c_str()) == 0) {
                it = enabledExtensions.erase(it);
                LOGI("[%s] vkCreateDevice: safely stripped from enabledExtensions", m_extension_name.c_str());
            } else {
                ++it;
            }
        }

        on_pre_create_device_custom(physicalDevice, pCreateInfo, pEnabledFeatures, enabledExtensions, pUserData);
    }

    void on_post_create_device(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkResult result,
        void* pUserData
    ) override {
        if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
            bool native = is_phys_device_native(physicalDevice);
            std::lock_guard<std::mutex> lock(m_ext_mutex);
            m_device_native[(uint64_t)(uintptr_t)device] = native;
        }
    }

    void on_destroy_device(VkDevice device) override {
        std::lock_guard<std::mutex> lock(m_ext_mutex);
        m_device_native.erase((uint64_t)(uintptr_t)device);
    }

protected:
    virtual bool query_native_support(VkPhysicalDevice physDev) {
        PFN_vkEnumerateDeviceExtensionProperties real_fn =
            (PFN_vkEnumerateDeviceExtensionProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
        if (!real_fn) return false;

        uint32_t count = 0;
        if (real_fn(physDev, NULL, &count, NULL) == VK_SUCCESS && count > 0) {
            std::vector<VkExtensionProperties> exts(count);
            if (real_fn(physDev, NULL, &count, exts.data()) == VK_SUCCESS) {
                for (const auto& e : exts) {
                    if (strcmp(e.extensionName, m_extension_name.c_str()) == 0) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    virtual void on_pre_create_device_custom(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData) {}

    std::string m_extension_name;
    uint32_t m_spec_version;

    std::mutex m_ext_mutex;
    std::unordered_map<uint64_t, bool> m_phys_native;
    std::unordered_map<uint64_t, bool> m_device_native;
};

#endif // EXTENSION_MODULE_BASE_H

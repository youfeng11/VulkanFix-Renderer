#ifndef HOST_QUERY_RESET_H
#define HOST_QUERY_RESET_H

#include "extension_module_base.h"

class HostQueryResetModule : public ExtensionModuleBase {
public:
    HostQueryResetModule();
    ~HostQueryResetModule() override = default;

    void on_pre_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void*& pUserData) override;

    void on_post_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void* pUserData) override;

    bool on_reset_query_pool(
        VkDevice device,
        VkQueryPool queryPool,
        uint32_t firstQuery,
        uint32_t queryCount) override;

protected:
    void on_pre_create_device_custom(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData) override;
};

#endif // HOST_QUERY_RESET_H

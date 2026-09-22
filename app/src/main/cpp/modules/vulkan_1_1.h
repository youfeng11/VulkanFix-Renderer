#ifndef VULKAN_1_1_H
#define VULKAN_1_1_H

#include "layer_module.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>

class Vulkan11Module : public IVulkanLayerModule {
public:
    Vulkan11Module();
    ~Vulkan11Module() override = default;

    const char* get_name() const override { return "Vulkan11Emulation"; }

    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    void on_get_properties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties* pProperties) override;

    void on_pre_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void*& pUserData) override;

    void on_post_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void* pUserData) override;

    void on_get_features(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures* pFeatures) override;

    void on_pre_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void*& pUserData) override;

    void on_post_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void* pUserData) override;

    void on_pre_create_device(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData) override;

    void on_post_create_device(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkResult result,
        void* pUserData) override;

    void on_destroy_device(VkDevice device) override;

    // Memory & Binding 2
    bool on_bind_buffer_memory2(
        VkDevice device,
        uint32_t bindInfoCount,
        const VkBindBufferMemoryInfo* pBindInfos,
        VkResult& outResult) override;

    bool on_bind_image_memory2(
        VkDevice device,
        uint32_t bindInfoCount,
        const VkBindImageMemoryInfo* pBindInfos,
        VkResult& outResult) override;

    bool on_get_buffer_memory_requirements2(
        VkDevice device,
        const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) override;

    bool on_get_image_memory_requirements2(
        VkDevice device,
        const VkImageMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) override;

    bool on_get_image_sparse_memory_requirements2(
        VkDevice device,
        const VkImageSparseMemoryRequirementsInfo2* pInfo,
        uint32_t* pSparseMemoryRequirementCount,
        VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) override;

    // Descriptor Update Template
    bool on_create_descriptor_update_template(
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate,
        VkResult& outResult) override;

    bool on_destroy_descriptor_update_template(
        VkDevice device,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const VkAllocationCallbacks* pAllocator) override;

    bool on_update_descriptor_set_with_template(
        VkDevice device,
        VkDescriptorSet descriptorSet,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const void* pData) override;

    // Maintenance 1 & 3
    void on_trim_command_pool(
        VkDevice device,
        VkCommandPool commandPool,
        VkCommandPoolTrimFlags flags) override;

    bool on_get_descriptor_set_layout_support(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        VkDescriptorSetLayoutSupport* pSupport) override;

    // Device Groups / Dispatch Base
    void on_cmd_set_device_mask(
        VkCommandBuffer commandBuffer,
        uint32_t deviceMask) override;

    bool on_get_device_group_peer_memory_features(
        VkDevice device,
        uint32_t heapIndex,
        uint32_t localDeviceIndex,
        uint32_t remoteDeviceIndex,
        VkPeerMemoryFeatureFlags* pPeerMemoryFeatures) override;

    bool on_cmd_dispatch_base(
        VkCommandBuffer commandBuffer,
        uint32_t baseGroupX,
        uint32_t baseGroupY,
        uint32_t baseGroupZ,
        uint32_t groupCountX,
        uint32_t groupCountY,
        uint32_t groupCountZ) override;

    bool on_enumerate_physical_device_groups(
        VkInstance instance,
        uint32_t* pPhysicalDeviceGroupCount,
        VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties,
        VkResult& outResult) override;

    bool is_phys_device_native(VkPhysicalDevice physDev);
    bool is_device_native(VkDevice device);
    uint32_t get_phys_real_api_version(VkPhysicalDevice physDev);

private:
    std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, uint32_t> m_phys_real_api_version;
    std::unordered_map<uint64_t, bool> m_device_needs_emulation;

    struct EmulatedTemplate {
        VkDescriptorUpdateTemplateType templateType;
        std::vector<VkDescriptorUpdateTemplateEntry> entries;
        VkPipelineLayout pipelineLayout;
        uint32_t set;
    };
    std::mutex m_template_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<EmulatedTemplate>> m_templates;
};

#endif // VULKAN_1_1_H

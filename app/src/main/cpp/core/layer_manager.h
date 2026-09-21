#ifndef LAYER_MANAGER_H
#define LAYER_MANAGER_H

#include "layer_module.h"
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_set>

class LayerManager {
public:
    static LayerManager& get();

    void register_module(std::unique_ptr<IVulkanLayerModule> module);

    void add_emulated_device(VkDevice device);
    void remove_emulated_device(VkDevice device);

    inline bool is_emulated_device(VkDevice device) {
        if (__builtin_expect(device == m_primary_emulated_device.load(std::memory_order_relaxed), 1)) {
            return true;
        }
        std::lock_guard<std::mutex> lock(m_state_mutex);
        return m_emulated_devices.find((uint64_t)(uintptr_t)device) != m_emulated_devices.end();
    }

    // Intercepted dispatchers
    VkResult dispatch_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        const char* pLayerName,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties);

    void dispatch_get_physical_device_features(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures* pFeatures);

    void dispatch_get_physical_device_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures);

    void dispatch_get_physical_device_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties);

    VkResult dispatch_create_device(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice);

    void dispatch_destroy_device(
        VkDevice device,
        const VkAllocationCallbacks* pAllocator);

    VkResult dispatch_create_graphics_pipelines(
        VkDevice device,
        VkPipelineCache pipelineCache,
        uint32_t createInfoCount,
        const VkGraphicsPipelineCreateInfo* pCreateInfos,
        const VkAllocationCallbacks* pAllocator,
        VkPipeline* pPipelines);

    inline VkDevice get_primary_device() const {
        return m_primary_emulated_device.load(std::memory_order_relaxed);
    }

    VkResult dispatch_create_descriptor_set_layout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorSetLayout* pSetLayout);

    void dispatch_destroy_descriptor_set_layout(
        VkDevice device,
        VkDescriptorSetLayout descriptorSetLayout,
        const VkAllocationCallbacks* pAllocator);

    VkResult dispatch_create_pipeline_layout(
        VkDevice device,
        const VkPipelineLayoutCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkPipelineLayout* pPipelineLayout);

    void dispatch_destroy_pipeline_layout(
        VkDevice device,
        VkPipelineLayout pipelineLayout,
        const VkAllocationCallbacks* pAllocator);

    VkResult dispatch_allocate_command_buffers(
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkCommandBuffer* pCommandBuffers);

    void dispatch_free_command_buffers(
        VkDevice device,
        VkCommandPool commandPool,
        uint32_t commandBufferCount,
        const VkCommandBuffer* pCommandBuffers);

    VkResult dispatch_begin_command_buffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo);

    VkResult dispatch_reset_command_buffer(
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags);

    VkResult dispatch_create_descriptor_update_template(
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate);

    void dispatch_destroy_descriptor_update_template(
        VkDevice device,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const VkAllocationCallbacks* pAllocator);

    void dispatch_cmd_push_descriptor_set(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipelineLayout layout,
        uint32_t set,
        uint32_t descriptorWriteCount,
        const VkWriteDescriptorSet* pDescriptorWrites);

    void dispatch_cmd_push_descriptor_set_with_template(
        VkCommandBuffer commandBuffer,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        VkPipelineLayout layout,
        uint32_t set,
        const void* pData);

    VkResult dispatch_create_image(
        VkDevice device,
        const VkImageCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkImage* pImage);

    void dispatch_destroy_image(
        VkDevice device,
        VkImage image,
        const VkAllocationCallbacks* pAllocator);

    VkResult dispatch_create_image_view(
        VkDevice device,
        const VkImageViewCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkImageView* pView);

    void dispatch_destroy_image_view(
        VkDevice device,
        VkImageView imageView,
        const VkAllocationCallbacks* pAllocator);

    void dispatch_cmd_begin_rendering(
        VkCommandBuffer commandBuffer,
        const VkRenderingInfo* pRenderingInfo);

    void dispatch_cmd_end_rendering(
        VkCommandBuffer commandBuffer);

private:
    LayerManager() = default;
    ~LayerManager() = default;
    LayerManager(const LayerManager&) = delete;
    LayerManager& operator=(const LayerManager&) = delete;

    std::vector<std::unique_ptr<IVulkanLayerModule>> m_modules;
    std::mutex m_modules_mutex;

    std::mutex m_state_mutex;
    std::unordered_set<uint64_t> m_emulated_devices;
    std::atomic<VkDevice> m_primary_emulated_device{VK_NULL_HANDLE};
};

#endif // LAYER_MANAGER_H

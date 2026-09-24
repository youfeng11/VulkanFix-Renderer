#ifndef LAYER_MODULE_H
#define LAYER_MODULE_H

#include "vk_common.h"
#include <vector>

/**
 * Interface IVulkanLayerModule
 * Represents a modular Vulkan extension emulation, feature supplement,
 * or driver compatibility layer.
 */
class IVulkanLayerModule {
public:
    virtual ~IVulkanLayerModule() = default;

    // Unique module name
    virtual const char* get_name() const = 0;

    // Check if module is active
    virtual bool is_enabled() const { return true; }

    // 1. Device Extension Enumeration:
    virtual void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) {}

    // 2. Physical Device Features Query:
    // Core Vulkan 1.0 features
    virtual void on_get_features(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures* pFeatures) {}

    // Vulkan 1.1+ features query
    virtual void on_pre_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void*& pUserData) {}

    virtual void on_post_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void* pUserData) {}

    // 3. Physical Device Properties Query:
    // Core Vulkan 1.0 properties query
    virtual void on_get_properties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties* pProperties) {}

    virtual void on_pre_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void*& pUserData) {}

    virtual void on_post_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void* pUserData) {}

    // 4. Logical Device Creation:
    virtual void on_pre_create_device(
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* pCreateInfo,
        VkPhysicalDeviceFeatures* pEnabledFeatures,
        std::vector<const char*>& enabledExtensions,
        void*& pUserData) {}

    virtual void on_post_create_device(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkResult result,
        void* pUserData) {}

    // 5. Logical Device Destruction:
    virtual void on_destroy_device(VkDevice device) {}

    // 6. Graphics Pipeline Creation:
    // Fast check: returns true if this module needs to inspect/modify any pipeline in this batch.
    virtual bool needs_pipeline_interception(
        VkDevice device,
        uint32_t createInfoCount,
        const VkGraphicsPipelineCreateInfo* pCreateInfos) {
        return false;
    }

    // Called for each pipeline in the batch to apply necessary state modifications.
    virtual void on_modify_pipeline_create_info(
        VkDevice device,
        uint32_t index,
        VkGraphicsPipelineCreateInfo& createInfo,
        VkPipelineVertexInputStateCreateInfo& viState,
        std::vector<void*>& allocationsToFree) {}

    virtual void on_post_create_graphics_pipelines(
        VkDevice device,
        uint32_t count,
        const VkGraphicsPipelineCreateInfo* pCreateInfos,
        const VkPipeline* pPipelines) {}

    virtual void on_destroy_pipeline(
        VkDevice device,
        VkPipeline pipeline) {}

    // 7. Descriptor Set Layout:
    virtual void on_pre_create_descriptor_set_layout(
        VkDevice device,
        VkDescriptorSetLayoutCreateInfo& createInfo) {}

    virtual void on_post_create_descriptor_set_layout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        VkResult result,
        VkDescriptorSetLayout setLayout) {}

    virtual void on_destroy_descriptor_set_layout(
        VkDevice device,
        VkDescriptorSetLayout setLayout) {}

    // 8. Pipeline Layout:
    virtual void on_post_create_pipeline_layout(
        VkDevice device,
        const VkPipelineLayoutCreateInfo* pCreateInfo,
        VkResult result,
        VkPipelineLayout pipelineLayout) {}

    virtual void on_destroy_pipeline_layout(
        VkDevice device,
        VkPipelineLayout pipelineLayout) {}

    // 9. Command Buffers:
    virtual void on_post_allocate_command_buffers(
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkResult result,
        VkCommandBuffer* pCommandBuffers) {}

    virtual void on_free_command_buffers(
        VkDevice device,
        uint32_t count,
        const VkCommandBuffer* pCommandBuffers) {}

    virtual void on_begin_command_buffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo) {}

    virtual void on_reset_command_buffer(
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags) {}

    virtual void on_pre_begin_command_buffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo,
        VkCommandBufferBeginInfo& modBeginInfo,
        VkCommandBufferInheritanceInfo& modInheritanceInfo,
        bool& modifiedInheritance) {}

    virtual void on_cmd_bind_pipeline(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipeline pipeline) {}

    virtual void on_cmd_bind_vertex_buffers(
        VkCommandBuffer commandBuffer,
        uint32_t firstBinding,
        uint32_t bindingCount,
        const VkBuffer* pBuffers,
        const VkDeviceSize* pOffsets) {}

    virtual bool on_cmd_draw(
        VkCommandBuffer commandBuffer,
        uint32_t vertexCount,
        uint32_t instanceCount,
        uint32_t firstVertex,
        uint32_t firstInstance) { return false; }

    virtual bool on_cmd_draw_indexed(
        VkCommandBuffer commandBuffer,
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex,
        int32_t vertexOffset,
        uint32_t firstInstance) { return false; }

    virtual bool on_cmd_draw_indirect(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        uint32_t drawCount,
        uint32_t stride) { return false; }

    virtual bool on_cmd_draw_indexed_indirect(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        uint32_t drawCount,
        uint32_t stride) { return false; }

    // 10. Descriptor Update Template:
    virtual void on_pre_create_descriptor_update_template(
        VkDevice device,
        VkDescriptorUpdateTemplateCreateInfo& createInfo) {}

    virtual bool on_create_descriptor_update_template(
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate,
        VkResult& outResult) { return false; }

    virtual bool on_destroy_descriptor_update_template(
        VkDevice device,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const VkAllocationCallbacks* pAllocator) { return false; }

    virtual bool on_update_descriptor_set_with_template(
        VkDevice device,
        VkDescriptorSet descriptorSet,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const void* pData) { return false; }

    // 11. Push Descriptors:
    virtual bool on_cmd_push_descriptor_set(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipelineLayout layout,
        uint32_t set,
        uint32_t descriptorWriteCount,
        const VkWriteDescriptorSet* pDescriptorWrites) { return false; }

    virtual bool on_cmd_push_descriptor_set_with_template(
        VkCommandBuffer commandBuffer,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        VkPipelineLayout layout,
        uint32_t set,
        const void* pData) { return false; }

    // 12. Images and Image Views:
    virtual void on_post_create_image(
        VkDevice device,
        const VkImageCreateInfo* pCreateInfo,
        VkResult result,
        VkImage image) {}

    virtual void on_destroy_image(
        VkDevice device,
        VkImage image) {}

    virtual void on_post_create_image_view(
        VkDevice device,
        const VkImageViewCreateInfo* pCreateInfo,
        VkResult result,
        VkImageView imageView) {}

    virtual void on_destroy_image_view(
        VkDevice device,
        VkImageView imageView) {}

    // 12.1. Samplers:
    virtual void on_pre_create_sampler(
        VkDevice device,
        VkSamplerCreateInfo& createInfo) {}

    virtual void on_post_create_sampler(
        VkDevice device,
        const VkSamplerCreateInfo* pCreateInfo,
        VkResult result,
        VkSampler sampler) {}

    virtual void on_destroy_sampler(
        VkDevice device,
        VkSampler sampler) {}

    // 13. Dynamic Rendering:
    virtual bool on_cmd_begin_rendering(
        VkCommandBuffer commandBuffer,
        const VkRenderingInfo* pRenderingInfo) { return false; }

    virtual bool on_cmd_end_rendering(
        VkCommandBuffer commandBuffer) { return false; }

    // 14. Synchronization2:
    virtual bool on_cmd_set_event2(
        VkCommandBuffer commandBuffer,
        VkEvent event,
        const VkDependencyInfo* pDependencyInfo) { return false; }

    virtual bool on_cmd_reset_event2(
        VkCommandBuffer commandBuffer,
        VkEvent event,
        VkPipelineStageFlags2 stageMask) { return false; }

    virtual bool on_cmd_wait_events2(
        VkCommandBuffer commandBuffer,
        uint32_t eventCount,
        const VkEvent* pEvents,
        const VkDependencyInfo* pDependencyInfos) { return false; }

    virtual bool on_cmd_pipeline_barrier2(
        VkCommandBuffer commandBuffer,
        const VkDependencyInfo* pDependencyInfo) { return false; }

    virtual bool on_cmd_write_timestamp2(
        VkCommandBuffer commandBuffer,
        VkPipelineStageFlags2 stage,
        VkQueryPool queryPool,
        uint32_t query) { return false; }

    virtual bool on_queue_submit2(
        VkQueue queue,
        uint32_t submitCount,
        const VkSubmitInfo2* pSubmits,
        VkFence fence,
        VkResult& outResult) { return false; }

    virtual bool on_queue_submit(
        VkQueue queue,
        uint32_t submitCount,
        const VkSubmitInfo* pSubmits,
        VkFence fence,
        VkResult& outResult) { return false; }

    // 15. Semaphore & Timeline Semaphore:
    virtual void on_pre_create_semaphore(
        VkDevice device,
        VkSemaphoreCreateInfo& createInfo,
        void*& pUserData) {}

    virtual void on_post_create_semaphore(
        VkDevice device,
        const VkSemaphoreCreateInfo* pCreateInfo,
        VkResult result,
        VkSemaphore semaphore,
        void* pUserData) {}

    virtual void on_destroy_semaphore(
        VkDevice device,
        VkSemaphore semaphore) {}

    virtual bool on_get_semaphore_counter_value(
        VkDevice device,
        VkSemaphore semaphore,
        uint64_t* pValue,
        VkResult& outResult) { return false; }

    virtual bool on_wait_semaphores(
        VkDevice device,
        const VkSemaphoreWaitInfo* pWaitInfo,
        uint64_t timeout,
        VkResult& outResult) { return false; }

    virtual bool on_signal_semaphore(
        VkDevice device,
        const VkSemaphoreSignalInfo* pSignalInfo,
        VkResult& outResult) { return false; }

    virtual void on_queue_wait_idle(VkQueue queue) {}
    virtual void on_device_wait_idle(VkDevice device) {}
    virtual bool is_timeline_semaphore(VkSemaphore semaphore) { return false; }

    // 16. Host Query Reset:
    virtual bool on_reset_query_pool(
        VkDevice device,
        VkQueryPool queryPool,
        uint32_t firstQuery,
        uint32_t queryCount) { return false; }

    // 17. RenderPass2:
    virtual bool on_create_render_pass2(
        VkDevice device,
        const VkRenderPassCreateInfo2* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkRenderPass* pRenderPass,
        VkResult& outResult) { return false; }

    virtual bool on_cmd_begin_render_pass2(
        VkCommandBuffer commandBuffer,
        const VkRenderPassBeginInfo* pRenderPassBegin,
        const VkSubpassBeginInfo* pSubpassBeginInfo) { return false; }

    virtual bool on_cmd_next_subpass2(
        VkCommandBuffer commandBuffer,
        const VkSubpassBeginInfo* pSubpassBeginInfo,
        const VkSubpassEndInfo* pSubpassEndInfo) { return false; }

    virtual bool on_cmd_end_render_pass2(
        VkCommandBuffer commandBuffer,
        const VkSubpassEndInfo* pSubpassEndInfo) { return false; }

    // 18. Draw Indirect Count:
    virtual bool on_cmd_draw_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride) { return false; }

    virtual bool on_cmd_draw_indexed_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride) { return false; }

    // 19. Buffer Device Address:
    virtual bool on_get_buffer_device_address(
        VkDevice device,
        const VkBufferDeviceAddressInfo* pInfo,
        VkDeviceAddress& outAddress) { return false; }

    // 20. Vulkan 1.1 Core / Promoted Features:
    virtual bool on_bind_buffer_memory2(
        VkDevice device,
        uint32_t bindInfoCount,
        const VkBindBufferMemoryInfo* pBindInfos,
        VkResult& outResult) { return false; }

    virtual bool on_bind_image_memory2(
        VkDevice device,
        uint32_t bindInfoCount,
        const VkBindImageMemoryInfo* pBindInfos,
        VkResult& outResult) { return false; }

    virtual bool on_get_buffer_memory_requirements2(
        VkDevice device,
        const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) { return false; }

    virtual bool on_get_image_memory_requirements2(
        VkDevice device,
        const VkImageMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) { return false; }

    virtual bool on_get_image_sparse_memory_requirements2(
        VkDevice device,
        const VkImageSparseMemoryRequirementsInfo2* pInfo,
        uint32_t* pSparseMemoryRequirementCount,
        VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) { return false; }

    virtual bool on_get_descriptor_set_layout_support(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        VkDescriptorSetLayoutSupport* pSupport) { return false; }

    virtual bool on_cmd_dispatch_base(
        VkCommandBuffer commandBuffer,
        uint32_t baseGroupX,
        uint32_t baseGroupY,
        uint32_t baseGroupZ,
        uint32_t groupCountX,
        uint32_t groupCountY,
        uint32_t groupCountZ) { return false; }

    virtual bool on_enumerate_physical_device_groups(
        VkInstance instance,
        uint32_t* pPhysicalDeviceGroupCount,
        VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties,
        VkResult& outResult) { return false; }

    virtual void on_trim_command_pool(
        VkDevice device,
        VkCommandPool commandPool,
        VkCommandPoolTrimFlags flags) {}

    virtual void on_cmd_set_device_mask(
        VkCommandBuffer commandBuffer,
        uint32_t deviceMask) {}

    virtual bool on_get_device_group_peer_memory_features(
        VkDevice device,
        uint32_t heapIndex,
        uint32_t localDeviceIndex,
        uint32_t remoteDeviceIndex,
        VkPeerMemoryFeatureFlags* pPeerMemoryFeatures) { return false; }

    // 21. Swapchain and Surface (VK_KHR_swapchain, VK_KHR_surface, VK_KHR_android_surface):
    virtual bool on_create_swapchain(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSwapchainKHR* pSwapchain,
        VkResult& outResult) { return false; }

    virtual bool on_destroy_swapchain(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* pAllocator) { return false; }

    virtual bool on_get_swapchain_images(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint32_t* pSwapchainImageCount,
        VkImage* pSwapchainImages,
        VkResult& outResult) { return false; }

    virtual bool on_acquire_next_image(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint64_t timeout,
        VkSemaphore semaphore,
        VkFence fence,
        uint32_t* pImageIndex,
        VkResult& outResult) { return false; }

    virtual bool on_queue_present(
        VkQueue queue,
        const VkPresentInfoKHR* pPresentInfo,
        VkResult& outResult) { return false; }

    virtual bool on_get_physical_device_surface_support(
        VkPhysicalDevice physicalDevice,
        uint32_t queueFamilyIndex,
        VkSurfaceKHR surface,
        VkBool32* pSupported,
        VkResult& outResult) { return false; }

    virtual bool on_get_physical_device_surface_capabilities(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        VkSurfaceCapabilitiesKHR* pSurfaceCapabilities,
        VkResult& outResult) { return false; }

    virtual bool on_get_physical_device_surface_formats(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        uint32_t* pSurfaceFormatCount,
        VkSurfaceFormatKHR* pSurfaceFormats,
        VkResult& outResult) { return false; }

    virtual bool on_get_physical_device_surface_present_modes(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        uint32_t* pPresentModeCount,
        VkPresentModeKHR* pPresentModes,
        VkResult& outResult) { return false; }

    virtual bool on_create_android_surface(
        VkInstance instance,
        const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSurfaceKHR* pSurface,
        VkResult& outResult) { return false; }

    virtual bool on_destroy_surface(
        VkInstance instance,
        VkSurfaceKHR surface,
        const VkAllocationCallbacks* pAllocator) { return false; }
};

#endif // LAYER_MODULE_H

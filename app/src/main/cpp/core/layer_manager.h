#ifndef LAYER_MANAGER_H
#define LAYER_MANAGER_H

#include "layer_module.h"
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_set>

struct DeviceDispatchTable {
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
    PFN_vkCmdDrawIndirect CmdDrawIndirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect CmdDrawIndexedIndirect = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindVertexBuffers2 CmdBindVertexBuffers2 = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCmdBeginRenderingKHR CmdBeginRendering = nullptr;
    PFN_vkCmdEndRenderingKHR CmdEndRendering = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdPipelineBarrier2KHR CmdPipelineBarrier2 = nullptr;
    PFN_vkCmdPushDescriptorSetKHR CmdPushDescriptorSetKHR = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueSubmit2KHR QueueSubmit2 = nullptr;
    PFN_vkCmdSetEvent CmdSetEvent = nullptr;
    PFN_vkCmdResetEvent CmdResetEvent = nullptr;
    PFN_vkCmdWaitEvents CmdWaitEvents = nullptr;
    PFN_vkCmdWriteTimestamp CmdWriteTimestamp = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2 = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdNextSubpass CmdNextSubpass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkFreeDescriptorSets FreeDescriptorSets = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkGetSemaphoreCounterValueKHR GetSemaphoreCounterValue = nullptr;
    PFN_vkWaitSemaphoresKHR WaitSemaphores = nullptr;
    PFN_vkSignalSemaphoreKHR SignalSemaphore = nullptr;
    PFN_vkResetQueryPool ResetQueryPool = nullptr;
    PFN_vkCreateRenderPass2KHR CreateRenderPass2 = nullptr;
    PFN_vkCmdBeginRenderPass2KHR CmdBeginRenderPass2 = nullptr;
    PFN_vkCmdNextSubpass2KHR CmdNextSubpass2 = nullptr;
    PFN_vkCmdEndRenderPass2KHR CmdEndRenderPass2 = nullptr;
    PFN_vkCmdDrawIndirectCountKHR CmdDrawIndirectCount = nullptr;
    PFN_vkCmdDrawIndexedIndirectCountKHR CmdDrawIndexedIndirectCount = nullptr;
    PFN_vkGetBufferDeviceAddressKHR GetBufferDeviceAddress = nullptr;
    PFN_vkBindBufferMemory2 BindBufferMemory2 = nullptr;
    PFN_vkBindImageMemory2 BindImageMemory2 = nullptr;
    PFN_vkGetBufferMemoryRequirements2 GetBufferMemoryRequirements2 = nullptr;
    PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2 = nullptr;
    PFN_vkGetImageSparseMemoryRequirements2 GetImageSparseMemoryRequirements2 = nullptr;
    PFN_vkUpdateDescriptorSetWithTemplate UpdateDescriptorSetWithTemplate = nullptr;
    PFN_vkGetDescriptorSetLayoutSupport GetDescriptorSetLayoutSupport = nullptr;
    PFN_vkCmdDispatchBase CmdDispatchBase = nullptr;
    PFN_vkTrimCommandPool TrimCommandPool = nullptr;
    PFN_vkCmdSetDeviceMask CmdSetDeviceMask = nullptr;
    PFN_vkGetDeviceGroupPeerMemoryFeatures GetDeviceGroupPeerMemoryFeatures = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImages = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImage = nullptr;
    PFN_vkAcquireNextImage2KHR AcquireNextImage2 = nullptr;
    PFN_vkQueuePresentKHR QueuePresent = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkGetFenceStatus GetFenceStatus = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
};

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

    void dispatch_get_physical_device_properties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties* pProperties);

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

    VkResult dispatch_create_sampler(
        VkDevice device,
        const VkSamplerCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSampler* pSampler);

    void dispatch_destroy_sampler(
        VkDevice device,
        VkSampler sampler,
        const VkAllocationCallbacks* pAllocator);

    void dispatch_cmd_begin_rendering(
        VkCommandBuffer commandBuffer,
        const VkRenderingInfo* pRenderingInfo);

    void dispatch_cmd_end_rendering(
        VkCommandBuffer commandBuffer);

    // Module auto-registration
    using ModuleFactory = std::function<std::unique_ptr<IVulkanLayerModule>()>;
    static void register_module_factory(ModuleFactory factory);
    void init_registered_modules();

    void dispatch_destroy_pipeline(
        VkDevice device,
        VkPipeline pipeline,
        const VkAllocationCallbacks* pAllocator);

    void dispatch_cmd_bind_pipeline(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipeline pipeline);

    void dispatch_cmd_bind_vertex_buffers(
        VkCommandBuffer commandBuffer,
        uint32_t firstBinding,
        uint32_t bindingCount,
        const VkBuffer* pBuffers,
        const VkDeviceSize* pOffsets);

    void dispatch_cmd_bind_vertex_buffers2(
        VkCommandBuffer commandBuffer,
        uint32_t firstBinding,
        uint32_t bindingCount,
        const VkBuffer* pBuffers,
        const VkDeviceSize* pOffsets,
        const VkDeviceSize* pSizes,
        const VkDeviceSize* pStrides);

    void dispatch_cmd_draw(
        VkCommandBuffer commandBuffer,
        uint32_t vertexCount,
        uint32_t instanceCount,
        uint32_t firstVertex,
        uint32_t firstInstance);

    void dispatch_cmd_draw_indexed(
        VkCommandBuffer commandBuffer,
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex,
        int32_t vertexOffset,
        uint32_t firstInstance);

    void dispatch_cmd_draw_indirect(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        uint32_t drawCount,
        uint32_t stride);

    void dispatch_cmd_draw_indexed_indirect(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        uint32_t drawCount,
        uint32_t stride);

    inline VkDevice get_device_for_cmd(VkCommandBuffer cmd) {
        if (__builtin_expect(m_device_count.load(std::memory_order_relaxed) <= 1, 1)) {
            return m_primary_device.load(std::memory_order_relaxed);
        }
        return get_device_for_cmd_slow(cmd);
    }

    inline VkDevice get_device_for_queue(VkQueue queue) {
        if (__builtin_expect(m_device_count.load(std::memory_order_relaxed) <= 1, 1)) {
            return m_primary_device.load(std::memory_order_relaxed);
        }
        return get_device_for_queue_slow(queue);
    }

    inline const DeviceDispatchTable& get_dispatch_table(VkDevice device) {
        if (__builtin_expect(device == m_primary_device.load(std::memory_order_relaxed) && m_has_primary_table.load(std::memory_order_relaxed), 1)) {
            return m_primary_table;
        }
        return get_dispatch_table_slow(device);
    }

    void init_device_dispatch_table(VkDevice device);
    void remove_device_dispatch_table(VkDevice device);

    VkDevice get_device_for_cmd_slow(VkCommandBuffer cmd);
    VkDevice get_device_for_queue_slow(VkQueue queue);
    const DeviceDispatchTable& get_dispatch_table_slow(VkDevice device);

    void dispatch_get_device_queue(
        VkDevice device,
        uint32_t queueFamilyIndex,
        uint32_t queueIndex,
        VkQueue* pQueue);

    void dispatch_get_device_queue2(
        VkDevice device,
        const VkDeviceQueueInfo2* pQueueInfo,
        VkQueue* pQueue);

    bool get_device_queue_info(VkDevice device, VkQueue& outQueue, uint32_t& outQueueFamily);

    void dispatch_cmd_set_event2(
        VkCommandBuffer commandBuffer,
        VkEvent event,
        const VkDependencyInfo* pDependencyInfo);

    void dispatch_cmd_reset_event2(
        VkCommandBuffer commandBuffer,
        VkEvent event,
        VkPipelineStageFlags2 stageMask);

    void dispatch_cmd_wait_events2(
        VkCommandBuffer commandBuffer,
        uint32_t eventCount,
        const VkEvent* pEvents,
        const VkDependencyInfo* pDependencyInfos);

    void dispatch_cmd_pipeline_barrier2(
        VkCommandBuffer commandBuffer,
        const VkDependencyInfo* pDependencyInfo);

    void dispatch_cmd_write_timestamp2(
        VkCommandBuffer commandBuffer,
        VkPipelineStageFlags2 stage,
        VkQueryPool queryPool,
        uint32_t query);

    VkResult dispatch_queue_submit2(
        VkQueue queue,
        uint32_t submitCount,
        const VkSubmitInfo2* pSubmits,
        VkFence fence);

    VkResult dispatch_queue_submit(
        VkQueue queue,
        uint32_t submitCount,
        const VkSubmitInfo* pSubmits,
        VkFence fence);

    VkResult dispatch_queue_wait_idle(VkQueue queue);
    VkResult dispatch_device_wait_idle(VkDevice device);
    bool is_timeline_semaphore(VkSemaphore semaphore);

    VkResult dispatch_create_semaphore(
        VkDevice device,
        const VkSemaphoreCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSemaphore* pSemaphore);

    void dispatch_destroy_semaphore(
        VkDevice device,
        VkSemaphore semaphore,
        const VkAllocationCallbacks* pAllocator);

    VkResult dispatch_get_semaphore_counter_value(
        VkDevice device,
        VkSemaphore semaphore,
        uint64_t* pValue);

    VkResult dispatch_wait_semaphores(
        VkDevice device,
        const VkSemaphoreWaitInfo* pWaitInfo,
        uint64_t timeout);

    VkResult dispatch_signal_semaphore(
        VkDevice device,
        const VkSemaphoreSignalInfo* pSignalInfo);

    void dispatch_reset_query_pool(
        VkDevice device,
        VkQueryPool queryPool,
        uint32_t firstQuery,
        uint32_t queryCount);

    VkResult dispatch_create_render_pass2(
        VkDevice device,
        const VkRenderPassCreateInfo2* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkRenderPass* pRenderPass);

    void dispatch_cmd_begin_render_pass2(
        VkCommandBuffer commandBuffer,
        const VkRenderPassBeginInfo* pRenderPassBegin,
        const VkSubpassBeginInfo* pSubpassBeginInfo);

    void dispatch_cmd_next_subpass2(
        VkCommandBuffer commandBuffer,
        const VkSubpassBeginInfo* pSubpassBeginInfo,
        const VkSubpassEndInfo* pSubpassEndInfo);

    void dispatch_cmd_end_render_pass2(
        VkCommandBuffer commandBuffer,
        const VkSubpassEndInfo* pSubpassEndInfo);

    void dispatch_cmd_draw_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride);

    void dispatch_cmd_draw_indexed_indirect_count(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkBuffer countBuffer,
        VkDeviceSize countBufferOffset,
        uint32_t maxDrawCount,
        uint32_t stride);

    VkDeviceAddress dispatch_get_buffer_device_address(
        VkDevice device,
        const VkBufferDeviceAddressInfo* pInfo);

    uint64_t dispatch_get_buffer_opaque_capture_address(
        VkDevice device,
        const VkBufferDeviceAddressInfo* pInfo);

    uint64_t dispatch_get_device_memory_opaque_capture_address(
        VkDevice device,
        const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo);

    // Swapchain and Surface (VK_KHR_swapchain, VK_KHR_surface, VK_KHR_android_surface)
    VkResult dispatch_create_swapchain(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSwapchainKHR* pSwapchain);

    void dispatch_destroy_swapchain(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* pAllocator);

    VkResult dispatch_get_swapchain_images(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint32_t* pSwapchainImageCount,
        VkImage* pSwapchainImages);

    VkResult dispatch_acquire_next_image(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint64_t timeout,
        VkSemaphore semaphore,
        VkFence fence,
        uint32_t* pImageIndex);

    VkResult dispatch_acquire_next_image2(
        VkDevice device,
        const VkAcquireNextImageInfoKHR* pAcquireInfo,
        uint32_t* pImageIndex);

    VkResult dispatch_queue_present(
        VkQueue queue,
        const VkPresentInfoKHR* pPresentInfo);

    VkResult dispatch_get_physical_device_surface_support(
        VkPhysicalDevice physicalDevice,
        uint32_t queueFamilyIndex,
        VkSurfaceKHR surface,
        VkBool32* pSupported);

    VkResult dispatch_get_physical_device_surface_capabilities(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        VkSurfaceCapabilitiesKHR* pSurfaceCapabilities);

    VkResult dispatch_get_physical_device_surface_capabilities2(
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
        VkSurfaceCapabilities2KHR* pSurfaceCapabilities);

    VkResult dispatch_get_physical_device_surface_formats(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        uint32_t* pSurfaceFormatCount,
        VkSurfaceFormatKHR* pSurfaceFormats);

    VkResult dispatch_get_physical_device_surface_formats2(
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
        uint32_t* pSurfaceFormatCount,
        VkSurfaceFormat2KHR* pSurfaceFormats);

    VkResult dispatch_get_physical_device_surface_present_modes(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        uint32_t* pPresentModeCount,
        VkPresentModeKHR* pPresentModes);

    VkResult dispatch_create_android_surface(
        VkInstance instance,
        const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSurfaceKHR* pSurface);

    void dispatch_destroy_surface(
        VkInstance instance,
        VkSurfaceKHR surface,
        const VkAllocationCallbacks* pAllocator);

    // Vulkan 1.1 Core / Promoted Features
    VkResult dispatch_bind_buffer_memory2(
        VkDevice device,
        uint32_t bindInfoCount,
        const VkBindBufferMemoryInfo* pBindInfos);

    VkResult dispatch_bind_image_memory2(
        VkDevice device,
        uint32_t bindInfoCount,
        const VkBindImageMemoryInfo* pBindInfos);

    void dispatch_get_buffer_memory_requirements2(
        VkDevice device,
        const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements);

    void dispatch_get_image_memory_requirements2(
        VkDevice device,
        const VkImageMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements);

    void dispatch_get_image_sparse_memory_requirements2(
        VkDevice device,
        const VkImageSparseMemoryRequirementsInfo2* pInfo,
        uint32_t* pSparseMemoryRequirementCount,
        VkSparseImageMemoryRequirements2* pSparseMemoryRequirements);

    void dispatch_update_descriptor_set_with_template(
        VkDevice device,
        VkDescriptorSet descriptorSet,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const void* pData);

    void dispatch_get_descriptor_set_layout_support(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        VkDescriptorSetLayoutSupport* pSupport);

    void dispatch_cmd_dispatch_base(
        VkCommandBuffer commandBuffer,
        uint32_t baseGroupX,
        uint32_t baseGroupY,
        uint32_t baseGroupZ,
        uint32_t groupCountX,
        uint32_t groupCountY,
        uint32_t groupCountZ);

    VkResult dispatch_enumerate_physical_device_groups(
        VkInstance instance,
        uint32_t* pPhysicalDeviceGroupCount,
        VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties);

    void dispatch_trim_command_pool(
        VkDevice device,
        VkCommandPool commandPool,
        VkCommandPoolTrimFlags flags);

    void dispatch_cmd_set_device_mask(
        VkCommandBuffer commandBuffer,
        uint32_t deviceMask);

    void dispatch_get_device_group_peer_memory_features(
        VkDevice device,
        uint32_t heapIndex,
        uint32_t localDeviceIndex,
        uint32_t remoteDeviceIndex,
        VkPeerMemoryFeatureFlags* pPeerMemoryFeatures);

    // Custom procedure address registry (allows modules to dynamically export Vulkan entry points)
    void register_custom_proc(const char* name, PFN_vkVoidFunction proc);
    PFN_vkVoidFunction get_custom_proc(const char* name);

private:
    LayerManager() = default;
    ~LayerManager() = default;
    LayerManager(const LayerManager&) = delete;
    LayerManager& operator=(const LayerManager&) = delete;

    std::vector<std::unique_ptr<IVulkanLayerModule>> m_modules;
    std::recursive_mutex m_modules_mutex;

    std::mutex m_proc_mutex;
    std::unordered_map<std::string, PFN_vkVoidFunction> m_custom_procs;

    std::mutex m_state_mutex;
    std::unordered_set<uint64_t> m_emulated_devices;
    std::atomic<VkDevice> m_primary_emulated_device{VK_NULL_HANDLE};

    std::mutex m_cmd_device_mutex;
    std::unordered_map<uint64_t, VkDevice> m_cmd_devices;
    std::unordered_map<uint64_t, VkDevice> m_queue_devices;
    std::unordered_map<uint64_t, std::pair<VkQueue, uint32_t>> m_device_queues;
    std::atomic<VkDevice> m_last_device{VK_NULL_HANDLE};

    std::atomic<uint32_t> m_device_count{0};
    std::atomic<VkDevice> m_primary_device{VK_NULL_HANDLE};
    DeviceDispatchTable m_primary_table{};
    std::atomic<bool> m_has_primary_table{false};

    std::mutex m_table_mutex;
    std::unordered_map<uint64_t, DeviceDispatchTable> m_device_tables;

    IVulkanLayerModule* m_divisor_mod = nullptr;
    IVulkanLayerModule* m_dyn_rendering_mod = nullptr;
    IVulkanLayerModule* m_sync2_mod = nullptr;
    IVulkanLayerModule* m_push_desc_mod = nullptr;
    IVulkanLayerModule* m_fill_mode_mod = nullptr;
    IVulkanLayerModule* m_multi_draw_indirect_mod = nullptr;
    IVulkanLayerModule* m_draw_indirect_first_instance_mod = nullptr;
    IVulkanLayerModule* m_sampler_anisotropy_mod = nullptr;
    IVulkanLayerModule* m_timeline_mod = nullptr;
    IVulkanLayerModule* m_renderpass2_mod = nullptr;
    IVulkanLayerModule* m_draw_indirect_count_mod = nullptr;
    IVulkanLayerModule* m_device_group_mod = nullptr;
    IVulkanLayerModule* m_swapchain_mod = nullptr;
    IVulkanLayerModule* m_bda_mod = nullptr;
    IVulkanLayerModule* m_host_query_reset_mod = nullptr;
};

/**
 * Macro to automatically register a layer module at startup.
 * Usage in any module source file:
 *   REGISTER_LAYER_MODULE(MyCustomModule);
 */
#define REGISTER_LAYER_MODULE(ModuleClass) \
    namespace { \
        struct AutoRegister_##ModuleClass { \
            AutoRegister_##ModuleClass() { \
                LayerManager::register_module_factory([]() -> std::unique_ptr<IVulkanLayerModule> { \
                    return std::make_unique<ModuleClass>(); \
                }); \
            } \
        } s_auto_register_##ModuleClass; \
    }

#endif // LAYER_MANAGER_H

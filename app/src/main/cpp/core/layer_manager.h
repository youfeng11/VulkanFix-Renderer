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

    VkDevice get_device_for_cmd(VkCommandBuffer cmd);
    VkDevice get_device_for_queue(VkQueue queue);

    void dispatch_get_device_queue(
        VkDevice device,
        uint32_t queueFamilyIndex,
        uint32_t queueIndex,
        VkQueue* pQueue);

    void dispatch_get_device_queue2(
        VkDevice device,
        const VkDeviceQueueInfo2* pQueueInfo,
        VkQueue* pQueue);

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

    // Custom procedure address registry (allows modules to dynamically export Vulkan entry points)
    void register_custom_proc(const char* name, PFN_vkVoidFunction proc);
    PFN_vkVoidFunction get_custom_proc(const char* name);

private:
    LayerManager() = default;
    ~LayerManager() = default;
    LayerManager(const LayerManager&) = delete;
    LayerManager& operator=(const LayerManager&) = delete;

    std::vector<std::unique_ptr<IVulkanLayerModule>> m_modules;
    std::mutex m_modules_mutex;

    std::mutex m_proc_mutex;
    std::unordered_map<std::string, PFN_vkVoidFunction> m_custom_procs;

    std::mutex m_state_mutex;
    std::unordered_set<uint64_t> m_emulated_devices;
    std::atomic<VkDevice> m_primary_emulated_device{VK_NULL_HANDLE};

    std::mutex m_cmd_device_mutex;
    std::unordered_map<uint64_t, VkDevice> m_cmd_devices;
    std::unordered_map<uint64_t, VkDevice> m_queue_devices;
    std::atomic<VkDevice> m_last_device{VK_NULL_HANDLE};
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

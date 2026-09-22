#ifndef VERTEX_ATTRIBUTE_DIVISOR_H
#define VERTEX_ATTRIBUTE_DIVISOR_H

#include "layer_module.h"
#include <unordered_map>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>

#ifndef VK_EXT_vertex_attribute_divisor
#define VK_EXT_vertex_attribute_divisor 1
#define VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION 3
#define VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME "VK_EXT_vertex_attribute_divisor"

typedef struct VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT {
    VkStructureType    sType;
    void*              pNext;
    uint32_t           maxVertexAttribDivisor;
} VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT;

typedef struct VkVertexInputBindingDivisorDescriptionEXT {
    uint32_t    binding;
    uint32_t    divisor;
} VkVertexInputBindingDivisorDescriptionEXT;

typedef struct VkPipelineVertexInputDivisorStateCreateInfoEXT {
    VkStructureType                                     sType;
    const void*                                         pNext;
    uint32_t                                            vertexBindingDivisorCount;
    const VkVertexInputBindingDivisorDescriptionEXT*    pVertexBindingDivisors;
} VkPipelineVertexInputDivisorStateCreateInfoEXT;

typedef struct VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           vertexAttributeInstanceRateDivisor;
    VkBool32           vertexAttributeInstanceRateZeroDivisor;
} VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT;
#endif

#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT ((VkStructureType)1000190000)
#endif
#ifndef VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT
#define VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT ((VkStructureType)1000190001)
#endif
#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT ((VkStructureType)1000190002)
#endif

// KHR aliases
#ifndef VK_KHR_vertex_attribute_divisor
#define VK_KHR_vertex_attribute_divisor 1
#define VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_SPEC_VERSION 1
#define VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME "VK_KHR_vertex_attribute_divisor"
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR ((VkStructureType)1000525000)
#define VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR ((VkStructureType)1000525001)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR ((VkStructureType)1000525002)

typedef VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR;
typedef VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR;
typedef VkPipelineVertexInputDivisorStateCreateInfoEXT VkPipelineVertexInputDivisorStateCreateInfoKHR;
typedef VkVertexInputBindingDivisorDescriptionEXT VkVertexInputBindingDivisorDescriptionKHR;
#endif

#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES ((VkStructureType)54)
#endif
#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES ((VkStructureType)55)
#endif

class VertexAttributeDivisorModule : public IVulkanLayerModule {
public:
    VertexAttributeDivisorModule();
    virtual ~VertexAttributeDivisorModule() = default;

    const char* get_name() const override { return "VK_EXT_vertex_attribute_divisor"; }

    // 1. Extensions
    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    // 2. Features
    void on_pre_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void*& pUserData) override;

    void on_post_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void* pUserData) override;

    // 3. Properties
    void on_pre_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void*& pUserData) override;

    void on_post_get_properties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties,
        void* pUserData) override;

    // 4. Create Device
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

    // 5. Pipelines
    bool needs_pipeline_interception(
        VkDevice device,
        uint32_t createInfoCount,
        const VkGraphicsPipelineCreateInfo* pCreateInfos) override;

    void on_modify_pipeline_create_info(
        VkDevice device,
        uint32_t index,
        VkGraphicsPipelineCreateInfo& createInfo,
        VkPipelineVertexInputStateCreateInfo& viState,
        std::vector<void*>& allocationsToFree) override;

    void on_post_create_graphics_pipelines(
        VkDevice device,
        uint32_t count,
        const VkGraphicsPipelineCreateInfo* pCreateInfos,
        const VkPipeline* pPipelines) override;

    void on_destroy_pipeline(
        VkDevice device,
        VkPipeline pipeline) override;

    // 6. Command buffer lifecycle
    void on_post_allocate_command_buffers(
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkResult result,
        VkCommandBuffer* pCommandBuffers) override;

    void on_free_command_buffers(
        VkDevice device,
        uint32_t count,
        const VkCommandBuffer* pCommandBuffers) override;

    void on_reset_command_buffer(
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags) override;

    // 7. Pipeline and buffer bindings
    void on_cmd_bind_pipeline(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipeline pipeline) override;

    void on_cmd_bind_vertex_buffers(
        VkCommandBuffer commandBuffer,
        uint32_t firstBinding,
        uint32_t bindingCount,
        const VkBuffer* pBuffers,
        const VkDeviceSize* pOffsets) override;

    // 8. Draw call emulation
    bool on_cmd_draw(
        VkCommandBuffer commandBuffer,
        uint32_t vertexCount,
        uint32_t instanceCount,
        uint32_t firstVertex,
        uint32_t firstInstance) override;

    bool on_cmd_draw_indexed(
        VkCommandBuffer commandBuffer,
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex,
        int32_t vertexOffset,
        uint32_t firstInstance) override;

private:
    struct BindingDivisorInfo {
        uint32_t binding = 0;
        uint32_t divisor = 1;
        uint32_t original_stride = 0;
    };

    struct PipelineDivisorInfo {
        std::vector<BindingDivisorInfo> divisors;
        bool has_divisor_greater_than_one = false;
    };

    struct CmdBufferState {
        VkPipeline current_pipeline = VK_NULL_HANDLE;
        std::shared_ptr<PipelineDivisorInfo> active_divisor_info;

        static constexpr uint32_t MAX_VERTEX_BINDINGS = 32;
        VkBuffer bound_buffers[MAX_VERTEX_BINDINGS]{};
        VkDeviceSize bound_offsets[MAX_VERTEX_BINDINGS]{};
    };

    bool is_phys_device_native(VkPhysicalDevice physDev);
    bool is_device_native(VkDevice device);
    bool query_native_support(VkPhysicalDevice physDev);

    CmdBufferState* get_or_create_cmd_state(VkCommandBuffer cmd);
    VkDevice get_device_for_cmd(VkCommandBuffer cmd);

    std::mutex m_mutex;
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, bool> m_device_native_support;
    std::unordered_map<uint64_t, VkDevice> m_cmd_devices;
    std::atomic<VkDevice> m_last_device{VK_NULL_HANDLE};

    std::atomic<bool> m_has_divisor_pipelines{false};
    std::unordered_map<uint64_t, std::shared_ptr<PipelineDivisorInfo>> m_pipeline_divisors;

    std::unordered_map<const void*, std::unordered_map<uint32_t, PipelineDivisorInfo>> m_pending_divisors;

    std::unordered_map<uint64_t, std::unique_ptr<CmdBufferState>> m_cmd_states;
};

#endif // VERTEX_ATTRIBUTE_DIVISOR_H

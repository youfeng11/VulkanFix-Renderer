#ifndef DYNAMIC_RENDERING_H
#define DYNAMIC_RENDERING_H

#include "layer_module.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <mutex>
#include <shared_mutex>
#include <atomic>

#ifndef VK_KHR_dynamic_rendering
#define VK_KHR_dynamic_rendering 1
#define VK_KHR_DYNAMIC_RENDERING_SPEC_VERSION 1
#define VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME "VK_KHR_dynamic_rendering"

typedef VkFlags VkRenderingFlagsKHR;
typedef VkFlags VkRenderingFlagBitsKHR;

#define VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR ((VkRenderingFlagsKHR)0x00000001)
#define VK_RENDERING_SUSPENDING_BIT_KHR ((VkRenderingFlagsKHR)0x00000002)
#define VK_RENDERING_RESUMING_BIT_KHR ((VkRenderingFlagsKHR)0x00000004)

typedef struct VkRenderingAttachmentInfoKHR {
    VkStructureType          sType;
    const void*              pNext;
    VkImageView              imageView;
    VkImageLayout            imageLayout;
    VkResolveModeFlagBits    resolveMode;
    VkImageView              resolveImageView;
    VkImageLayout            resolveImageLayout;
    VkAttachmentLoadOp       loadOp;
    VkAttachmentStoreOp      storeOp;
    VkClearValue             clearValue;
} VkRenderingAttachmentInfoKHR;

typedef struct VkRenderingInfoKHR {
    VkStructureType                         sType;
    const void*                             pNext;
    VkRenderingFlagsKHR                     flags;
    VkRect2D                                renderArea;
    uint32_t                                layerCount;
    uint32_t                                viewMask;
    uint32_t                                colorAttachmentCount;
    const VkRenderingAttachmentInfoKHR*     pColorAttachments;
    const VkRenderingAttachmentInfoKHR*     pDepthAttachment;
    const VkRenderingAttachmentInfoKHR*     pStencilAttachment;
} VkRenderingInfoKHR;

typedef struct VkPipelineRenderingCreateInfoKHR {
    VkStructureType    sType;
    const void*        pNext;
    uint32_t           viewMask;
    uint32_t           colorAttachmentCount;
    const VkFormat*    pColorAttachmentFormats;
    VkFormat           depthAttachmentFormat;
    VkFormat           stencilAttachmentFormat;
} VkPipelineRenderingCreateInfoKHR;

typedef struct VkPhysicalDeviceDynamicRenderingFeaturesKHR {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           dynamicRendering;
} VkPhysicalDeviceDynamicRenderingFeaturesKHR;

typedef struct VkCommandBufferInheritanceRenderingInfoKHR {
    VkStructureType          sType;
    const void*              pNext;
    VkRenderingFlagsKHR      flags;
    uint32_t                 viewMask;
    uint32_t                 colorAttachmentCount;
    const VkFormat*          pColorAttachmentFormats;
    VkFormat                 depthAttachmentFormat;
    VkFormat                 stencilAttachmentFormat;
    VkSampleCountFlagBits    rasterizationSamples;
} VkCommandBufferInheritanceRenderingInfoKHR;

typedef void (VKAPI_PTR *PFN_vkCmdBeginRenderingKHR)(VkCommandBuffer commandBuffer, const VkRenderingInfoKHR* pRenderingInfo);
typedef void (VKAPI_PTR *PFN_vkCmdEndRenderingKHR)(VkCommandBuffer commandBuffer);
#endif

#ifndef VK_STRUCTURE_TYPE_RENDERING_INFO_KHR
#define VK_STRUCTURE_TYPE_RENDERING_INFO_KHR ((VkStructureType)1000044000)
#endif
#ifndef VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR
#define VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR ((VkStructureType)1000044001)
#endif
#ifndef VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR
#define VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR ((VkStructureType)1000044002)
#endif
#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR ((VkStructureType)1000044003)
#endif
#ifndef VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO_KHR
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO_KHR ((VkStructureType)1000044004)
#endif
#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES ((VkStructureType)53)
#endif

#ifndef VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR
#define VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR ((VkImageLayout)1000314000)
#endif
#ifndef VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR
#define VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR ((VkImageLayout)1000314001)
#endif

class DynamicRenderingModule : public IVulkanLayerModule {
public:
    DynamicRenderingModule();
    virtual ~DynamicRenderingModule() = default;

    const char* get_name() const override { return "VK_KHR_dynamic_rendering"; }

    // 1. Device Extensions:
    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    // 2. Features:
    void on_pre_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void*& pUserData) override;

    void on_post_get_features2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures,
        void* pUserData) override;

    // 3. Logical Device:
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

    // 4. Graphics Pipeline Creation:
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

    // 5. Command Buffers:
    void on_post_allocate_command_buffers(
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkResult result,
        VkCommandBuffer* pCommandBuffers) override;

    void on_free_command_buffers(
        VkDevice device,
        uint32_t count,
        const VkCommandBuffer* pCommandBuffers) override;

    void on_begin_command_buffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo) override;

    void on_reset_command_buffer(
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags) override;

    void on_pre_begin_command_buffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo,
        VkCommandBufferBeginInfo& modBeginInfo,
        VkCommandBufferInheritanceInfo& modInheritanceInfo,
        bool& modifiedInheritance) override;

    // 6. Image & ImageView Tracking:
    void on_post_create_image(
        VkDevice device,
        const VkImageCreateInfo* pCreateInfo,
        VkResult result,
        VkImage image) override;

    void on_destroy_image(
        VkDevice device,
        VkImage image) override;

    void on_post_create_image_view(
        VkDevice device,
        const VkImageViewCreateInfo* pCreateInfo,
        VkResult result,
        VkImageView imageView) override;

    void on_destroy_image_view(
        VkDevice device,
        VkImageView imageView) override;

    void on_post_get_swapchain_images(
        VkDevice device,
        VkSwapchainKHR swapchain,
        VkFormat format,
        VkExtent2D extent,
        VkImageUsageFlags usage,
        uint32_t count,
        const VkImage* pImages) override;

    // 7. Dynamic Rendering Commands:
    bool on_cmd_begin_rendering(
        VkCommandBuffer commandBuffer,
        const VkRenderingInfo* pRenderingInfo) override;

    bool on_cmd_end_rendering(
        VkCommandBuffer commandBuffer) override;

    bool is_device_native(VkDevice device) override;

private:
    bool is_phys_device_native(VkPhysicalDevice physDev);
    VkDevice get_device_for_cmd(VkCommandBuffer cmd);

    VkRenderPass get_or_create_pipeline_render_pass(
        VkDevice device,
        uint32_t colorAttachmentCount,
        const VkFormat* pColorAttachmentFormats,
        VkFormat depthAttachmentFormat,
        VkFormat stencilAttachmentFormat,
        VkSampleCountFlagBits samples,
        uint32_t viewMask
    );

    struct ImageMeta {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkExtent3D extent = {0, 0, 0};
        VkImageUsageFlags usage = 0;
    };

    struct ImageViewMeta {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkExtent2D extent = {0, 0};
        VkImage image = VK_NULL_HANDLE;
        VkImageUsageFlags usage = 0;
    };

    static constexpr uint32_t MAX_KEY_ATTACHMENTS = 16;

    struct PipelineRenderPassKey {
        std::array<VkFormat, MAX_KEY_ATTACHMENTS> colorFormats{};
        uint32_t colorAttachmentCount = 0;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        uint32_t viewMask = 0;

        bool operator==(const PipelineRenderPassKey& o) const {
            if (colorAttachmentCount != o.colorAttachmentCount ||
                depthFormat != o.depthFormat ||
                stencilFormat != o.stencilFormat ||
                samples != o.samples ||
                viewMask != o.viewMask) return false;
            for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
                if (colorFormats[i] != o.colorFormats[i]) return false;
            }
            return true;
        }
    };

    struct PipelineRenderPassKeyHash {
        size_t operator()(const PipelineRenderPassKey& k) const;
    };

    struct DynamicRenderPassKey {
        struct AttachmentDesc {
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            VkAttachmentLoadOp stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            VkAttachmentStoreOp stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout refLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            bool operator==(const AttachmentDesc& o) const {
                return format == o.format &&
                       samples == o.samples &&
                       loadOp == o.loadOp &&
                       storeOp == o.storeOp &&
                       stencilLoadOp == o.stencilLoadOp &&
                       stencilStoreOp == o.stencilStoreOp &&
                       initialLayout == o.initialLayout &&
                       finalLayout == o.finalLayout &&
                       refLayout == o.refLayout;
            }
        };

        std::array<AttachmentDesc, MAX_KEY_ATTACHMENTS> colorAttachments{};
        uint32_t colorAttachmentCount = 0;
        bool has_depth_stencil = false;
        AttachmentDesc depthStencilAttachment{};
        std::array<AttachmentDesc, MAX_KEY_ATTACHMENTS> resolveAttachments{};
        uint32_t resolveAttachmentCount = 0;

        bool operator==(const DynamicRenderPassKey& o) const {
            if (colorAttachmentCount != o.colorAttachmentCount ||
                resolveAttachmentCount != o.resolveAttachmentCount ||
                has_depth_stencil != o.has_depth_stencil) return false;
            if (has_depth_stencil && !(depthStencilAttachment == o.depthStencilAttachment)) return false;
            for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
                if (!(colorAttachments[i] == o.colorAttachments[i])) return false;
            }
            for (uint32_t i = 0; i < resolveAttachmentCount; ++i) {
                if (!(resolveAttachments[i] == o.resolveAttachments[i])) return false;
            }
            return true;
        }
    };

    struct DynamicRenderPassKeyHash {
        size_t operator()(const DynamicRenderPassKey& k) const;
    };

    struct FramebufferKey {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        std::array<VkImageView, MAX_KEY_ATTACHMENTS> views{};
        uint32_t viewCount = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 1;

        bool operator==(const FramebufferKey& o) const {
            if (renderPass != o.renderPass ||
                viewCount != o.viewCount ||
                width != o.width ||
                height != o.height ||
                layers != o.layers) return false;
            for (uint32_t i = 0; i < viewCount; ++i) {
                if (views[i] != o.views[i]) return false;
            }
            return true;
        }
    };

    struct FramebufferKeyHash {
        size_t operator()(const FramebufferKey& k) const;
    };

    struct CmdRenderingState {
        VkDevice device = VK_NULL_HANDLE;
        bool is_rendering = false;
        VkRenderPass activeRenderPass = VK_NULL_HANDLE;
        VkFramebuffer activeFramebuffer = VK_NULL_HANDLE;
    };

    struct DeviceResources {
        std::vector<VkRenderPass> renderPasses;
        std::vector<VkFramebuffer> framebuffers;
    };

    mutable std::shared_mutex m_rw_mutex;
    std::mutex m_cmd_mutex;
    std::atomic<VkDevice> m_primary_dev{VK_NULL_HANDLE};
    std::atomic<bool> m_primary_native{false};
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, bool> m_device_native_support;
    std::unordered_map<uint64_t, VkDevice> m_cmd_devices;

    std::unordered_map<uint64_t, ImageMeta> m_images;
    std::unordered_map<uint64_t, ImageViewMeta> m_image_views;

    std::unordered_map<PipelineRenderPassKey, VkRenderPass, PipelineRenderPassKeyHash> m_pipeline_rp_cache;
    std::unordered_map<DynamicRenderPassKey, VkRenderPass, DynamicRenderPassKeyHash> m_dynamic_rp_cache;
    std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKeyHash> m_framebuffer_cache;
    std::unordered_map<uint64_t, std::vector<FramebufferKey>> m_image_view_to_framebuffers;

    std::unordered_map<uint64_t, CmdRenderingState> m_cmd_rendering_states;
    std::unordered_map<uint64_t, DeviceResources> m_device_resources;
};

#endif // DYNAMIC_RENDERING_H

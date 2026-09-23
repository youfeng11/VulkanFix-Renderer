#ifndef RENDERPASS2_H
#define RENDERPASS2_H

#include "extension_module_base.h"

class RenderPass2Module : public ExtensionModuleBase {
public:
    RenderPass2Module();
    ~RenderPass2Module() override = default;

    bool on_create_render_pass2(
        VkDevice device,
        const VkRenderPassCreateInfo2* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkRenderPass* pRenderPass,
        VkResult& outResult) override;

    bool on_cmd_begin_render_pass2(
        VkCommandBuffer commandBuffer,
        const VkRenderPassBeginInfo* pRenderPassBegin,
        const VkSubpassBeginInfo* pSubpassBeginInfo) override;

    bool on_cmd_next_subpass2(
        VkCommandBuffer commandBuffer,
        const VkSubpassBeginInfo* pSubpassBeginInfo,
        const VkSubpassEndInfo* pSubpassEndInfo) override;

    bool on_cmd_end_render_pass2(
        VkCommandBuffer commandBuffer,
        const VkSubpassEndInfo* pSubpassEndInfo) override;
};

#endif // RENDERPASS2_H

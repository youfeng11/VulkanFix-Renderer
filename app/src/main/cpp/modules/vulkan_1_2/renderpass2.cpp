#include "renderpass2.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include <vector>

REGISTER_LAYER_MODULE(RenderPass2Module);

static inline VkImageLayout sanitize_layout(VkImageLayout layout) {
    if ((int32_t)layout == 1000241000 /* VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL */ ||
        (int32_t)layout == 1000241002 /* VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL */) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if ((int32_t)layout == 1000241001 /* VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL */ ||
        (int32_t)layout == 1000241003 /* VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL */) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    return layout;
}

RenderPass2Module::RenderPass2Module()
    : ExtensionModuleBase(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_SPEC_VERSION, VK_API_VERSION_1_2) {
    LOGI("RenderPass2Module initialized");
}

bool RenderPass2Module::on_create_render_pass2(
    VkDevice device,
    const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    PFN_vkCreateRenderPass2KHR real_fn =
        (PFN_vkCreateRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCreateRenderPass2KHR");
    if (!real_fn) {
        real_fn = (PFN_vkCreateRenderPass2KHR) get_real_proc(get_last_instance(), device, "vkCreateRenderPass2");
    }
    if (real_fn) {
        outResult = real_fn(device, pCreateInfo, pAllocator, pRenderPass);
        return true;
    }

    if (!pCreateInfo) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    // Convert VkRenderPassCreateInfo2 to Vulkan 1.0 VkRenderPassCreateInfo
    std::vector<VkAttachmentDescription> attachments(pCreateInfo->attachmentCount);
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
        const auto& src = pCreateInfo->pAttachments[i];
        auto& dst = attachments[i];
        dst.flags = src.flags;
        dst.format = src.format;
        dst.samples = src.samples;
        dst.loadOp = src.loadOp;
        dst.storeOp = src.storeOp;
        dst.stencilLoadOp = src.stencilLoadOp;
        dst.stencilStoreOp = src.stencilStoreOp;
        dst.initialLayout = sanitize_layout(src.initialLayout);
        dst.finalLayout = sanitize_layout(src.finalLayout);
    }

    std::vector<std::vector<VkAttachmentReference>> inputRefs(pCreateInfo->subpassCount);
    std::vector<std::vector<VkAttachmentReference>> colorRefs(pCreateInfo->subpassCount);
    std::vector<std::vector<VkAttachmentReference>> resolveRefs(pCreateInfo->subpassCount);
    std::vector<VkAttachmentReference> dsRefs(pCreateInfo->subpassCount);
    std::vector<bool> hasDs(pCreateInfo->subpassCount, false);

    std::vector<VkSubpassDescription> subpasses(pCreateInfo->subpassCount);
    for (uint32_t s = 0; s < pCreateInfo->subpassCount; ++s) {
        const auto& src = pCreateInfo->pSubpasses[s];
        auto& dst = subpasses[s];
        dst.flags = src.flags;
        dst.pipelineBindPoint = src.pipelineBindPoint;

        if (src.inputAttachmentCount > 0 && src.pInputAttachments) {
            inputRefs[s].resize(src.inputAttachmentCount);
            for (uint32_t i = 0; i < src.inputAttachmentCount; ++i) {
                inputRefs[s][i].attachment = src.pInputAttachments[i].attachment;
                inputRefs[s][i].layout = sanitize_layout(src.pInputAttachments[i].layout);
            }
            dst.inputAttachmentCount = src.inputAttachmentCount;
            dst.pInputAttachments = inputRefs[s].data();
        }

        if (src.colorAttachmentCount > 0 && src.pColorAttachments) {
            colorRefs[s].resize(src.colorAttachmentCount);
            for (uint32_t i = 0; i < src.colorAttachmentCount; ++i) {
                colorRefs[s][i].attachment = src.pColorAttachments[i].attachment;
                colorRefs[s][i].layout = sanitize_layout(src.pColorAttachments[i].layout);
            }
            dst.colorAttachmentCount = src.colorAttachmentCount;
            dst.pColorAttachments = colorRefs[s].data();
        }

        if (src.pResolveAttachments) {
            resolveRefs[s].resize(src.colorAttachmentCount);
            for (uint32_t i = 0; i < src.colorAttachmentCount; ++i) {
                resolveRefs[s][i].attachment = src.pResolveAttachments[i].attachment;
                resolveRefs[s][i].layout = sanitize_layout(src.pResolveAttachments[i].layout);
            }
            dst.pResolveAttachments = resolveRefs[s].data();
        }

        if (src.pDepthStencilAttachment) {
            dsRefs[s].attachment = src.pDepthStencilAttachment->attachment;
            dsRefs[s].layout = sanitize_layout(src.pDepthStencilAttachment->layout);
            dst.pDepthStencilAttachment = &dsRefs[s];
        }
    }

    std::vector<VkSubpassDependency> dependencies(pCreateInfo->dependencyCount);
    for (uint32_t i = 0; i < pCreateInfo->dependencyCount; ++i) {
        const auto& src = pCreateInfo->pDependencies[i];
        auto& dst = dependencies[i];
        dst.srcSubpass = src.srcSubpass;
        dst.dstSubpass = src.dstSubpass;
        dst.srcStageMask = src.srcStageMask;
        dst.dstStageMask = src.dstStageMask;
        dst.srcAccessMask = src.srcAccessMask;
        dst.dstAccessMask = src.dstAccessMask;
        dst.dependencyFlags = src.dependencyFlags;
    }

    VkRenderPassCreateInfo ci1{};
    ci1.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci1.pNext = nullptr;
    ci1.flags = pCreateInfo->flags;
    ci1.attachmentCount = (uint32_t)attachments.size();
    ci1.pAttachments = attachments.empty() ? nullptr : attachments.data();
    ci1.subpassCount = (uint32_t)subpasses.size();
    ci1.pSubpasses = subpasses.empty() ? nullptr : subpasses.data();
    ci1.dependencyCount = (uint32_t)dependencies.size();
    ci1.pDependencies = dependencies.empty() ? nullptr : dependencies.data();

    PFN_vkCreateRenderPass real_crp =
        (PFN_vkCreateRenderPass) get_real_proc(get_last_instance(), device, "vkCreateRenderPass");
    if (real_crp) {
        outResult = real_crp(device, &ci1, pAllocator, pRenderPass);
    } else {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
    }
    return true;
}

bool RenderPass2Module::on_cmd_begin_render_pass2(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.CmdBeginRenderPass2) {
        dt.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
        return true;
    }

    if (dt.CmdBeginRenderPass) {
        VkSubpassContents contents = pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE;
        dt.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
        return true;
    }

    PFN_vkCmdBeginRenderPass real_begin =
        (PFN_vkCmdBeginRenderPass) get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass");
    if (real_begin) {
        VkSubpassContents contents = pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE;
        real_begin(commandBuffer, pRenderPassBegin, contents);
    }
    return true;
}

bool RenderPass2Module::on_cmd_next_subpass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassBeginInfo* pSubpassBeginInfo,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.CmdNextSubpass2) {
        dt.CmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
        return true;
    }

    PFN_vkCmdNextSubpass real_next =
        (PFN_vkCmdNextSubpass) get_real_proc(get_last_instance(), device, "vkCmdNextSubpass");
    if (real_next) {
        VkSubpassContents contents = pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE;
        real_next(commandBuffer, contents);
    }
    return true;
}

bool RenderPass2Module::on_cmd_end_render_pass2(
    VkCommandBuffer commandBuffer,
    const VkSubpassEndInfo* pSubpassEndInfo
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.CmdEndRenderPass2) {
        dt.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
        return true;
    }

    if (dt.CmdEndRenderPass) {
        dt.CmdEndRenderPass(commandBuffer);
        return true;
    }

    PFN_vkCmdEndRenderPass real_end =
        (PFN_vkCmdEndRenderPass) get_real_proc(get_last_instance(), device, "vkCmdEndRenderPass");
    if (real_end) {
        real_end(commandBuffer);
    }
    return true;
}

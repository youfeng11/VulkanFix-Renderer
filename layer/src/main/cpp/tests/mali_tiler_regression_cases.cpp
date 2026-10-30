#include "core/layer_manager.h"
#include "modules/vulkan_1_0/multi_draw_indirect.h"
#include "modules/vulkan_1_3/dynamic_rendering.h"
#include <cstring>
#include <map>
#include <string>

struct DynamicRenderingTestAccess {
    static uint32_t set_device_count(LayerManager& manager, uint32_t count) {
        return manager.m_device_count.exchange(count, std::memory_order_relaxed);
    }
    static size_t indexed_framebuffers(const DynamicRenderingModule &module) {
        size_t count = 0;
        for (const auto &entry : module.m_image_view_to_framebuffers)
            count += entry.second.size();
        return count;
    }
    static size_t images(const DynamicRenderingModule &module) { return module.m_images.size(); }
    static size_t views(const DynamicRenderingModule &module) {
        return module.m_image_views.size();
    }
};

namespace {
bool active = false, fail_resume = false;
uint32_t vendor = 0x13b5;
uint64_t serial = 0x70000;
std::map<VkRenderPass, std::vector<VkAttachmentDescription>> passes;
std::map<VkFramebuffer, VkExtent2D> framebuffer_extents;
struct Begin {
    VkRenderPass pass;
    VkFramebuffer framebuffer;
    uint32_t clears;
};
std::vector<Begin> begins;
std::string events;
unsigned barriers = 0, draws = 0;
bool scopes_valid = true, barrier_valid = true, draw_args_valid = true;
bool in_pass = false;
VkCommandBuffer command = (VkCommandBuffer)(uintptr_t)0x777700;
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties *p) {
    *p = {};
    p->vendorID = vendor;
    p->apiVersion = VK_API_VERSION_1_1;
}
VKAPI_ATTR void VKAPI_CALL features(VkPhysicalDevice, VkPhysicalDeviceFeatures *p) {
    *p = {};
    p->multiDrawIndirect = VK_TRUE;
}
VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice, const char *, uint32_t *count,
                                          VkExtensionProperties *) {
    *count = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create_pass(VkDevice, const VkRenderPassCreateInfo *ci,
                                           const VkAllocationCallbacks *, VkRenderPass *out) {
    if (fail_resume && ci->attachmentCount &&
        ci->pAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = (VkRenderPass)++serial;
    passes[*out] = {ci->pAttachments, ci->pAttachments + ci->attachmentCount};
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_pass(VkDevice, VkRenderPass pass,
                                        const VkAllocationCallbacks *) {
    passes.erase(pass);
}
VKAPI_ATTR VkResult VKAPI_CALL create_pass2(VkDevice device, const VkRenderPassCreateInfo2 *ci,
                                            const VkAllocationCallbacks *allocator,
                                            VkRenderPass *out) {
    std::vector<VkAttachmentDescription> attachments(ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
        const auto &a = ci->pAttachments[i];
        attachments[i] = {a.flags,          a.format,        a.samples,
                          a.loadOp,         a.storeOp,       a.stencilLoadOp,
                          a.stencilStoreOp, a.initialLayout, a.finalLayout};
    }
    VkRenderPassCreateInfo converted{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    converted.attachmentCount = (uint32_t)attachments.size();
    converted.pAttachments = attachments.data();
    return create_pass(device, &converted, allocator, out);
}
VKAPI_ATTR VkResult VKAPI_CALL create_fb(VkDevice, const VkFramebufferCreateInfo *info,
                                         const VkAllocationCallbacks *, VkFramebuffer *out) {
    *out = (VkFramebuffer)++serial;
    framebuffer_extents[*out] = {info->width, info->height};
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_fb(VkDevice, VkFramebuffer framebuffer,
                                      const VkAllocationCallbacks *) {
    framebuffer_extents.erase(framebuffer);
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice, const VkCommandBufferAllocateInfo *ci,
                                        VkCommandBuffer *out) {
    for (uint32_t i = 0; i < ci->commandBufferCount; ++i)
        out[i] = command;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL begin_pass(VkCommandBuffer, const VkRenderPassBeginInfo *info,
                                      VkSubpassContents) {
    scopes_valid &= !in_pass;
    in_pass = true;
    events += 'B';
    begins.push_back({info->renderPass, info->framebuffer, info->clearValueCount});
}
VKAPI_ATTR void VKAPI_CALL end_pass(VkCommandBuffer) {
    scopes_valid &= in_pass;
    in_pass = false;
    events += 'E';
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags src,
                                   VkPipelineStageFlags dst, VkDependencyFlags, uint32_t count,
                                   const VkMemoryBarrier *memory, uint32_t,
                                   const VkBufferMemoryBarrier *, uint32_t,
                                   const VkImageMemoryBarrier *) {
    ++barriers;
    events += 'M';
    barrier_valid &=
        !in_pass && src == VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT &&
        dst == VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT && count == 1 && memory &&
        memory[0].srcAccessMask == VK_ACCESS_MEMORY_WRITE_BIT &&
        memory[0].dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}
VKAPI_ATTR void VKAPI_CALL indirect(VkCommandBuffer, VkBuffer buffer, VkDeviceSize offset,
                                    uint32_t count, uint32_t stride) {
    scopes_valid &= in_pass;
    ++draws;
    events += 'D';
    draw_args_valid &= buffer == (VkBuffer)0x888 && offset == 40 && count == 8 && stride == 24;
}
void reset_events() {
    begins.clear();
    events.clear();
    barriers = draws = 0;
    in_pass = false;
}
} // namespace
void *mali_tiler_mock_proc(const char *name) {
    if (!active)
        return nullptr;
#define PROC(n, f)                                                                                 \
    if (!strcmp(name, "vk" #n))                                                                    \
    return (void *)f
    PROC(GetPhysicalDeviceProperties, properties);
    PROC(GetPhysicalDeviceFeatures, features);
    PROC(EnumerateDeviceExtensionProperties, extensions);
    PROC(CreateRenderPass, create_pass);
    PROC(CreateRenderPass2, create_pass2);
    PROC(CreateRenderPass2KHR, create_pass2);
    PROC(DestroyRenderPass, destroy_pass);
    PROC(CreateFramebuffer, create_fb);
    PROC(DestroyFramebuffer, destroy_fb);
    PROC(CmdBeginRenderPass, begin_pass);
    PROC(CmdEndRenderPass, end_pass);
    PROC(CmdPipelineBarrier, barrier);
    PROC(CmdDrawIndexedIndirect, indirect);
    PROC(AllocateCommandBuffers, allocate);
#undef PROC
    return nullptr;
}
void run_mali_tiler_regressions(void (*check)(bool, const char *)) {
    active = true;
    auto &manager = LayerManager::get();
    auto owned = std::make_unique<DynamicRenderingModule>();
    auto &module = *owned;
    manager.register_module(std::move(owned));
    auto device = (VkDevice)(uintptr_t)0x77000;
    auto phys = (VkPhysicalDevice)(uintptr_t)0x77001;
    module.on_post_create_device(phys, device, VK_SUCCESS, nullptr);
    auto multi = std::make_unique<MultiDrawIndirectModule>();
    multi->on_post_create_device(phys, device, VK_SUCCESS, nullptr);
    manager.register_module(std::move(multi));
    manager.init_device_dispatch_table(device);
    manager.update_device_bypass_flags(device);
    check(manager.should_intercept_device_proc(device, "vkCreateRenderPass2KHR") &&
              manager.should_intercept_device_proc(device, "vkCmdBeginQuery") &&
              manager.should_intercept_device_proc(device, "vkDestroyCommandPool"),
          "native entry-point shortcuts preserve tiler metadata, query and pool tracking");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandBufferCount = 1;
    check(manager.dispatch_allocate_command_buffers(device, &allocation, &command) == VK_SUCCESS,
          "tiler guard tracks the primary command buffer through the manager");
    auto image = (VkImage)0x779;
    auto view = (VkImageView)0x780;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.extent = {64, 64, 1};
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    module.on_post_create_image(device, &ici, VK_SUCCESS, image);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.format = ici.format;
    module.on_post_create_image_view(device, &vi, VK_SUCCESS, view);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {64, 64};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    auto draw = [&] {
        manager.dispatch_cmd_draw_indexed_indirect(command, (VkBuffer)0x888, 40, 8, 24);
    };
    auto begin = [&] {
        reset_events();
        module.on_begin_command_buffer(command, nullptr);
        check(module.on_cmd_begin_rendering(command, &rendering), "emulated scope begins");
    };
    begin();
    for (unsigned i = 0; i < 9; ++i)
        draw();
    module.on_cmd_end_rendering(command);
    check(begins.size() == 3 && barriers == 2 && draws == 9,
          "nine intact indirect commands are executed in three bounded batches");
    check(draw_args_valid, "multi-draw count, buffer, offset and stride remain unchanged");
    check(scopes_valid && barrier_valid && events.find("EMBD") != std::string::npos,
          "continuation ends the pass, drains graphics, begins LOAD, then draws");
    check(begins[0].framebuffer == begins[1].framebuffer && begins[1].clears == 0 &&
              passes[begins[0].pass][0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
              passes[begins[1].pass][0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD,
          "initial clear runs once and continuation reuses the compatible framebuffer with LOAD");
    module.block_tiler_split(command, true);
    module.block_tiler_split(command, true);
    module.on_cmd_begin_rendering(command, &rendering);
    auto before = barriers;
    for (unsigned i = 0; i < 8; ++i)
        draw();
    module.block_tiler_split(command, false);
    draw();
    check(barriers == before, "nested query/feedback scopes defer every split");
    module.block_tiler_split(command, false);
    draw();
    check(barriers == before + 1, "deferred budget is serviced after the final scope ends");
    module.on_cmd_end_rendering(command);
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    begin();
    for (unsigned i = 0; i < 8; ++i)
        draw();
    module.on_cmd_end_rendering(command);
    check(barriers == 0, "DONT_CARE attachments keep their original discard semantics");
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    rendering.flags = VK_RENDERING_SUSPENDING_BIT;
    begin();
    for (unsigned i = 0; i < 8; ++i)
        draw();
    module.on_cmd_end_rendering(command);
    check(barriers == 0, "suspended rendering is never split");
    rendering.flags = 0;
    begin();
    module.before_draw(command, 262144);
    module.before_draw(command, 3);
    module.on_cmd_end_rendering(command);
    check(barriers == 1, "direct vertex pressure also starts a preserving continuation");
    begin();
    module.before_draw(command, 0, 129);
    module.before_draw(command, 0, 1);
    module.on_cmd_end_rendering(command);
    check(barriers == 1,
          "a large multi-draw stays intact and the following draw starts a new batch");
    begin();
    module.before_draw(command, 0, 128);
    module.before_draw(command, 3);
    module.on_cmd_end_rendering(command);
    check(barriers == 1, "a direct draw drains an exhausted indirect budget too");
    begin();
    module.before_draw(command, 262144);
    module.before_draw(command, 0, 1);
    module.on_cmd_end_rendering(command);
    check(barriers == 1, "an indirect command drains an exhausted direct budget too");
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    module.on_post_create_image(device, &ici, VK_SUCCESS, (VkImage)0x781);
    vi.image = (VkImage)0x781;
    vi.format = ici.format;
    module.on_post_create_image_view(device, &vi, VK_SUCCESS, (VkImageView)0x782);
    attachment.imageView = (VkImageView)0x782;
    fail_resume = true;
    begin();
    for (unsigned i = 0; i < 8; ++i)
        draw();
    module.on_cmd_end_rendering(command);
    check(barriers == 0 && draws == 8 && !in_pass,
          "continuation allocation failure leaves the original pass and all draws intact");
    fail_resume = false;

    VkAttachmentDescription legacyAttachments[2]{};
    for (auto &a : legacyAttachments) {
        a.format = VK_FORMAT_R8G8B8A8_UNORM;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    legacyAttachments[1].format = VK_FORMAT_D32_SFLOAT;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo legacyCreate{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    legacyCreate.attachmentCount = 2;
    legacyCreate.pAttachments = legacyAttachments;
    legacyCreate.subpassCount = 1;
    legacyCreate.pSubpasses = &subpass;
    VkRenderPass legacyPass{};
    create_pass(device, &legacyCreate, nullptr, &legacyPass);
    manager.track_render_pass(device, &legacyCreate, legacyPass);
    VkRenderPassBeginInfo legacyBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    legacyBegin.renderPass = legacyPass;
    legacyBegin.framebuffer = (VkFramebuffer)0x799;
    legacyBegin.renderArea = {{0, 0}, {2048, 2048}};
    VkClearValue clears[2]{};
    legacyBegin.clearValueCount = 2;
    legacyBegin.pClearValues = clears;
    auto legacy_begin = [&] {
        reset_events();
        module.on_begin_command_buffer(command, nullptr);
        manager.dispatch_cmd_begin_render_pass(command, &legacyBegin, VK_SUBPASS_CONTENTS_INLINE);
    };
    legacy_begin();
    for (unsigned i = 0; i < 9; ++i)
        draw();
    manager.dispatch_cmd_end_render_pass(command);
    check(begins.size() == 3 && barriers == 2 && draws == 9,
          "legacy BSL render pass now executes preserving continuations through the manager");
    const auto &resumeAttachments = passes[begins.back().pass];
    check(resumeAttachments.size() == 2 &&
              resumeAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
              resumeAttachments[1].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
              resumeAttachments[0].initialLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
              resumeAttachments[1].initialLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          "legacy continuation loads color and depth from the previous final layout");
    check(begins.front().clears == 2 && begins.back().clears == 0 &&
              begins.back().framebuffer == legacyBegin.framebuffer && scopes_valid,
          "legacy framebuffer, render area and clear-once semantics survive splitting");
    legacy_begin();
    manager.block_tiler_split(command, true);
    for (unsigned i = 0; i < 8; ++i)
        draw();
    check(barriers == 0, "legacy active query defers continuation");
    manager.block_tiler_split(command, false);
    draw();
    manager.dispatch_cmd_end_render_pass(command);
    check(barriers == 1, "legacy exhausted budget drains after query ends");
    reset_events();
    manager.dispatch_cmd_begin_render_pass(command, &legacyBegin,
                                           VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    for (unsigned i = 0; i < 8; ++i)
        module.before_draw(command, 0, 8);
    manager.dispatch_cmd_end_render_pass(command);
    check(barriers == 0, "legacy secondary contents never split");
    manager.forget_render_pass(device, legacyPass);
    destroy_pass(device, legacyPass, nullptr);

    VkAttachmentDescription2 a2[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        const auto &a = legacyAttachments[i];
        a2[i] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
                 nullptr,
                 a.flags,
                 a.format,
                 a.samples,
                 VK_ATTACHMENT_LOAD_OP_LOAD,
                 a.storeOp,
                 a.stencilLoadOp,
                 a.stencilStoreOp,
                 VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_GENERAL};
    }
    VkAttachmentReference2 c2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0,
                              VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference2 d2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1,
                              VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription2 s2{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    s2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    s2.colorAttachmentCount = 1;
    s2.pColorAttachments = &c2;
    s2.pDepthStencilAttachment = &d2;
    VkRenderPassCreateInfo2 ci2{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    ci2.attachmentCount = 2;
    ci2.pAttachments = a2;
    ci2.subpassCount = 1;
    ci2.pSubpasses = &s2;
    check(manager.dispatch_create_render_pass2(device, &ci2, nullptr, &legacyPass) == VK_SUCCESS,
          "RenderPass2 creation also tracks native-dispatch passes");
    legacyBegin.renderPass = legacyPass;
    reset_events();
    module.on_begin_command_buffer(command, nullptr);
    VkSubpassBeginInfo sb{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
    manager.dispatch_cmd_begin_render_pass2(command, &legacyBegin, &sb);
    for (unsigned i = 0; i < 9; ++i)
        draw();
    VkSubpassEndInfo se{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
    manager.dispatch_cmd_end_render_pass2(command, &se);
    check(barriers == 2 && begins.size() == 3 && !in_pass,
          "RenderPass2 begin/end forwards and tracks the same protected scope");
    manager.forget_render_pass(device, legacyPass);
    destroy_pass(device, legacyPass, nullptr);
    module.on_free_command_buffers(device, 1, &command);
    module.on_destroy_device(device);
    manager.remove_device_dispatch_table(device);
    check(passes.empty(), "all continuation render passes are retired at device destruction");
    vendor = 0x1234;
    module.on_post_create_device((VkPhysicalDevice)(uintptr_t)0x77002, device, VK_SUCCESS, nullptr);
    manager.init_device_dispatch_table(device);
    module.on_post_allocate_command_buffers(device, &allocation, VK_SUCCESS, &command);
    module.on_post_create_image(device, &ici, VK_SUCCESS, vi.image);
    module.on_post_create_image_view(device, &vi, VK_SUCCESS, attachment.imageView);
    begin();
    for (unsigned i = 0; i < 9; ++i)
        module.before_draw(command, 0, 8);
    module.on_cmd_end_rendering(command);
    check(barriers == 2, "unmeasured devices retain bounded rendering independently of vendor");
    module.on_free_command_buffers(device, 1, &command);
    module.on_destroy_device(device);
    manager.remove_device_dispatch_table(device);
    active = false;
}

void run_dynamic_rendering_lifetime_regressions(void (*check)(bool, const char *)) {
    active = true;
    vendor = 0x1234;
    auto &manager = LayerManager::get();
    auto owned = std::make_unique<DynamicRenderingModule>();
    auto &module = *owned;
    manager.register_module(std::move(owned));
    auto device = (VkDevice)(uintptr_t)0x88000;
    auto secondDevice = (VkDevice)(uintptr_t)0x88001;
    auto phys = (VkPhysicalDevice)(uintptr_t)0x88002;
    module.on_post_create_device(phys, device, VK_SUCCESS, nullptr);
    module.on_post_create_device(phys, secondDevice, VK_SUCCESS, nullptr);
    const uint32_t previousDeviceCount = DynamicRenderingTestAccess::set_device_count(manager, 2);
    manager.init_device_dispatch_table(device);
    manager.init_device_dispatch_table(secondDevice);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandBufferCount = 1;
    command = (VkCommandBuffer)(uintptr_t)0x888800;
    VkCommandBuffer firstCommand{};
    manager.dispatch_allocate_command_buffers(device, &allocation, &firstCommand);
    command = (VkCommandBuffer)(uintptr_t)0x888801;
    VkCommandBuffer secondCommand{};
    manager.dispatch_allocate_command_buffers(secondDevice, &allocation, &secondCommand);
    auto make_view = [&](VkDevice dev, uint64_t id, VkFormat format, uint32_t mip = 0,
                         VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.format = format;
        image.samples = samples;
        image.extent = {256, 256, 1};
        image.mipLevels = 4;
        image.usage = format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        module.on_post_create_image(dev, &image, VK_SUCCESS, (VkImage)id);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = (VkImage)id;
        view.format = format;
        view.subresourceRange.baseMipLevel = mip;
        module.on_post_create_image_view(dev, &view, VK_SUCCESS, (VkImageView)(id + 1));
        return (VkImageView)(id + 1);
    };
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = make_view(device, 0x90000, VK_FORMAT_R8G8B8A8_UNORM);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {64, 64};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    reset_events();
    module.on_cmd_begin_rendering(firstCommand, &rendering);
    module.on_cmd_end_rendering(firstCommand);
    auto firstPass = begins.back().pass;
    // Non-dispatchable handles may have the same value on distinct devices.
    make_view(secondDevice, 0x90000, VK_FORMAT_R8G8B8A8_UNORM);
    module.on_cmd_begin_rendering(secondCommand, &rendering);
    module.on_cmd_end_rendering(secondCommand);
    auto secondPass = begins.back().pass;
    check(firstPass != secondPass,
          "identical rendering on two devices owns distinct native render passes");
    module.on_destroy_device(device);
    manager.remove_device_dispatch_table(device);
    check(passes.count(secondPass) == 1,
          "destroying one device preserves the other device's cached pass");
    module.on_cmd_begin_rendering(secondCommand, &rendering);
    module.on_cmd_end_rendering(secondCommand);
    check(begins.back().pass == secondPass, "surviving device retains its live rendering cache");
    module.on_destroy_image_view(secondDevice, color.imageView);
    module.on_destroy_image(secondDevice, (VkImage)0x90000);

    color.imageView = make_view(secondDevice, 0x92000, VK_FORMAT_R8G8B8A8_UNORM, 2);
    module.on_cmd_begin_rendering(secondCommand, &rendering);
    module.on_cmd_end_rendering(secondCommand);
    check(framebuffer_extents[begins.back().framebuffer].width == 64 &&
              framebuffer_extents[begins.back().framebuffer].height == 64,
          "framebuffer dimensions use the view's mip level rather than the base image extent");
    module.on_destroy_image_view(secondDevice, color.imageView);
    module.on_destroy_image(secondDevice, (VkImage)0x92000);

    std::vector<uint64_t> msaaImages;
    auto msaa_view = [&](VkFormat format, VkSampleCountFlagBits samples) {
        auto id = 0xb0000 + msaaImages.size() * 2;
        msaaImages.push_back(id);
        return make_view(secondDevice, id, format, 0, samples);
    };
    VkRenderingAttachmentInfo colors[8]{};
    for (auto &attachment : colors) {
        attachment = color;
        attachment.imageView = msaa_view(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT);
        attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        attachment.resolveImageView = msaa_view(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);
        attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo msaaDepth = color;
    msaaDepth.imageView = msaa_view(VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_4_BIT);
    msaaDepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    rendering.colorAttachmentCount = 8;
    rendering.pColorAttachments = colors;
    rendering.pDepthAttachment = &msaaDepth;
    module.on_cmd_begin_rendering(secondCommand, &rendering);
    module.on_cmd_end_rendering(secondCommand);
    auto firstFramebuffer = begins.back().framebuffer;
    colors[7].resolveImageView = msaa_view(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);
    module.on_cmd_begin_rendering(secondCommand, &rendering);
    module.on_cmd_end_rendering(secondCommand);
    check(begins.back().framebuffer != firstFramebuffer,
          "all 17 views participate in an eight-color MSAA framebuffer cache key");

    const auto resolveView = colors[0].resolveImageView;
    colors[1].resolveImageView = VK_NULL_HANDLE;
    rendering.colorAttachmentCount = 2;
    rendering.pDepthAttachment = nullptr;
    module.on_cmd_begin_rendering(secondCommand, &rendering);
    module.on_cmd_end_rendering(secondCommand);
    const auto resolveFirstPass = begins.back().pass;
    colors[0].resolveImageView = VK_NULL_HANDLE;
    colors[1].resolveImageView = resolveView;
    module.on_cmd_begin_rendering(secondCommand, &rendering);
    module.on_cmd_end_rendering(secondCommand);
    check(begins.back().pass != resolveFirstPass,
          "moving a resolve between color slots cannot reuse an incompatible render pass");
    for (uint64_t id : msaaImages) {
        module.on_destroy_image_view(secondDevice, (VkImageView)(id + 1));
        module.on_destroy_image(secondDevice, (VkImage)id);
    }
    check(
        DynamicRenderingTestAccess::indexed_framebuffers(module) == 0 &&
            framebuffer_extents.empty(),
        "destroying MSAA and resolve views retires all framebuffer ownership and reverse indices");
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = make_view(secondDevice, 0x91000, VK_FORMAT_D32_SFLOAT);
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    rendering.pDepthAttachment = &depth;
    for (uint64_t i = 0; i < 512; ++i) {
        const uint64_t image = 0xa0000 + i * 2;
        color.imageView = make_view(secondDevice, image, VK_FORMAT_R8G8B8A8_UNORM);
        module.on_cmd_begin_rendering(secondCommand, &rendering);
        module.on_cmd_end_rendering(secondCommand);
        module.on_destroy_image_view(secondDevice, color.imageView);
        module.on_destroy_image(secondDevice, (VkImage)image);
    }
    check(DynamicRenderingTestAccess::indexed_framebuffers(module) == 0,
          "512 transient color targets leave no dead framebuffer references on persistent depth "
          "view");
    module.on_destroy_image_view(secondDevice, depth.imageView);
    module.on_destroy_image(secondDevice, (VkImage)0x91000);
    module.on_destroy_device(secondDevice);
    manager.remove_device_dispatch_table(secondDevice);
    check(passes.empty() && DynamicRenderingTestAccess::images(module) == 0 &&
              DynamicRenderingTestAccess::views(module) == 0,
          "all device-owned rendering metadata and native passes are reclaimed");
    check(!(vkfix::DeviceObjectKey::make(device, uint64_t(0x100000001)) ==
            vkfix::DeviceObjectKey::make(device, uint64_t(1))),
          "non-dispatchable 64-bit handle keys preserve high bits on every ABI");
    active = false;
    DynamicRenderingTestAccess::set_device_count(manager, previousDeviceCount);
}

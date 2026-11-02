#include "core/multiview_commands.h"
#include "core/multiview_pipeline_layouts.h"
#include "core/native_probe_resources.h"
#include "core/driver_loader.h"
#include "diagnostics/driver_post_shaders.h"
#include <dlfcn.h>
#include <cstdio>

using namespace vkfix;
using vkfix::nativeprobe::check;
namespace {
struct Context {
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{};
    VkDevice device{};
    bool pending = false, retain_device = false;
    template<class T> T get(const char* name) {
        auto fn = reinterpret_cast<T>(gipa(instance, name));
        if (!fn) throw std::runtime_error(std::string("missing ") + name); return fn;
    }
    template<class T> T dev(const char* name) {
        auto fn = reinterpret_cast<T>(gdpa(device, name));
        if (!fn) throw std::runtime_error(std::string("missing ") + name); return fn;
    }
} context;
bool force_reclear = false;
bool volume_mode = false, inspect_volume_passes = false;
bool undefined_volume = false, alias_mode = false;
bool query_mode = false;
bool statistics_queries = false;
bool wrong_query_slots = false;
uint32_t query_timestamp_bits = 0;
unsigned volume_passes = 0;
unsigned alias_memory_dependencies = 0;
std::vector<VkImageMemoryBarrier> unused_transitions;
std::vector<std::vector<VkImageMemoryBarrier>> transition_batches;
uint32_t wrong_slot(uint32_t query) { return query == 3 ? 5 : query == 4 ? 7 : query; }
VKAPI_ATTR void VKAPI_CALL begin_wrong_slot(VkCommandBuffer cmd, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags) {
    context.dev<PFN_vkCmdBeginQuery>("vkCmdBeginQuery")(cmd,pool,wrong_slot(query),flags);
}
VKAPI_ATTR void VKAPI_CALL end_wrong_slot(VkCommandBuffer cmd, VkQueryPool pool, uint32_t query) {
    context.dev<PFN_vkCmdEndQuery>("vkCmdEndQuery")(cmd,pool,wrong_slot(query));
}
VKAPI_ATTR void VKAPI_CALL record_unused_transitions(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t mc, const VkMemoryBarrier* memory, uint32_t bc, const VkBufferMemoryBarrier* buffers,
    uint32_t ic, const VkImageMemoryBarrier* images) {
    if (!MultiviewCommands::replaying() || bc) throw std::runtime_error("unexpected generated transition scope");
    if (mc) {
        if (!alias_mode || mc != 1 || ic || memory[0].srcAccessMask != VK_ACCESS_MEMORY_WRITE_BIT ||
            memory[0].dstAccessMask != (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT))
            throw std::runtime_error("unexpected alias memory dependency");
        ++alias_memory_dependencies;
    } else {
        if (!ic) throw std::runtime_error("empty generated transition");
        for (uint32_t i = 0; i < ic; ++i)
            if (images[i].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && !(images[i].srcAccessMask & VK_ACCESS_MEMORY_WRITE_BIT))
                throw std::runtime_error("UNDEFINED transition lost the write-after-write memory dependency");
        unused_transitions.assign(images, images + ic);
        transition_batches.emplace_back(images, images + ic);
    }
    context.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cmd,src,dst,flags,mc,memory,bc,buffers,ic,images);
}
VKAPI_ATTR VkResult VKAPI_CALL create_pass_reclear(VkDevice device, const VkRenderPassCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* pass) {
    auto changed = *info; std::vector<VkAttachmentDescription> attachments;
    if (info->attachmentCount) attachments.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
    if (inspect_volume_passes) for (const auto& attachment : attachments) {
        if (attachment.initialLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || attachment.finalLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            return VK_ERROR_INITIALIZATION_FAILED;
        ++volume_passes;
    }
    if (force_reclear) for (auto& attachment : attachments) attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    changed.pAttachments = attachments.data();
    return context.dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(device,&changed,allocator,pass);
}
struct QueryDraw { VkPipeline pipeline{}; VkPipelineLayout layout{}; VkBuffer vertices{}; };
QueryDraw make_query_draw(vkfix::nativeprobe::Resources<Context>& gpu, VkRenderPass pass) {
    auto& p = context;
    VkShaderModule modules[2]{};
    const uint32_t* code[] = {post_draw_vert,post_draw_frag}; const size_t sizes[] = {sizeof(post_draw_vert),sizeof(post_draw_frag)};
    auto destroy_shader = p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    for (unsigned i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = sizes[i]; ci.pCode = code[i];
        check(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&ci,nullptr,&modules[i]),"query shader");
        auto module = modules[i]; gpu.objects.push_back([=,&p] { destroy_shader(p.device,module,nullptr); });
    }
    QueryDraw result{};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,4}; VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &push;
    check(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&lci,nullptr,&result.layout),"query pipeline layout");
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout"); const auto layout = result.layout;
    gpu.objects.push_back([=,&p] { destroy_layout(p.device,layout,nullptr); });
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (unsigned i = 0; i < 2; ++i) { stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; stages[i].module = modules[i]; stages[i].pName = "main"; }
    VkVertexInputBindingDescription binding{0,4,VK_VERTEX_INPUT_RATE_INSTANCE}; VkVertexInputAttributeDescription attribute{0,0,VK_FORMAT_R32_SFLOAT,0};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vertex.vertexBindingDescriptionCount = vertex.vertexAttributeDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding; vertex.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,4,4,0,1}; VkRect2D scissor{{0,0},{4,4}};
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport_state.viewportCount = viewport_state.scissorCount = 1;
    viewport_state.pViewports = &viewport; viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{}; // Draws count samples without overwriting pixel-preservation markers.
    VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; colors.attachmentCount = 1; colors.pAttachments = &blend;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; ci.stageCount = 2; ci.pStages = stages;
    ci.pVertexInputState = &vertex; ci.pInputAssemblyState = &assembly; ci.pViewportState = &viewport_state; ci.pRasterizationState = &raster;
    ci.pMultisampleState = &samples; ci.pColorBlendState = &colors; ci.layout = result.layout; ci.renderPass = pass;
    check(p.dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(p.device,VK_NULL_HANDLE,1,&ci,nullptr,&result.pipeline),"query graphics pipeline");
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline"); const auto pipeline = result.pipeline;
    gpu.objects.push_back([=,&p] { destroy_pipeline(p.device,pipeline,nullptr); });
    result.vertices = gpu.buffer(4,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT).handle;
    return result;
}
void run(VkPhysicalDevice physical, uint32_t family, bool secondary, bool unused = false) {
    auto& p = context;
    vkfix::nativeprobe::Resources<Context> gpu(p, physical, family);
    const auto output = gpu.buffer(4 * 4 * 8 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM; ici.extent = {4,4,1};
    ici.mipLevels = 1; ici.arrayLayers = 8; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (volume_mode) { ici.imageType = VK_IMAGE_TYPE_3D; ici.flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT; ici.arrayLayers = 1; ici.extent.depth = 8; }
    VkImage image{}; check(p.dev<PFN_vkCreateImage>("vkCreateImage")(p.device, &ici, nullptr, &image), "image");
    auto destroy_image = p.dev<PFN_vkDestroyImage>("vkDestroyImage");
    gpu.objects.push_back([=, &p] { destroy_image(p.device, image, nullptr); });
    VkMemoryRequirements mr{}; p.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(p.device, image, &mr);
    const auto memory = gpu.allocate(mr, 0);
    check(p.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(p.device, image, memory, 0), "bind image");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = ici.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 7};
    VkImageView view{}; check(p.dev<PFN_vkCreateImageView>("vkCreateImageView")(p.device, &vi, nullptr, &view), "array view");
    auto destroy_view = p.dev<PFN_vkDestroyImageView>("vkDestroyImageView");
    gpu.objects.push_back([=, &p] { destroy_view(p.device, view, nullptr); });
    gpu.objects.push_back([&p] {
        MultiviewCommands::get().forget_device(p.device); MultiviewPipelineLayouts::get().forget_device(p.device);
        MultiviewDescriptors::get().forget_device(p.device); MultiviewAttachments::get().forget_device(p.device);
        MultiviewRenderPasses::get().forget_device(p.device);
    });
    auto& attachments = MultiviewAttachments::get(); attachments.remember_image(p.device, image, ici); attachments.remember_view(p.device, view, vi);
    VkImageView unused_view{};
    if (unused) {
        auto unused_vi = vi; unused_vi.subresourceRange.baseArrayLayer = 0; unused_vi.subresourceRange.layerCount = 8;
        check(p.dev<PFN_vkCreateImageView>("vkCreateImageView")(p.device,&unused_vi,nullptr,&unused_view),"unused view");
        gpu.objects.push_back([=,&p] { destroy_view(p.device,unused_view,nullptr); });
        attachments.remember_view(p.device,unused_view,unused_vi);
    }
    VkAttachmentDescription attachment{}; attachment.format = ici.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_GENERAL};
    if (volume_mode) { attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; }
    if (undefined_volume) attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
    const uint32_t mask = (1u << 2) | (1u << 4);
    VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO}; mv.subpassCount = 1; mv.pViewMasks = &mask;
    VkSubpassDependency dep{0,0,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT};
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rci.pNext = &mv; rci.attachmentCount = 1; rci.pAttachments = &attachment;
    rci.subpassCount = 1; rci.pSubpasses = &subpass; rci.dependencyCount = 1; rci.pDependencies = &dep;
    VkAttachmentDescription descriptions[] = {attachment,attachment};
    if (unused) {
        descriptions[1].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        rci.attachmentCount = 2; rci.pAttachments = descriptions;
    }
    const uint32_t alias_masks[] = {mask,mask};
    VkAttachmentReference alias_color{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription alias_subpasses[] = {subpass,subpass}; alias_subpasses[1].pColorAttachments = &alias_color;
    VkSubpassDependency alias_deps[] = {dep,dep,{0,1,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,0}};
    alias_deps[1].srcSubpass = alias_deps[1].dstSubpass = 1;
    if (alias_mode) {
        descriptions[0].flags = descriptions[1].flags = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;
        descriptions[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL; descriptions[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        mv.subpassCount = 2; mv.pViewMasks = alias_masks;
        rci.attachmentCount = 2; rci.pAttachments = descriptions; rci.subpassCount = 2; rci.pSubpasses = alias_subpasses;
        rci.dependencyCount = 3; rci.pDependencies = alias_deps;
    }
    VkRenderPass pass{}; VkResult status{};
    if (!MultiviewRenderPasses::get().create_fallback(p.device, &rci, nullptr, &pass, false, status)) throw std::runtime_error("missing fallback capture");
    check(status, "software pass");
    auto destroy_pass = p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    gpu.objects.push_back([=, &p] { destroy_pass(p.device, pass, nullptr); });
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fci.renderPass = pass; fci.attachmentCount = 1; fci.pAttachments = &view;
    const VkImageView source_views[] = {view,alias_mode ? view : unused_view};
    if (unused || alias_mode) { fci.attachmentCount = 2; fci.pAttachments = source_views; }
    fci.width = fci.height = 4; fci.layers = 1;
    VkFramebuffer fb{}; check(p.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device, &fci, nullptr, &fb), "public framebuffer");
    auto destroy_fb = p.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
    gpu.objects.push_back([=, &p] { destroy_fb(p.device, fb, nullptr); }); attachments.remember_framebuffer(p.device, fb, fci);
    QueryDraw query_draw{}; VkQueryPool occlusion{}, times{}, statistics{};
    if (query_mode) {
        query_draw = make_query_draw(gpu,pass);
        auto create = p.dev<PFN_vkCreateQueryPool>("vkCreateQueryPool"); auto destroy = p.dev<PFN_vkDestroyQueryPool>("vkDestroyQueryPool");
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qci.queryCount = 8; qci.queryType = VK_QUERY_TYPE_OCCLUSION;
        check(create(p.device,&qci,nullptr,&occlusion),"occlusion pool"); qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        gpu.objects.push_back([=,&p] { destroy(p.device,occlusion,nullptr); });
        check(create(p.device,&qci,nullptr,&times),"timestamp pool"); gpu.objects.push_back([=,&p] { destroy(p.device,times,nullptr); });
        if (statistics_queries) {
            qci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            qci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT;
            check(create(p.device,&qci,nullptr,&statistics),"statistics pool"); gpu.objects.push_back([=,&p] { destroy(p.device,statistics,nullptr); });
        }
    }
    auto cmd = gpu.begin();
    if (query_mode) {
        auto reset = p.dev<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool"); reset(cmd,occlusion,0,8); reset(cmd,times,0,8);
        if (statistics) reset(cmd,statistics,0,8);
    }
    auto barrier = p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; transition.image = image;
    transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    transition.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,volume_mode ? 1u : 8u};
    barrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&transition);
    VkClearColorValue zero{};
    p.dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&transition.subresourceRange);
    transition.oldLayout = VK_IMAGE_LAYOUT_GENERAL; transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    transition.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&transition);
    auto begin_pass = p.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass"); auto end_pass = p.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    auto clear_attachment = p.dev<PFN_vkCmdClearAttachments>("vkCmdClearAttachments");
    auto& commands = MultiviewCommands::get();
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    unused_transitions.clear();
    transition_batches.clear(); volume_passes = 0; alias_memory_dependencies = 0; inspect_volume_passes = volume_mode;
    check(commands.begin(p.device,cmd,VK_NULL_HANDLE,cbi,false,{begin_pass,end_pass,record_unused_transitions}),"capture primary");
    auto record_body = [&](VkCommandBuffer target, uint32_t phase) {
        const auto query_begin = wrong_query_slots ? begin_wrong_slot : p.dev<PFN_vkCmdBeginQuery>("vkCmdBeginQuery");
        const auto query_end = wrong_query_slots ? end_wrong_slot : p.dev<PFN_vkCmdEndQuery>("vkCmdEndQuery");
        if (query_mode) {
            commands.capture(target,true,[=] { return [=](VkCommandBuffer native,uint32_t) {
                auto& native_context = context;
                native_context.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(native,VK_PIPELINE_BIND_POINT_GRAPHICS,query_draw.pipeline);
                uint32_t mode = 0; native_context.dev<PFN_vkCmdPushConstants>("vkCmdPushConstants")(native,query_draw.layout,VK_SHADER_STAGE_VERTEX_BIT,0,4,&mode);
                VkDeviceSize offset = 0; native_context.dev<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")(native,0,1,&query_draw.vertices,&offset);
                return VK_SUCCESS;
            }; });
            commands.timestamp(target,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,times,0,p.dev<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp"));
            commands.begin_query(target,occlusion,3,VK_QUERY_CONTROL_PRECISE_BIT,query_begin,query_end);
            if (statistics) commands.begin_query(target,statistics,3,0,query_begin,query_end);
            commands.capture(target,false,[] { return [](VkCommandBuffer native,uint32_t) { context.dev<PFN_vkCmdDraw>("vkCmdDraw")(native,6,1,0,0); return VK_SUCCESS; }; });
        }
        commands.capture(target,false,[=] { return [=](VkCommandBuffer native,uint32_t selected) {
            VkClearAttachment a{}; a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; a.colorAttachment = 0;
            a.clearValue.color.float32[0] = float(selected) / 255; a.clearValue.color.float32[1] = float(17 + phase) / 255; a.clearValue.color.float32[3] = 1;
            VkClearRect rect{{{0,0},{1,1}},0,1}; clear_attachment(native,1,&a,1,&rect); return VK_SUCCESS;
        }; });
        VkMemoryBarrier memory_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; memory_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        if (!commands.pipeline_barrier(target,dep.srcStageMask,dep.dstStageMask,dep.dependencyFlags,1,&memory_barrier,0,nullptr,0,nullptr,false,barrier)) throw std::runtime_error("barrier not captured");
        if (query_mode) commands.capture(target,false,[] { return [](VkCommandBuffer native,uint32_t) { context.dev<PFN_vkCmdDraw>("vkCmdDraw")(native,6,1,0,0); return VK_SUCCESS; }; });
        commands.capture(target,false,[=] { return [=](VkCommandBuffer native,uint32_t selected) {
            VkClearAttachment a{}; a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; a.colorAttachment = 0;
            a.clearValue.color.float32[1] = float(selected) / 255; a.clearValue.color.float32[2] = float(33 + phase) / 255; a.clearValue.color.float32[3] = 1;
            VkClearRect rect{{{1,0},{1,1}},0,1}; clear_attachment(native,1,&a,1,&rect); return VK_SUCCESS;
        }; });
        if (query_mode) {
            commands.end_query(target,occlusion,3,query_end);
            if (statistics) commands.end_query(target,statistics,3,query_end);
            commands.timestamp(target,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,times,2,p.dev<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp"));
        }
    };
    VkClearValue red{}; red.color.float32[0] = red.color.float32[3] = 1;
    const VkClearValue source_clears[] = {red,red};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass = pass; rp.framebuffer = fb; rp.renderArea.extent = {4,4}; rp.clearValueCount = unused || alias_mode ? 2 : 1; rp.pClearValues = source_clears;
    commands.begin_pass(cmd,&rp,secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
    for (uint32_t phase = 0; phase < (alias_mode ? 2u : 1u); ++phase) {
    if (secondary) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool_info.queueFamilyIndex = family;
        VkCommandPool pool{}; check(p.dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(p.device,&pool_info,nullptr,&pool),"secondary pool");
        auto destroy_pool = p.dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool"); gpu.objects.push_back([=,&p] { destroy_pool(p.device,pool,nullptr); });
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY; ai.commandBufferCount = 1;
        VkCommandBuffer child{}; check(p.dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(p.device,&ai,&child),"secondary allocation");
        VkCommandBufferInheritanceInfo inherited{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}; inherited.renderPass = pass; inherited.subpass = phase;
        cbi.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT; cbi.pInheritanceInfo = &inherited;
        check(p.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(child,&cbi),"native secondary begin");
        check(commands.begin(p.device,child,pool,cbi,false,{begin_pass,end_pass}),"capture secondary"); record_body(child,phase);
        check(p.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(child),"native secondary end"); check(commands.error(child),"secondary capture");
        if (!commands.execute(cmd,1,&child)) throw std::runtime_error("secondary not captured");
    } else record_body(cmd,phase);
    if (phase + 1 < (alias_mode ? 2u : 1u)) commands.next_subpass(cmd,secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
    }
    commands.end_pass(cmd); check(commands.error(cmd),"primary capture");
    inspect_volume_passes = false;
    if (volume_mode && !alias_mode && (volume_passes != 4 || transition_batches.size() != 2 ||
        transition_batches[0].size() != 1 || transition_batches[1].size() != 1 ||
        transition_batches[0][0].oldLayout != (undefined_volume ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL) ||
        transition_batches[0][0].newLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        transition_batches[1][0].oldLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        transition_batches[1][0].newLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        transition_batches[0][0].subresourceRange.baseArrayLayer != 0 || transition_batches[0][0].subresourceRange.layerCount != 1 ||
        transition_batches[1][0].subresourceRange.baseArrayLayer != 0 || transition_batches[1][0].subresourceRange.layerCount != 1))
        throw std::runtime_error("3D whole-mip transitions must surround all four slice/segment passes once");
    if (alias_mode && (volume_passes != 8 || transition_batches.size() != 4 || alias_memory_dependencies != 2 ||
        transition_batches[0][0].oldLayout != VK_IMAGE_LAYOUT_GENERAL || transition_batches[0][0].newLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        transition_batches[1][0].oldLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || transition_batches[1][0].newLayout != VK_IMAGE_LAYOUT_GENERAL ||
        transition_batches[2][0].oldLayout != VK_IMAGE_LAYOUT_UNDEFINED || transition_batches[2][0].newLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        transition_batches[3][0].oldLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || transition_batches[3][0].newLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
        throw std::runtime_error("3D alias transition/dependency order is incorrect");
    if (unused && (unused_transitions.size() != 2 || unused_transitions[0].image != image ||
        unused_transitions[0].oldLayout != VK_IMAGE_LAYOUT_GENERAL || unused_transitions[0].newLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        unused_transitions[0].subresourceRange.baseArrayLayer != 2 || unused_transitions[0].subresourceRange.layerCount != 1 ||
        unused_transitions[1].subresourceRange.baseArrayLayer != 4 || unused_transitions[1].subresourceRange.layerCount != 1))
        throw std::runtime_error("missing/incorrect unused attachment transitions");
    transition.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; transition.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    transition.oldLayout = VK_IMAGE_LAYOUT_GENERAL; transition.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    for (uint32_t layer = 0; layer < (volume_mode ? 1u : 8u); ++layer) {
        transition.subresourceRange.baseArrayLayer = layer; transition.subresourceRange.layerCount = 1;
        transition.oldLayout = volume_mode || (unused && (layer == 2 || layer == 4)) ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&transition);
    }
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,8}; copy.imageExtent = {4,4,1};
    if (volume_mode) { copy.imageSubresource.layerCount = 1; copy.imageExtent.depth = 8; }
    p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
    gpu.host_barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT); gpu.submit(cmd); gpu.invalidate(output);
    if (query_mode) {
        uint64_t values[4]{}, stamps[8]{};
        auto read = p.dev<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults");
        const auto flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
        check(read(p.device,occlusion,3,2,sizeof(values),values,16,flags),"occlusion results");
        check(read(p.device,times,0,4,sizeof(stamps),stamps,16,flags),"timestamp results");
        if (values[0] != 32 || values[2] != 32 || !values[1] || !values[3]) throw std::runtime_error("precise multiview occlusion must report32 samples for each of two consecutive slots");
        if (statistics) {
            uint64_t counts[6]{};
            check(read(p.device,statistics,3,2,sizeof(counts),counts,24,flags),"pipeline statistics results");
            if (counts[0] != 12 || counts[1] != 4 || !counts[2] || counts[3] != 12 || counts[4] != 4 || !counts[5])
                throw std::runtime_error("each view must report12 input vertices,4 input primitives and availability");
            printf("PASS: %s overlapping pipeline-statistics queries=(12 vertices,4 primitives) per view\n",secondary ? "secondary" : "primary");
        }
        for (unsigned i = 0; i < 2; ++i) {
            uint64_t mask = query_timestamp_bits == 64 ? ~uint64_t(0) : (uint64_t(1) << query_timestamp_bits) - 1;
            if (!stamps[2*i+1] || !stamps[2*i+5] || ((stamps[2*i+4]-stamps[2*i]) & mask) > (mask >> 1))
                throw std::runtime_error("per-view timestamps are unavailable or move backwards");
        }
        const auto copied = gpu.buffer(sizeof(values),VK_BUFFER_USAGE_TRANSFER_DST_BIT); auto copy_cmd = gpu.begin();
        p.dev<PFN_vkCmdCopyQueryPoolResults>("vkCmdCopyQueryPoolResults")(copy_cmd,occlusion,3,2,copied.handle,0,16,flags | VK_QUERY_RESULT_WAIT_BIT);
        gpu.host_barrier(copy_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT); gpu.submit(copy_cmd); gpu.invalidate(copied);
        if (std::memcmp(copied.mapped,values,sizeof(values))) throw std::runtime_error("device query copy differs from host results/availability");
        printf("PASS: %s precise queries=(32,32), availability, device copy and per-view timestamps verified\n",secondary ? "secondary" : "primary");
    }
    const auto* pixels = static_cast<const uint8_t*>(output.mapped);
    for (uint32_t layer = 0; layer < 8; ++layer) for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        // A whole-mip UNDEFINED transition discards all slices, including those
        // that are never rendered. Only rendered slices have defined values.
        if ((undefined_volume || alias_mode) && layer != 3 && layer != 5) continue;
        uint8_t expected[4]{};
        if (layer == 3 || layer == 5) {
            const uint8_t selected = uint8_t(layer - 1); expected[0] = expected[3] = 255;
            if (pixel == 0) { expected[0] = selected; expected[1] = alias_mode ? 18 : 17; }
            if (pixel == 1) { expected[0] = 0; expected[1] = selected; expected[2] = alias_mode ? 34 : 33; }
        }
        const auto* actual = pixels + (layer * 16 + pixel) * 4;
        if (std::memcmp(actual,expected,4)) {
            char message[192]; std::snprintf(message,sizeof(message),"secondary=%u layer=%u pixel=%u got=(%u,%u,%u,%u) expected=(%u,%u,%u,%u)",
                unsigned(secondary),layer,pixel,actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]); throw std::runtime_error(message);
        }
    }
    printf("PASS: native GPU %s%s%s%s, views=(2,4), base layer=1, pre-barrier pixels preserved, clear-once%s\n",
        secondary ? "secondary barrier" : "primary barrier", unused ? ", unused-attachment transitions" : "", volume_mode ? ", 3D whole-mip transitions" : "",
        alias_mode ? ", sequential aliases" : undefined_volume ? ", UNDEFINED initial" : "",
        undefined_volume || alias_mode ? ", defined rendered slices verified" : " and untouched layers verified");
}
}
extern "C" VkInstance get_last_instance() { return context.instance; }
extern "C" void* get_real_proc(VkInstance instance, VkDevice device, const char* name) {
    if ((force_reclear || volume_mode) && !std::strcmp(name,"vkCreateRenderPass")) return reinterpret_cast<void*>(create_pass_reclear);
    if (device && context.gdpa) return reinterpret_cast<void*>(context.gdpa(device,name));
    return context.gipa ? reinterpret_cast<void*>(context.gipa(instance,name)) : nullptr;
}
int main(int argc, char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0); auto& p = context;
    for (int i = 1; i < argc; ++i) {
        force_reclear |= !std::strcmp(argv[i],"--reclear-negative-control");
        volume_mode |= !std::strcmp(argv[i],"--volume");
        undefined_volume |= !std::strcmp(argv[i],"--undefined-volume");
        alias_mode |= !std::strcmp(argv[i],"--alias-volume");
        query_mode |= !std::strcmp(argv[i],"--queries");
        wrong_query_slots |= !std::strcmp(argv[i],"--query-slot-negative-control");
    }
    volume_mode |= undefined_volume || alias_mode;
    if (query_mode && volume_mode) { puts("FAIL: query probe requires2D mode"); return 1; }
    try {
        auto library = dlopen("libvulkan.so",RTLD_NOW | RTLD_LOCAL); if (!library) throw std::runtime_error(dlerror());
        p.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr")); if (!p.gipa) throw std::runtime_error("no resolver");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
        check(p.get<PFN_vkCreateInstance>("vkCreateInstance")(&ci,nullptr,&p.instance),"instance");
        uint32_t count = 0; auto enumerate = p.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        check(enumerate(p.instance,&count,nullptr),"physical count"); std::vector<VkPhysicalDevice> devices(count);
        check(enumerate(p.instance,&count,devices.data()),"physical devices"); if (!count) throw std::runtime_error("no device");
        auto physical = devices[0];
        VkPhysicalDeviceProperties properties{}; p.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&properties);
        printf("Native device: %s; requested native API 1.0, %s, native multiview disabled\n",properties.deviceName,
            volume_mode ? "KHR_maintenance1 for 2D views of 3D images" : query_mode ? "occlusionQueryPrecise enabled" : "no device extensions/features");
        auto query = p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        query(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> queues(count); query(physical,&count,queues.data());
        uint32_t family = count; for (uint32_t i = 0; i < count; ++i) if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
        if (family == count) throw std::runtime_error("no graphics queue");
        query_timestamp_bits = queues[family].timestampValidBits;
        float priority = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        VkPhysicalDeviceFeatures enabled{};
        if (query_mode) {
            VkPhysicalDeviceFeatures supported{}; p.get<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical,&supported);
            if (!supported.occlusionQueryPrecise || !query_timestamp_bits) throw std::runtime_error("precise query/timestamp probe unsupported on device");
            enabled.occlusionQueryPrecise = VK_TRUE; dci.pEnabledFeatures = &enabled;
            statistics_queries = supported.pipelineStatisticsQuery != VK_FALSE;
            enabled.pipelineStatisticsQuery = statistics_queries ? VK_TRUE : VK_FALSE;
            printf("Pipeline statistics probe: %s\n",statistics_queries ? "enabled" : "SKIP (native feature unavailable)");
        }
        const char* maintenance1 = VK_KHR_MAINTENANCE_1_EXTENSION_NAME;
        if (volume_mode) { dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = &maintenance1; }
        check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dci,nullptr,&p.device),"device"); p.gdpa = p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        for (bool secondary : {false,true}) run(physical,family,secondary);
        if (!volume_mode) for (bool secondary : {false,true}) run(physical,family,secondary,true);
        p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr); p.device = VK_NULL_HANDLE;
        p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr); p.instance = VK_NULL_HANDLE;
        puts("Multiview barrier GPU failures: 0 (Vulkan 1.0 native API, native multiview disabled)"); return 0;
    } catch (const std::exception& e) {
        printf("FAIL: %s\n",e.what());
        // A timed-out queue remains pending; avoid native destruction of live
        // GPU resources during C++ static teardown on this failure path.
        if (p.pending) std::_Exit(1);
        if (p.device && p.gdpa) p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr);
        if (p.instance && p.gipa) p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr);
        return 1;
    }
}

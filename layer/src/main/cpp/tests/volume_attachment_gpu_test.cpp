#include "core/volume_attachment.h"
#include "core/multiview_commands.h"
#include "core/driver_loader.h"
#include "core/native_probe_resources.h"
#include "diagnostics/driver_post_shaders.h"
#include "diagnostics/volume_attachment_probe.h"
#include "diagnostics/volume_input_probe.h"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

using namespace vkfix;
using nativeprobe::check;
namespace {
struct Context {
    VkInstance instance{}; VkDevice device{};
    PFN_vkGetInstanceProcAddr gipa{}; PFN_vkGetDeviceProcAddr gdpa{};
    bool pending = false, retain_device = false;
    template<class T> T get(const char* name) {
        auto fn = reinterpret_cast<T>(gipa(instance,name));
        if (!fn) throw std::runtime_error(std::string("missing ") + name); return fn;
    }
    template<class T> T dev(const char* name) {
        auto fn = reinterpret_cast<T>(gdpa(device,name));
        if (!fn) throw std::runtime_error(std::string("missing ") + name); return fn;
    }
} context;
bool precise_queries = false;
struct QueryDraw { VkPipeline pipeline{}; VkPipelineLayout layout{}; VkBuffer vertices{}; };
QueryDraw query_draw(nativeprobe::Resources<Context>& gpu, VkRenderPass pass) {
    auto& p = context; QueryDraw draw;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,4};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &push;
    check(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&lci,nullptr,&draw.layout),"query layout");
    auto layout = draw.layout; gpu.objects.push_back([=,&p] { p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(p.device,layout,nullptr); });
    VkPipelineShaderStageCreateInfo stages[] = {
        gpu.shader_stage(VK_SHADER_STAGE_VERTEX_BIT,post_draw_vert,sizeof post_draw_vert),
        gpu.shader_stage(VK_SHADER_STAGE_FRAGMENT_BIT,post_draw_frag,sizeof post_draw_frag)};
    VkVertexInputBindingDescription binding{0,4,VK_VERTEX_INPUT_RATE_INSTANCE}; VkVertexInputAttributeDescription attribute{0,0,VK_FORMAT_R32_SFLOAT,0};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = vertex.vertexAttributeDescriptionCount = 1; vertex.pVertexBindingDescriptions = &binding; vertex.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,7,5,0,1}; VkRect2D scissor{{0,0},{7,5}};
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = viewport_state.scissorCount = 1; viewport_state.pViewports = &viewport; viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Count rasterized samples without altering the pixel-preservation markers.
    VkPipelineColorBlendAttachmentState blend{}; VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colors.attachmentCount = 1; colors.pAttachments = &blend;
    const VkDynamicState scissor_state = VK_DYNAMIC_STATE_SCISSOR;
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 1; dynamic.pDynamicStates = &scissor_state;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; ci.stageCount = 2; ci.pStages = stages;
    ci.pVertexInputState = &vertex; ci.pInputAssemblyState = &assembly; ci.pViewportState = &viewport_state; ci.pRasterizationState = &raster;
    ci.pMultisampleState = &samples; ci.pColorBlendState = &colors; ci.pDynamicState = &dynamic; ci.layout = draw.layout; ci.renderPass = pass;
    check(p.dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(p.device,{},1,&ci,nullptr,&draw.pipeline),"query pipeline");
    auto pipeline = draw.pipeline; gpu.objects.push_back([=,&p] { p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(p.device,pipeline,nullptr); });
    draw.vertices = gpu.buffer(4,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT).handle; return draw;
}
uint32_t marker(uint32_t x, uint32_t y, uint32_t z, uint32_t mip) {
    return 0xff000000u | ((x * 11 + mip * 19) & 255) | (((y * 13 + mip * 17) & 255) << 8) | (((z * 7 + mip * 23) & 255) << 16);
}
void run(VkPhysicalDevice physical, uint32_t family, bool wrong_area) {
    auto& p = context;
    std::unique_ptr<VolumeAttachment> a,b;
    nativeprobe::Resources<Context> gpu(p,physical,family);
    constexpr uint32_t mip0 = 16 * 12 * 96, mip1 = 8 * 6 * 48;
    auto upload = gpu.buffer((mip0+mip1)*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto output = gpu.buffer((mip0+mip1)*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto words = static_cast<uint32_t*>(upload.mapped);
    for (uint32_t mip = 0; mip < 2; ++mip) {
        const uint32_t w = 16 >> mip, h = 12 >> mip, d = 96 >> mip, base = mip ? mip0 : 0;
        for (uint32_t z = 0; z < d; ++z) for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x)
            words[base+(z*h+y)*w+x] = marker(x,y,z,mip);
    }
    VkMappedMemoryRange mapped{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; mapped.memory = upload.memory; mapped.size = VK_WHOLE_SIZE;
    check(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,1,&mapped),"upload flush");
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {16,12,96}; ci.mipLevels = 2; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto volume = gpu.create_image(ci);
    MultiviewImage original{volume,VK_IMAGE_TYPE_3D,2,1,96,ci.format,VK_SAMPLE_COUNT_1_BIT,ci.usage,16,12};
    VkImageViewCreateInfo logical{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; logical.image = volume;
    logical.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; logical.format = ci.format;
    logical.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,1,1,5,VK_REMAINING_ARRAY_LAYERS};
    VolumeAttachmentPlan plan;
    check(VolumeAttachmentPlan::capture(original,logical,{7,5},40,plan),"capture volume attachment");
    check(VolumeAttachment::create(p.device,physical,plan,a),"private attachment A");
    check(VolumeAttachment::create(p.device,physical,plan,b),"private attachment B");
    if (a->image().handle == b->image().handle) throw std::runtime_error("recordings share a mutable backing");
    // Retained resources are retired after their framebuffers, before their
    // original volume and native device. The command recording owns staging.
    gpu.objects.push_back([&] {
        MultiviewCommands::get().forget_device(p.device);
        MultiviewAttachments::get().forget_device(p.device);
        MultiviewRenderPasses::get().forget_device(p.device);
        ImageTypeCopy::get().forget_device(p.device); a.reset(); b.reset();
    });
    VkAttachmentDescription desc{}; desc.format = ci.format; desc.samples = VK_SAMPLE_COUNT_1_BIT;
    desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    desc.initialLayout = desc.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = rci.subpassCount = 1; rci.pAttachments = &desc; rci.pSubpasses = &subpass;
    VkRenderPass pass{}; VkResult result{};
    if (!MultiviewRenderPasses::get().create_fallback(p.device,&rci,nullptr,&pass,false,result,false,true))
        throw std::runtime_error("ordinary pass was not captured");
    check(result,"ordinary captured pass");
    gpu.objects.push_back([=,&p] {
        MultiviewRenderPasses::get().forget(p.device,pass);
        p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(p.device,pass,nullptr);
    });
    VkFramebuffer framebuffers[2]{}; VolumeAttachment* backings[]{a.get(),b.get()};
    for (unsigned i = 0; i < 2; ++i) {
        VkImageView view{}; check(backings[i]->view(0,40,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_VIEW_TYPE_2D_ARRAY,view),"whole layered view");
        VkImageCreateInfo backing_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; backing_info.imageType = VK_IMAGE_TYPE_2D;
        backing_info.format = ci.format; backing_info.extent = {7,5,1}; backing_info.mipLevels = 1; backing_info.arrayLayers = 40;
        backing_info.samples = VK_SAMPLE_COUNT_1_BIT; backing_info.usage = backings[i]->image().native_usage;
        MultiviewAttachments::get().remember_image(p.device,backings[i]->image().handle,backing_info);
        VkImageViewCreateInfo backing_view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; backing_view.image = backings[i]->image().handle;
        backing_view.format = ci.format; backing_view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        backing_view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,40};
        MultiviewAttachments::get().remember_view(p.device,view,backing_view);
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fci.renderPass = pass;
        fci.attachmentCount = 1; fci.pAttachments = &view; fci.width = 7; fci.height = 5; fci.layers = 40;
        check(p.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device,&fci,nullptr,&framebuffers[i]),"layered framebuffer");
        MultiviewAttachments::get().remember_framebuffer(p.device,framebuffers[i],fci);
        auto fb = framebuffers[i]; gpu.objects.push_back([=,&p] { p.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(p.device,fb,nullptr); });
    }
    const auto draw = query_draw(gpu,pass);
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qci.queryType = VK_QUERY_TYPE_OCCLUSION; qci.queryCount = 2;
    VkQueryPool queries{}; check(p.dev<PFN_vkCreateQueryPool>("vkCreateQueryPool")(p.device,&qci,nullptr,&queries),"query pool");
    gpu.objects.push_back([=,&p] { p.dev<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(p.device,queries,nullptr); });
    const auto cmd = gpu.begin(); check(ImageTypeCopy::get().begin(p.device,cmd,{}),"copy recording");
    p.dev<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(cmd,queries,0,2);
    auto barrier = p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    auto& commands = MultiviewCommands::get();
    VkCommandBufferBeginInfo command_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(commands.begin(p.device,cmd,{},command_begin,false,
        {p.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass"),p.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass"),barrier}),"ordinary command capture");
    VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; transition.image = volume;
    transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.newLayout = VK_IMAGE_LAYOUT_GENERAL; transition.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,2,0,1};
    barrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&transition);
    VkBufferImageCopy regions[2]{};
    for (unsigned mip = 0; mip < 2; ++mip) {
        regions[mip].bufferOffset = mip ? mip0*4 : 0; regions[mip].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,mip,0,1};
        regions[mip].imageExtent = {16u >> mip,12u >> mip,96u >> mip};
    }
    p.dev<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(cmd,upload.handle,volume,VK_IMAGE_LAYOUT_GENERAL,2,regions);
    check(a->initialize(cmd),"initialize A"); check(b->initialize(cmd),"initialize B");
    const VkRect2D areas[] = {{{1,1},{3,3}},{{4,0},{3,2}}};
    // Import both before either writes the volume, so an over-broad store from
    // B would overwrite A with stale pixels and be caught by full-volume checks.
    for (unsigned i = 0; i < 2; ++i) for (uint32_t layer : {0u,39u})
        check(backings[i]->import(cmd,VK_IMAGE_LAYOUT_GENERAL,areas[i],layer,1,VK_IMAGE_ASPECT_COLOR_BIT),"LOAD selected sparse slice run");
    for (unsigned i = 0; i < 2; ++i) {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = pass;
        begin.framebuffer = framebuffers[i]; begin.renderArea = areas[i];
        if (!commands.begin_pass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE)) throw std::runtime_error("ordinary pass bypassed replay");
        commands.capture(cmd,true,[=,&p] { return [=,&p](VkCommandBuffer native,uint32_t) {
            p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(native,VK_PIPELINE_BIND_POINT_GRAPHICS,draw.pipeline);
            p.dev<PFN_vkCmdSetScissor>("vkCmdSetScissor")(native,0,1,&areas[i]);
            uint32_t mode = 0; p.dev<PFN_vkCmdPushConstants>("vkCmdPushConstants")(native,draw.layout,VK_SHADER_STAGE_VERTEX_BIT,0,4,&mode);
            VkDeviceSize offset = 0; p.dev<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")(native,0,1,&draw.vertices,&offset); return VK_SUCCESS;
        }; });
        commands.begin_query(cmd,queries,i,precise_queries ? VK_QUERY_CONTROL_PRECISE_BIT : 0,
            p.dev<PFN_vkCmdBeginQuery>("vkCmdBeginQuery"),p.dev<PFN_vkCmdEndQuery>("vkCmdEndQuery"));
        auto capture_draw = [&] { commands.capture(cmd,false,[&p] { return [&p](VkCommandBuffer native,uint32_t) {
            p.dev<PFN_vkCmdDraw>("vkCmdDraw")(native,6,1,0,0); return VK_SUCCESS;
        }; }); };
        capture_draw();
        commands.capture(cmd,false,[=] { return [=](VkCommandBuffer native,uint32_t) {
            for (uint32_t layer : {0u,39u}) {
                const auto status = backings[i]->store(native,VK_IMAGE_LAYOUT_GENERAL,areas[i],layer,1,VK_IMAGE_ASPECT_COLOR_BIT);
                if (status != VK_SUCCESS) return status;
            }
            return VK_SUCCESS;
        }; },true);
        capture_draw();
        VkClearAttachment clear{}; clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; clear.colorAttachment = 0;
        if (i == 0) { clear.clearValue.color.float32[0] = 1; clear.clearValue.color.float32[3] = 1; }
        else { clear.clearValue.color.float32[1] = 1; clear.clearValue.color.float32[3] = 1; }
        VkClearRect rect{}; rect.rect = i ? VkRect2D{{4,0},{2,1}} : VkRect2D{{2,2},{1,1}};
        rect.baseArrayLayer = 39; rect.layerCount = 1;
        if (!commands.capture(cmd,false,[=,&p] { return [=,&p](VkCommandBuffer native,uint32_t view) {
            if (view || MultiviewCommands::replaying_multiview()) return VK_ERROR_INITIALIZATION_FAILED;
            p.dev<PFN_vkCmdClearAttachments>("vkCmdClearAttachments")(native,1,&clear,1,&rect); return VK_SUCCESS;
        }; })) throw std::runtime_error("ordinary command bypassed replay");
        commands.end_query(cmd,queries,i,p.dev<PFN_vkCmdEndQuery>("vkCmdEndQuery"));
        commands.end_pass(cmd); check(commands.error(cmd),"ordinary layered replay");
        for (uint32_t layer : {0u,39u}) {
            // Deliberately widen only B's *store* for the negative control.
            const VkRect2D area = wrong_area && i ? VkRect2D{{0,0},{7,5}} : areas[i];
            check(backings[i]->store(cmd,VK_IMAGE_LAYOUT_GENERAL,area,layer,1,VK_IMAGE_ASPECT_COLOR_BIT),"STORE selected sparse slice run");
        }
    }
    transition.oldLayout = VK_IMAGE_LAYOUT_GENERAL; transition.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    transition.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&transition);
    p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd,volume,VK_IMAGE_LAYOUT_GENERAL,output.handle,2,regions);
    gpu.host_barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    gpu.submit(cmd); gpu.invalidate(output);
    auto validate_queries = [&] {
        uint64_t counts[4]{};
        check(p.dev<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(p.device,queries,0,2,sizeof counts,counts,16,
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),"query results");
        if (!counts[1] || !counts[3] || !counts[0] || !counts[2] || (precise_queries && (counts[0] != 18 || counts[2] != 12)))
            throw std::runtime_error("query count lost samples across attachment transfers/pass splits");
    };
    validate_queries();
    auto validate = [&] {
        const auto pixels = static_cast<const uint32_t*>(output.mapped);
        for (uint32_t mip = 0; mip < 2; ++mip) {
            const uint32_t w = 16 >> mip, h = 12 >> mip, d = 96 >> mip, base = mip ? mip0 : 0;
            for (uint32_t z = 0; z < d; ++z) for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
                uint32_t expected = marker(x,y,z,mip);
                if (mip == 1 && z == 44 && x == 2 && y == 2) expected = 0xff0000ff;
                if (mip == 1 && z == 44 && x >= 4 && x < 6 && y == 0) expected = 0xff00ff00;
                if (pixels[base+(z*h+y)*w+x] != expected) {
                    char error[160]; snprintf(error,sizeof error,"mip%u xyz(%u,%u,%u): actual=%08x expected=%08x",mip,x,y,z,pixels[base+(z*h+y)*w+x],expected);
                    throw std::runtime_error(error);
                }
            }
        }
    };
    validate();
    // The same executable command buffer may be resubmitted: neither its
    // private image nor the staging resources may be retired at first submit.
    VkQueue queue{}; p.dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(p.device,family,0,&queue);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    check(p.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&submit,{}),"resubmit"); p.pending = true;
    check(p.dev<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue),"wait resubmission"); p.pending = false;
    gpu.invalidate(output); validate(); validate_queries();
    printf("PASS: native1.0 volume attachment backing, 40 layers, mip1/baseSlice5, sparse slices0/39, disjoint render areas, LOAD/STORE, %u guarded texels, queries spanning transfers (%s), resubmission\n",
        mip0+mip1,precise_queries ? "18/12 exact samples" : "available/nonzero");
}
}
extern "C" VkInstance get_last_instance() { return context.instance; }
extern "C" void* get_real_proc(VkInstance instance, VkDevice device, const char* name) {
    return device && context.gdpa ? reinterpret_cast<void*>(context.gdpa(device,name)) : reinterpret_cast<void*>(context.gipa(instance ? instance : context.instance,name));
}
int main(int argc, char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0); setenv("VULKAN_FIX_TEST_HIDE_MAINTENANCE1","1",1);
    const bool public_api = argc > 1 && !strncmp(argv[1],"--public",8);
    const bool dynamic = public_api && !strcmp(argv[1],"--public-dynamic");
    const bool sync2 = public_api && !strcmp(argv[1],"--public-sync2");
    if (public_api) {
        setenv("VULKAN_FIX_DRIVER_PATH","./libmultiview_test_driver.so",1);
        setenv("VULKAN_FIX_TEST_HIDE_MAINTENANCE2","1",1);
        setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE","1",1);
    }
    auto& p = context;
    try {
        auto library = dlopen(public_api ? (argc > 2 ? argv[2] : "./libvulkanfix-volume-public.so") : "./libmultiview_test_driver.so",RTLD_NOW | RTLD_LOCAL);
        if (!library) throw std::runtime_error(dlerror());
        p.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr"));
        if (!p.gipa) throw std::runtime_error("missing strict native provider");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = public_api ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
        VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance.pApplicationInfo = &app;
        check(p.get<PFN_vkCreateInstance>("vkCreateInstance")(&instance,nullptr,&p.instance),"instance");
        uint32_t count = 0; auto enumerate = p.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        check(enumerate(p.instance,&count,nullptr),"physical count"); std::vector<VkPhysicalDevice> devices(count);
        check(enumerate(p.instance,&count,devices.data()),"physical devices"); if (devices.empty()) throw std::runtime_error("no GPU");
        const auto physical = devices[0]; auto queues = p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        queues(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count); queues(physical,&count,families.data());
        uint32_t family = count; for (uint32_t i = 0; i < count; ++i) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
        if (family == count) throw std::runtime_error("no graphics queue");
        float priority = 1; VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
        VkPhysicalDeviceFeatures supported{}, enabled{};
        p.get<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical,&supported);
        precise_queries = supported.occlusionQueryPrecise; enabled.occlusionQueryPrecise = precise_queries;
        device.pEnabledFeatures = &enabled;
        VkPhysicalDeviceDynamicRenderingFeatures enable_dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        VkPhysicalDeviceSynchronization2Features enable_sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        const char* extension = dynamic ? VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME : VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
        if (dynamic || sync2) { device.enabledExtensionCount = 1; device.ppEnabledExtensionNames = &extension; }
        if (dynamic) { enable_dynamic.dynamicRendering = VK_TRUE; device.pNext = &enable_dynamic; }
        if (sync2) { enable_sync2.synchronization2 = VK_TRUE; device.pNext = &enable_sync2; }
        check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical,&device,nullptr,&p.device),"device");
        p.gdpa = p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        if (public_api) {
            const bool input = !strcmp(argv[1],"--public-input");
            if (input) nativeprobe::volume_input_probe(p,physical,family);
            else nativeprobe::volume_attachment_probe(p,physical,family,!strcmp(argv[1],"--public-template"),
                !strcmp(argv[1],"--public-secondary"),dynamic,sync2);
            printf("PASS: exported API %s, %s\n",argv[1],input ?
                "3D slice CLEAR, two subpasses, equivalent input descriptor view and16 pixels" :
                "3D render/LOAD/STORE, sampler3D, descriptor copies, event synchronization and384 guarded texels");
        } else run(physical,family,argc > 1 && !strcmp(argv[1],"--store-area-negative-control"));
        p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr); p.device = {};
        p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr); p.instance = {}; return 0;
    } catch (const std::exception& error) {
        printf("FAIL: %s\n",error.what()); if (p.pending) std::_Exit(1);
        if (p.device) p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr);
        if (p.instance) p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr); return 1;
    }
}

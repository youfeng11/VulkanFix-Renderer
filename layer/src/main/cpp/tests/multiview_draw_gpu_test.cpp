#include "core/driver_loader.h"
#include "core/multiview_commands.h"
#include "core/multiview_pipeline_layouts.h"
#include "core/multiview_pipelines.h"
#include "core/native_probe_resources.h"
#include "core/shader_module_cache.h"
#include "shader/constant_input_lowering.h"
#include "multiview_draw_shaders.h"
#include <cstdio>
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
        if (!fn) throw std::runtime_error(std::string("missing ") + name);
        return fn;
    }
    template<class T> T dev(const char* name) {
        auto fn = reinterpret_cast<T>(gdpa(device,name));
        if (!fn) throw std::runtime_error(std::string("missing ") + name);
        return fn;
    }
} context;
bool wrong_view = false, input_attachments = false, wrong_input = false, wrong_group_route = false, device_area = false;
bool ordinary = false;
using Resources = nativeprobe::Resources<Context>;
struct Slice { VkImage image; VkImageViewType type; VkImageSubresourceRange range; };
std::map<VkImageView,Slice> native_slices;
std::map<VkDescriptorSet,VkImageView> shadow_inputs;
VkImage expected_input_image{};
uint32_t active_view = 0, shadow_binds = 0;
VKAPI_ATTR VkResult VKAPI_CALL create_image_view(VkDevice device, const VkImageViewCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImageView* view) {
    auto status = context.dev<PFN_vkCreateImageView>("vkCreateImageView")(device,info,allocator,view);
    if (status == VK_SUCCESS) native_slices[*view] = {info->image,info->viewType,info->subresourceRange};
    return status;
}
VKAPI_ATTR void VKAPI_CALL update_descriptors(VkDevice device, uint32_t wc, const VkWriteDescriptorSet* writes,
    uint32_t cc, const VkCopyDescriptorSet* copies) {
    context.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(device,wc,writes,cc,copies);
    for (uint32_t i = 0; i < wc; ++i) if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
        if (writes[i].dstBinding != 1 || writes[i].dstArrayElement || writes[i].descriptorCount != 1)
            throw std::runtime_error("unexpected native input override");
        shadow_inputs[writes[i].dstSet] = writes[i].pImageInfo[0].imageView;
    }
}
VKAPI_ATTR void VKAPI_CALL bind_descriptors(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t dc, const uint32_t* offsets) {
    // A tiler can use attachment indices without consulting all descriptor
    // fields. Also inspect the actual native writes/binds so that a correct
    // readback on such a driver cannot hide a missing or wrong shadow slice.
    if (input_attachments) {
        const auto found = count == 1 ? shadow_inputs.find(sets[0]) : shadow_inputs.end();
        if (found == shadow_inputs.end()) throw std::runtime_error("input draw did not bind a shadow set");
        const auto slice = native_slices.find(found->second);
        if (slice == native_slices.end() || slice->second.image != expected_input_image ||
            slice->second.type != (ordinary ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D) ||
            slice->second.range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || slice->second.range.baseMipLevel != 1 || slice->second.range.levelCount != 1 ||
            slice->second.range.baseArrayLayer != (ordinary ? 2 : active_view+2) || slice->second.range.layerCount != (ordinary ? 7 : 1))
            throw std::runtime_error("shadow input binding has the wrong native image, mip, aspect or view layer");
        ++shadow_binds;
    }
    context.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cmd,point,layout,first,count,sets,dc,offsets);
}
// Fail deterministically if fallback internals send 1.1-only dependency flags
// to this API1.0 device, rather than relying on a permissive modern driver.
VKAPI_ATTR VkResult VKAPI_CALL create_render_pass(VkDevice device, const VkRenderPassCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* pass) {
    for (uint32_t i = 0; i < info->dependencyCount; ++i)
        if (info->pDependencies[i].dependencyFlags & (VK_DEPENDENCY_DEVICE_GROUP_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    return context.dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(device,info,allocator,pass);
}
VKAPI_ATTR void VKAPI_CALL begin_render_pass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    if (info->pNext) throw std::runtime_error("unexpected native render-pass begin pNext on the API1.0 route");
    context.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cmd,info,contents);
}
template<class T, class Destroy> std::shared_ptr<T> own(Resources& gpu, T value, Destroy destroy) {
    auto slot = std::make_shared<T>(value);
    gpu.objects.push_back([slot,destroy] { if (*slot) destroy(*slot); });
    return slot;
}
VKAPI_ATTR void VKAPI_CALL destroy_pipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    MultiviewPipelines::get().forget(device,pipeline);
    context.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device,pipeline,allocator);
}
void run(VkPhysicalDevice physical, uint32_t family, bool secondary) {
    auto& p = context;
    native_slices.clear(); shadow_inputs.clear(); shadow_binds = 0;
    Resources gpu(p,physical,family);
    auto output = gpu.buffer(8*4*4*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM; ici.extent = {4,4,1};
    ici.mipLevels = 1; ici.arrayLayers = 8; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image{}; check(p.dev<PFN_vkCreateImage>("vkCreateImage")(p.device,&ici,nullptr,&image),"image");
    own(gpu,image,[](VkImage h) { context.dev<PFN_vkDestroyImage>("vkDestroyImage")(context.device,h,nullptr); });
    VkMemoryRequirements mr{}; p.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(p.device,image,&mr);
    auto memory = gpu.allocate(mr,0); check(p.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(p.device,image,memory,0),"bind image");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = ici.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,1,7};
    VkImageView view{}; check(p.dev<PFN_vkCreateImageView>("vkCreateImageView")(p.device,&vi,nullptr,&view),"view");
    own(gpu,view,[](VkImageView h) { context.dev<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device,h,nullptr); });
    // Deliberately use a different base layer and a nonzero mip for inputs.
    // Reusing the output's slice/range must not accidentally pass this probe.
    VkImage input_image{}; VkImageView input_view{};
    auto input_ci = ici; input_ci.extent = {8,8,1}; input_ci.mipLevels = 2; input_ci.arrayLayers = 9;
    input_ci.usage = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto input_vi = vi; input_vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,1,1,2,7};
    if (input_attachments) {
        check(p.dev<PFN_vkCreateImage>("vkCreateImage")(p.device,&input_ci,nullptr,&input_image),"input image");
        own(gpu,input_image,[](VkImage h) { context.dev<PFN_vkDestroyImage>("vkDestroyImage")(context.device,h,nullptr); });
        p.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(p.device,input_image,&mr);
        auto input_memory = gpu.allocate(mr,0);
        check(p.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(p.device,input_image,input_memory,0),"bind input image");
        input_vi.image = input_image;
        check(p.dev<PFN_vkCreateImageView>("vkCreateImageView")(p.device,&input_vi,nullptr,&input_view),"input view");
        own(gpu,input_view,[](VkImageView h) { context.dev<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device,h,nullptr); });
    }
    expected_input_image = input_image;
    // Retire internal framebuffers/views/passes before their source image.
    gpu.objects.push_back([&p] {
        MultiviewCommands::get().forget_device(p.device); MultiviewPipelines::get().forget_device(p.device);
        MultiviewPipelineLayouts::get().forget_device(p.device); MultiviewDescriptors::get().forget_device(p.device);
        MultiviewAttachments::get().forget_device(p.device); MultiviewRenderPasses::get().forget_device(p.device);
        ShaderModuleCache::get().forget_device(p.device);
    });
    auto& attachments = MultiviewAttachments::get(); attachments.remember_image(p.device,image,ici); attachments.remember_view(p.device,view,vi);
    if (input_attachments) { attachments.remember_image(p.device,input_image,input_ci); attachments.remember_view(p.device,input_view,input_vi); }
    VkAttachmentDescription attachment{}; attachment.format = ici.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL; attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
    VkAttachmentDescription pass_attachments[] = {attachment,attachment};
    pass_attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; pass_attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    pass_attachments[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference input{1,VK_IMAGE_LAYOUT_GENERAL};
    if (input_attachments) { subpass.inputAttachmentCount = 1; subpass.pInputAttachments = &input; }
    const uint32_t mask = (1u<<2) | (1u<<4);
    VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO}; mv.subpassCount = 1; mv.pViewMasks = &mask;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rci.pNext = ordinary ? nullptr : &mv; rci.subpassCount = rci.attachmentCount = 1;
    rci.pSubpasses = &subpass; rci.pAttachments = pass_attachments; rci.attachmentCount = input_attachments ? 2 : 1;
    VkSubpassDependency dependency{0,0,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT | VK_DEPENDENCY_DEVICE_GROUP_BIT};
    if (input_attachments) { rci.dependencyCount = 1; rci.pDependencies = &dependency; }
    VkRenderPass pass{}; VkResult status{};
    if (!MultiviewRenderPasses::get().create_fallback(p.device,&rci,nullptr,&pass,false,status,wrong_group_route,ordinary)) throw std::runtime_error("missing fallback pass");
    check(status,"fallback pass");
    own(gpu,pass,[](VkRenderPass h) { context.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device,h,nullptr); });
    VkImageView framebuffer_views[] = {view,input_view};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fci.renderPass = pass; fci.attachmentCount = rci.attachmentCount; fci.pAttachments = framebuffer_views;
    fci.width = fci.height = 4; fci.layers = ordinary ? 7 : 1;
    VkFramebuffer fb{}; check(p.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device,&fci,nullptr,&fb),"framebuffer");
    own(gpu,fb,[](VkFramebuffer h) { context.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(context.device,h,nullptr); });
    attachments.remember_framebuffer(p.device,fb,fci);

    VkDescriptorSetLayoutBinding bindings[] = {{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sci.bindingCount = input_attachments ? 2 : 1; sci.pBindings = bindings;
    VkDescriptorSetLayout set_layout{}; check(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device,&sci,nullptr,&set_layout),"set layout");
    auto source_set_layout = own(gpu,set_layout,[](VkDescriptorSetLayout h) { context.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device,h,nullptr); });
    MultiviewDescriptors::get().remember_layout(p.device,set_layout,sci);
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT,0,16};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; lci.setLayoutCount = lci.pushConstantRangeCount = 1;
    lci.pSetLayouts = &set_layout; lci.pPushConstantRanges = &range;
    VkPipelineLayout layout{}; check(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&lci,nullptr,&layout),"pipeline layout");
    auto source_layout = own(gpu,layout,[](VkPipelineLayout h) { context.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device,h,nullptr); });
    check(MultiviewPipelineLayouts::get().remember(p.device,layout,lci),"private pipeline layout");
    VkPhysicalDeviceProperties properties{}; p.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&properties);
    uint32_t offset = uint32_t(std::max<VkDeviceSize>(256,properties.limits.minUniformBufferOffsetAlignment));
    auto uniform = gpu.buffer(offset+16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const float tint[] = {0,17.f/255,0,0}; std::memcpy(static_cast<char*>(uniform.mapped)+offset,tint,sizeof(tint));
    VkMappedMemoryRange flush{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; flush.memory = uniform.memory; flush.size = VK_WHOLE_SIZE;
    check(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,1,&flush),"uniform flush");
    const uint32_t set_count = input_attachments && secondary ? 2 : 1;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,set_count},{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,set_count}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = set_count; pci.poolSizeCount = input_attachments ? 2 : 1; pci.pPoolSizes = sizes;
    VkDescriptorPool pool{}; check(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&pci,nullptr,&pool),"descriptor pool");
    own(gpu,pool,[](VkDescriptorPool h) { context.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device,h,nullptr); });
    MultiviewDescriptors::get().remember_pool(p.device,pool,pci);
    VkDescriptorSetLayout set_layouts[] = {set_layout,set_layout}; VkDescriptorSet sets[2]{};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = pool; ai.descriptorSetCount = set_count; ai.pSetLayouts = set_layouts;
    check(MultiviewDescriptors::get().allocate(p.device,&ai,sets,p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")),"descriptor allocation");
    VkDescriptorSet set = sets[set_count-1];
    VkDescriptorBufferInfo buffer{uniform.handle,0,16}; VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets[0]; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; write.pBufferInfo = &buffer;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,1,&write,0,nullptr); MultiviewDescriptors::get().update(p.device,1,&write,0,nullptr);
    if (input_attachments) {
        VkDescriptorImageInfo image_info{VK_NULL_HANDLE,input_view,VK_IMAGE_LAYOUT_GENERAL};
        write.dstBinding = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; write.pImageInfo = &image_info;
        p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,1,&write,0,nullptr); MultiviewDescriptors::get().update(p.device,1,&write,0,nullptr);
        if (set_count == 2) {
            VkCopyDescriptorSet copies[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                copies[i].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET; copies[i].srcSet = sets[0]; copies[i].dstSet = set;
                copies[i].srcBinding = copies[i].dstBinding = i; copies[i].descriptorCount = 1;
            }
            p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,0,nullptr,2,copies); MultiviewDescriptors::get().update(p.device,0,nullptr,2,copies);
        }
    }

    const uint32_t* words[] = {multiview_draw_vert,input_attachments ? multiview_input_frag : multiview_draw_frag};
    const size_t bytes[] = {sizeof(multiview_draw_vert),input_attachments ? sizeof(multiview_input_frag) : sizeof(multiview_draw_frag)};
    std::shared_ptr<VkShaderModule> modules[2]; VkPipelineShaderStageCreateInfo stages[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        std::vector<uint32_t> code(words[i],words[i]+bytes[i]/4); std::string error;
        if (lower_view_index(code,0,error) == ConstantInputLowering::Invalid) throw std::runtime_error(error);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = code.size()*4; ci.pCode = code.data();
        VkShaderModule module{}; check(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&ci,nullptr,&module),"default shader");
        modules[i] = own(gpu,module,[](VkShaderModule h) { context.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device,h,nullptr); });
        ci.codeSize = bytes[i]; ci.pCode = words[i]; ShaderModuleCache::get().remember(p.device,module,&ci,false,true);
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = module; stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,4,4,0,1}; VkRect2D scissor{{0,0},{4,4}};
    if (device_area) scissor = {{1,1},{2,2}};
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport_state.viewportCount = viewport_state.scissorCount = 1;
    viewport_state.pViewports = &viewport; viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; colors.attachmentCount = 1; colors.pAttachments = &blend;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gi.stageCount = 2; gi.pStages = stages;
    gi.pVertexInputState = &vertex; gi.pInputAssemblyState = &assembly; gi.pViewportState = &viewport_state; gi.pRasterizationState = &raster;
    gi.pMultisampleState = &samples; gi.pColorBlendState = &colors; gi.layout = layout; gi.renderPass = pass; gi.basePipelineIndex = -1;
    VkPipeline pipeline{};
    if (!MultiviewPipelines::get().create(p.device,VK_NULL_HANDLE,1,&gi,nullptr,&pipeline,status,p.dev<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines"),
        destroy_pipeline,p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule"),p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule"),0))
        throw std::runtime_error("pipeline was not lowered");
    check(status,"view pipeline variants");
    own(gpu,pipeline,[](VkPipeline h) { destroy_pipeline(context.device,h,nullptr); });
    auto variants = MultiviewPipelines::get().acquire(p.device,pipeline);
    if (!variants || (ordinary ? (!variants->for_view(0) || variants->for_view(2)) :
        (!variants->for_view(2) || !variants->for_view(4) || variants->for_view(2) == variants->for_view(4) || variants->for_view(0))))
        throw std::runtime_error("invalid eager variant identities");
    for (auto module : modules) {
        ShaderModuleCache::get().forget(p.device,*module); p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(p.device,*module,nullptr); *module = VK_NULL_HANDLE;
    }
    MultiviewDescriptors::get().forget_layout(p.device,set_layout);
    p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(p.device,set_layout,nullptr); *source_set_layout = VK_NULL_HANDLE;

    auto cmd = gpu.begin(); auto barrier = p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; transition.image = image;
    transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.newLayout = VK_IMAGE_LAYOUT_GENERAL; transition.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,8};
    barrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&transition);
    VkClearColorValue zero{}; p.dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&transition.subresourceRange);
    transition.oldLayout = VK_IMAGE_LAYOUT_GENERAL; transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; transition.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&transition);
    if (input_attachments) {
        auto t = transition; t.image = input_image; t.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.newLayout = VK_IMAGE_LAYOUT_GENERAL; t.srcAccessMask = 0; t.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        t.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,1,1,0,9};
        barrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&t);
        for (uint32_t layer = 0; layer < 9; ++layer) {
            // The negative control spoils only view4's input; its output
            // must differ even though its ViewIndex and UBO are correct.
            const uint32_t marker = wrong_input && layer == 6 ? 4 : layer;
            VkClearColorValue value{}; value.float32[0] = float(marker+7)/255;
            value.float32[1] = float(2*marker+3)/255; value.float32[2] = float(3*marker+5)/255; value.float32[3] = 1;
            auto range = t.subresourceRange; range.baseArrayLayer = layer; range.layerCount = 1;
            p.dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,input_image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
        }
        t.oldLayout = VK_IMAGE_LAYOUT_GENERAL; t.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; t.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&t);
    }
    auto& commands = MultiviewCommands::get();
    MultiviewCommands::Callbacks callbacks{begin_render_pass,p.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass"),barrier};
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(commands.begin(p.device,cmd,VK_NULL_HANDLE,begin,false,callbacks),"primary capture");
    VkClearValue clear{}; clear.color.float32[0] = clear.color.float32[3] = 1;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass = pass; rp.framebuffer = fb; rp.renderArea.extent = {4,4}; rp.clearValueCount = 1; rp.pClearValues = &clear;
    VkRect2D selected_area{{1,1},{2,2}};
    VkDeviceGroupRenderPassBeginInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO};
    group.deviceMask = group.deviceRenderAreaCount = 1; group.pDeviceRenderAreas = &selected_area;
    if (device_area) rp.pNext = &group;
    commands.begin_pass(cmd,&rp,secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
    selected_area = {{0,0},{4,4}}; group.deviceMask = 0; // Capture must own group parameters too.
    auto body = [&](VkCommandBuffer target) {
        auto bind = p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
        commands.capture(target,true,[=] { return [=](VkCommandBuffer native,uint32_t selected) {
            active_view = selected;
            bind(native,VK_PIPELINE_BIND_POINT_GRAPHICS,variants->for_view(wrong_view ? 2 : selected)); return VK_SUCCESS;
        }; });
        commands.bind_descriptors(target,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,1,&offset,bind_descriptors);
        float push[] = {0,0,33.f/255,0};
        commands.push_constants(target,layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push,p.dev<PFN_vkCmdPushConstants>("vkCmdPushConstants"));
        push[2] = 0; offset = 0; // Host source arrays must be owned by capture.
        auto draw = p.dev<PFN_vkCmdDraw>("vkCmdDraw"); commands.capture(target,false,[=] { return [=](VkCommandBuffer native,uint32_t) { draw(native,3,1,0,0); return VK_SUCCESS; }; });
    };
    if (secondary) {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; ci.queueFamilyIndex = family;
        VkCommandPool pool{}; check(p.dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(p.device,&ci,nullptr,&pool),"secondary pool");
        own(gpu,pool,[](VkCommandPool h) { context.dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device,h,nullptr); });
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = pool; ai.commandBufferCount = 1; ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        VkCommandBuffer child{}; check(p.dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(p.device,&ai,&child),"secondary allocation");
        VkCommandBufferInheritanceInfo inherited{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}; inherited.renderPass = pass;
        begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT; begin.pInheritanceInfo = &inherited;
        check(p.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(child,&begin),"secondary begin"); check(commands.begin(p.device,child,pool,begin,false,callbacks),"secondary capture");
        body(child); check(p.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(child),"secondary end"); commands.execute(cmd,1,&child);
    } else body(cmd);
    MultiviewPipelineLayouts::get().forget(p.device,layout);
    p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(p.device,layout,nullptr); *source_layout = VK_NULL_HANDLE;
    commands.end_pass(cmd); check(commands.error(cmd),"replay");
    if (input_attachments && shadow_binds != (ordinary ? 1 : 2)) throw std::runtime_error("not all views bound a verified native input slice");
    for (uint32_t layer = 0; layer < 8; ++layer) {
        transition.oldLayout = (ordinary ? layer >= 1 : layer == 3 || layer == 5) ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        transition.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; transition.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; transition.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        transition.subresourceRange.baseArrayLayer = layer; transition.subresourceRange.layerCount = 1;
        barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&transition);
    }
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,8}; copy.imageExtent = {4,4,1};
    p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
    gpu.host_barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT); gpu.submit(cmd); gpu.invalidate(output);
    const auto pixels = static_cast<const uint8_t*>(output.mapped);
    for (uint32_t layer = 0; layer < 8; ++layer) for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        uint8_t expected[4]{};
        if (ordinary && layer >= 1) { expected[0] = 255; expected[3] = 255; }
        const bool drawn = (ordinary ? layer == 1 : layer == 3 || layer == 5) && (!device_area || (pixel%4 >= 1 && pixel%4 < 3 && pixel/4 >= 1 && pixel/4 < 3));
        if (drawn) { const auto view = ordinary ? 0u : layer-1; expected[0] = uint8_t(view); expected[1] = uint8_t(view+17); expected[2] = 33; expected[3] = 255; }
        if (input_attachments && drawn) {
            const uint32_t source_layer = ordinary ? 2 : layer+1;
            expected[0] += uint8_t(source_layer+7); expected[1] += uint8_t(2*source_layer+3); expected[2] += uint8_t(3*source_layer+5);
        }
        const auto actual = pixels+(layer*16+pixel)*4;
        if (std::memcmp(actual,expected,4)) {
            char text[192]; std::snprintf(text,sizeof(text),"layer=%u pixel=%u got=(%u,%u,%u,%u), expected=(%u,%u,%u,%u)",layer,pixel,
                actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]); throw std::runtime_error(text);
        }
    }
    printf("PASS: %s %s, dynamic UBO, owned pushes, %s, %s, destroyed source shaders/set layout/pipeline layout,128 pixels verified\n",
        secondary ? "secondary" : "primary", ordinary ? "ordinary layered VS/FS,7 layers,one variant" : "eager VS/FS ViewIndex=(2,4)", input_attachments ? (secondary ? "copied input attachment at mip1/baseLayer2" : "written input attachment at mip1/baseLayer2") : "no input attachment",
        device_area ? "owned singleton device render area" : "full render area");
}
}
extern "C" VkInstance get_last_instance() { return context.instance; }
extern "C" void* get_real_proc(VkInstance instance, VkDevice device, const char* name) {
    if (device && !std::strcmp(name,"vkCreateRenderPass")) return reinterpret_cast<void*>(create_render_pass);
    if (device && !std::strcmp(name,"vkCreateImageView")) return reinterpret_cast<void*>(create_image_view);
    if (device && !std::strcmp(name,"vkUpdateDescriptorSets")) return reinterpret_cast<void*>(update_descriptors);
    return device && context.gdpa ? reinterpret_cast<void*>(context.gdpa(device,name)) : reinterpret_cast<void*>(context.gipa(instance,name));
}
int main(int argc, char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0); wrong_view = argc > 1 && !std::strcmp(argv[1],"--wrong-view-negative-control");
    wrong_input = argc > 1 && !std::strcmp(argv[1],"--input-source-negative-control");
    wrong_group_route = argc > 1 && !std::strcmp(argv[1],"--device-group-route-negative-control");
    device_area = argc > 1 && !std::strcmp(argv[1],"--input-device-area");
    ordinary = argc > 1 && (!std::strcmp(argv[1],"--ordinary") || !std::strcmp(argv[1],"--ordinary-input"));
    input_attachments = wrong_input || wrong_group_route || device_area || (argc > 1 &&
        (!std::strcmp(argv[1],"--input-attachments") || !std::strcmp(argv[1],"--ordinary-input")));
    auto& p = context;
    try {
        auto library = dlopen("libvulkan.so",RTLD_NOW | RTLD_LOCAL); if (!library) throw std::runtime_error(dlerror());
        p.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr")); if (!p.gipa) throw std::runtime_error("no resolver");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
        check(p.get<PFN_vkCreateInstance>("vkCreateInstance")(&ci,nullptr,&p.instance),"instance");
        auto enumerate = p.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices"); uint32_t count = 0; check(enumerate(p.instance,&count,nullptr),"devices");
        std::vector<VkPhysicalDevice> devices(count); check(enumerate(p.instance,&count,devices.data()),"devices"); if (!count) throw std::runtime_error("no device");
        auto physical = devices[0]; VkPhysicalDeviceProperties props{}; p.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&props);
        auto queues = p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties"); queues(physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count); queues(physical,&count,families.data()); uint32_t family = count;
        for (uint32_t i = 0; i < count; ++i) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
        if (family == count) throw std::runtime_error("no graphics queue");
        float priority = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dci,nullptr,&p.device),"device"); p.gdpa = p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        printf("Native device: %s, API1.0, no enabled device extensions/features/native multiview\n",props.deviceName);
        for (bool secondary : {false,true}) run(physical,family,secondary);
        p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr); p.device = VK_NULL_HANDLE;
        p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr); p.instance = VK_NULL_HANDLE; return 0;
    } catch (const std::exception& e) {
        printf("FAIL: %s\n",e.what()); if (p.pending) std::_Exit(1);
        if (p.device) p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr);
        if (p.instance) p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr); return 1;
    }
}

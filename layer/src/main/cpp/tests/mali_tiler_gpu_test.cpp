#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void success(VkResult r, const char *message) {
    if (r != VK_SUCCESS) {
        printf("VkResult=%d: %s\n", r, message);
        throw std::runtime_error(message);
    }
}
static const char *vertex_source = R"(
OpCapability Shader
OpCapability DrawParameters
OpExtension "SPV_KHR_shader_draw_parameters"
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %vertex %draw %position %color
OpDecorate %vertex BuiltIn VertexIndex
OpDecorate %draw BuiltIn DrawIndex
OpDecorate %position BuiltIn Position
OpDecorate %color Location 0
OpDecorate %Push Block
OpMemberDecorate %Push 0 Offset 0
OpMemberDecorate %Push 1 Offset 4
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%in = OpTypePointer Input %int
%out = OpTypePointer Output %v4
%Push = OpTypeStruct %float %float
%pushptr = OpTypePointer PushConstant %Push
%floatptr = OpTypePointer PushConstant %float
%push = OpVariable %pushptr PushConstant
%vertex = OpVariable %in Input
%draw = OpVariable %in Input
%position = OpVariable %out Output
%color = OpVariable %out Output
%zero = OpConstant %int 0
%one = OpConstant %int 1
%two = OpConstant %int 2
%three = OpConstant %int 3
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%minus = OpConstant %float -1
%fthree = OpConstant %float 3
%bias = OpConstant %float 0.05
%main = OpFunction %void None %fn
%entry = OpLabel
%v = OpLoad %int %vertex
%i = OpSMod %int %v %three
%is1 = OpIEqual %bool %i %one
%is2 = OpIEqual %bool %i %two
%x = OpSelect %float %is1 %fthree %minus
%y = OpSelect %float %is2 %fthree %minus
%d = OpLoad %int %draw
%first = OpIEqual %bool %d %zero
%zp = OpAccessChain %floatptr %push %zero
%bp = OpAccessChain %floatptr %push %one
%z = OpLoad %float %zp
%b = OpLoad %float %bp
%closer = OpFSub %float %z %bias
%depth = OpSelect %float %first %z %closer
%pos = OpCompositeConstruct %v4 %x %y %depth %fone
OpStore %position %pos
%r = OpSelect %float %first %fone %fzero
%g = OpSelect %float %first %fzero %fone
%c = OpCompositeConstruct %v4 %r %g %b %fone
OpStore %color %c
OpReturn
OpFunctionEnd
)";
static const char *fragment_source = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %color %pixel
OpExecutionMode %main OriginUpperLeft
OpDecorate %color Location 0
OpDecorate %pixel Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%in = OpTypePointer Input %v4
%out = OpTypePointer Output %v4
%color = OpVariable %in Input
%pixel = OpVariable %out Output
%main = OpFunction %void None %fn
%entry = OpLabel
%c = OpLoad %v4 %color
OpStore %pixel %c
OpReturn
OpFunctionEnd
)";
struct Harness {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkCommandPool pool{};
    VkFence fence{};
    VkQueryPool query{};
    VkRenderPass pass{};
    VkFramebuffer framebuffer{};
    struct Image {
        VkImage image;
        VkDeviceMemory memory;
        VkImageView view;
        VkImageAspectFlags aspect;
    };
    struct Buffer {
        VkBuffer buffer;
        VkDeviceMemory memory;
        void *mapped;
    };
    std::vector<Image> images;
    std::vector<Buffer> buffers;
    std::vector<VkShaderModule> shaders;
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p{};
        vkGetPhysicalDeviceMemoryProperties(physical, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("memory type");
    }
    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b{};
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = usage;
        success(vkCreateBuffer(device, &ci, nullptr, &b.buffer), "buffer");
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device, b.buffer, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex =
            memory_type(mr.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        success(vkAllocateMemory(device, &ai, nullptr, &b.memory), "buffer memory");
        success(vkBindBufferMemory(device, b.buffer, b.memory, 0), "bind buffer");
        success(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "map buffer");
        buffers.push_back(b);
        return b;
    }
    Image image(VkFormat format, VkImageAspectFlags aspect, VkImageUsageFlags usage) {
        Image im{};
        im.aspect = aspect;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {64, 64, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT;
        success(vkCreateImage(device, &ci, nullptr, &im.image), "image");
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device, im.image, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        success(vkAllocateMemory(device, &ai, nullptr, &im.memory), "image memory");
        success(vkBindImageMemory(device, im.image, im.memory, 0), "bind image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        success(vkCreateImageView(device, &vi, nullptr, &im.view), "image view");
        images.push_back(im);
        return im;
    }
    VkShaderModule shader(const char *source) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        std::vector<uint32_t> words;
        require(tools.Assemble(source, &words) && tools.Validate(words),
                "shader assembly and validation");
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = words.size() * 4;
        ci.pCode = words.data();
        VkShaderModule module{};
        success(vkCreateShaderModule(device, &ci, nullptr, &module), "shader");
        shaders.push_back(module);
        return module;
    }
    void transition(VkCommandBuffer cmd, const Image &im, VkImageLayout oldLayout,
                    VkImageLayout newLayout) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                              ? 0
                              : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im.image;
        b.subresourceRange = {im.aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    ~Harness() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (pool)
                vkDestroyCommandPool(device, pool, nullptr);
            if (query)
                vkDestroyQueryPool(device, query, nullptr);
            if (fence)
                vkDestroyFence(device, fence, nullptr);
            if (pipeline)
                vkDestroyPipeline(device, pipeline, nullptr);
            if (framebuffer)
                vkDestroyFramebuffer(device, framebuffer, nullptr);
            if (pass)
                vkDestroyRenderPass(device, pass, nullptr);
            if (layout)
                vkDestroyPipelineLayout(device, layout, nullptr);
            for (auto s : shaders)
                vkDestroyShaderModule(device, s, nullptr);
            for (auto b : buffers) {
                vkUnmapMemory(device, b.memory);
                vkDestroyBuffer(device, b.buffer, nullptr);
                vkFreeMemory(device, b.memory, nullptr);
            }
            for (auto im : images) {
                vkDestroyImageView(device, im.view, nullptr);
                vkDestroyImage(device, im.image, nullptr);
                vkFreeMemory(device, im.memory, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
    void run(bool direct, bool with_query, bool stress, unsigned frames, unsigned legacy,
             bool general, bool bsl) {
        setenv("VULKAN_FIX_EMULATE_DYNAMIC_RENDERING", "force", 1);
        setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        success(vkCreateInstance(&ici, nullptr, &instance), "instance");
        uint32_t count = 1;
        success(vkEnumeratePhysicalDevices(instance, &count, &physical), "physical device");
        VkPhysicalDeviceFeatures f{};
        vkGetPhysicalDeviceFeatures(physical, &f);
        require(f.multiDrawIndirect, "native multi-draw support");
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = 0;
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            ++family;
        require(family < count, "graphics queue");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkPhysicalDeviceShaderDrawParametersFeatures params{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
        params.shaderDrawParameters = VK_TRUE;
        VkPhysicalDeviceFeatures enabled{};
        enabled.multiDrawIndirect = VK_TRUE;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &params;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        dci.pEnabledFeatures = &enabled;
        const char *rp2extension = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
        if (legacy == 2) {
            dci.enabledExtensionCount = 1;
            dci.ppEnabledExtensionNames = &rp2extension;
        }
        success(vkCreateDevice(physical, &dci, nullptr, &device), "device");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.queueFamilyIndex = family;
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        success(vkCreateCommandPool(device, &cp, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        success(vkAllocateCommandBuffers(device, &ai, &cmd), "commands");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        success(vkCreateFence(device, &fi, nullptr, &fence), "fence");
        VkQueryPoolCreateInfo qc{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qc.queryType = VK_QUERY_TYPE_OCCLUSION;
        qc.queryCount = 1;
        success(vkCreateQueryPool(device, &qc, nullptr, &query), "query pool");
        auto color = image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        auto depth = image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        Image secondColor{};
        if (bsl)
            secondColor = image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        const VkImageLayout colorLayout =
            general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const VkImageLayout depthLayout =
            general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkImageLayout finalLayout =
            general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (legacy) {
            std::vector<VkAttachmentDescription> attachments(bsl ? 3 : 2);
            for (auto &a : attachments) {
                a.samples = VK_SAMPLE_COUNT_1_BIT;
                a.loadOp = bsl ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
                a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                a.finalLayout = finalLayout;
            }
            attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
            attachments[0].initialLayout = colorLayout;
            attachments[1].format = VK_FORMAT_R8G8B8A8_UNORM;
            attachments[1].initialLayout = colorLayout;
            attachments.back().format = VK_FORMAT_D32_SFLOAT;
            attachments.back().initialLayout = depthLayout;
            VkAttachmentReference colorRefs[] = {{0, colorLayout}, {1, colorLayout}};
            VkAttachmentReference depthRef{bsl ? 2u : 1u, depthLayout};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = bsl ? 2 : 1;
            subpass.pColorAttachments = colorRefs;
            subpass.pDepthStencilAttachment = &depthRef;
            VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            ci.attachmentCount = (uint32_t)attachments.size();
            ci.pAttachments = attachments.data();
            ci.subpassCount = 1;
            ci.pSubpasses = &subpass;
            if (legacy == 1) {
                auto create =
                    (PFN_vkCreateRenderPass)vkGetDeviceProcAddr(device, "vkCreateRenderPass");
                require(create && create == &vkCreateRenderPass, "legacy create proc interception");
                success(create(device, &ci, nullptr, &pass), "legacy render pass");
            } else {
                VkAttachmentDescription2 a2[2]{};
                for (unsigned i = 0; i < 2; ++i) {
                    const auto &a = attachments[i];
                    a2[i] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
                             nullptr,
                             a.flags,
                             a.format,
                             a.samples,
                             a.loadOp,
                             a.storeOp,
                             a.stencilLoadOp,
                             a.stencilStoreOp,
                             a.initialLayout,
                             a.finalLayout};
                }
                VkAttachmentReference2 c2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0,
                                          colorLayout, VK_IMAGE_ASPECT_COLOR_BIT};
                VkAttachmentReference2 d2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1,
                                          depthLayout, VK_IMAGE_ASPECT_DEPTH_BIT};
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
                auto create = (PFN_vkCreateRenderPass2KHR)vkGetDeviceProcAddr(
                    device, "vkCreateRenderPass2KHR");
                require(create, "renderpass2 proc");
                success(create(device, &ci2, nullptr, &pass), "renderpass2");
            }
            std::vector<VkImageView> views{color.view};
            if (bsl)
                views.push_back(secondColor.view);
            views.push_back(depth.view);
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = pass;
            fb.attachmentCount = (uint32_t)views.size();
            fb.pAttachments = views.data();
            fb.width = fb.height = 64;
            fb.layers = 1;
            success(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer), "legacy framebuffer");
        }
        auto readback = buffer(64 * 64 * (bsl ? 12 : 8), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto indirect =
            buffer(2 * sizeof(VkDrawIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        VkDrawIndirectCommand indirect_data[2] = {{stress ? 196608u : 3u, 1, 0, 0},
                                                  {stress ? 196608u : 3u, 1, 0, 0}};
        memcpy(indirect.mapped, indirect_data, sizeof(indirect_data));
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
        VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lc.pushConstantRangeCount = 1;
        lc.pPushConstantRanges = &range;
        success(vkCreatePipelineLayout(device, &lc, nullptr, &layout), "layout");
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto &stage : stages) {
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = shader(vertex_source);
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        std::string fragment = fragment_source;
        if (bsl) {
            auto at = fragment.find("%color %pixel");
            fragment.replace(at, strlen("%color %pixel"), "%color %pixel %pixel2");
            at = fragment.find("OpDecorate %pixel Location 0");
            fragment.insert(at, "OpDecorate %pixel2 Location 1\n");
            at = fragment.find("%main = OpFunction");
            fragment.insert(at, "%pixel2 = OpVariable %out Output\n");
            at = fragment.find("OpStore %pixel %c");
            fragment.insert(at, "OpStore %pixel2 %c\n");
        }
        stages[1].module = shader(fragment.c_str());
        VkPipelineVertexInputStateCreateInfo vi{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, 64, 64, 0, 1};
        VkRect2D scissor{{0, 0}, {64, 64}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        vp.pViewports = &viewport;
        vp.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blends[2]{};
        blends[0].colorWriteMask = blends[1].colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo bs{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        bs.attachmentCount = bsl ? 2 : 1;
        bs.pAttachments = blends;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo pc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pc.pNext = legacy ? nullptr : &rendering;
        pc.renderPass = pass;
        pc.stageCount = 2;
        pc.pStages = stages;
        pc.pVertexInputState = &vi;
        pc.pInputAssemblyState = &ia;
        pc.pViewportState = &vp;
        pc.pRasterizationState = &rs;
        pc.pMultisampleState = &ms;
        pc.pDepthStencilState = &ds;
        pc.pColorBlendState = &bs;
        pc.layout = layout;
        success(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline),
                "graphics pipeline");
        for (unsigned frame = 0; frame < frames; ++frame) {
            success(vkResetFences(device, 1, &fence), "reset fence");
            success(vkResetCommandPool(device, pool, 0), "reset pool");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            success(vkBeginCommandBuffer(cmd, &bi), "begin");
            transition(cmd, color,
                       frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       colorLayout);
            transition(cmd, depth,
                       frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       depthLayout);
            if (bsl) {
                transition(cmd, secondColor,
                           frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                           colorLayout);
                VkClearColorValue clear{{0, 0, 1, 1}};
                VkClearDepthStencilValue clearDepth{1, 0};
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd, color.image, colorLayout, &clear, 1, &range);
                vkCmdClearColorImage(cmd, secondColor.image, colorLayout, &clear, 1, &range);
                range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                vkCmdClearDepthStencilImage(cmd, depth.image, depthLayout, &clearDepth, 1, &range);
                transition(cmd, color, colorLayout, colorLayout);
                transition(cmd, secondColor, colorLayout, colorLayout);
                transition(cmd, depth, depthLayout, depthLayout);
            }
            vkCmdResetQueryPool(cmd, query, 0, 1);
            VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ca.imageView = color.view;
            ca.imageLayout = colorLayout;
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ca.clearValue.color.float32[2] = ca.clearValue.color.float32[3] = 1;
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = depth.view;
            da.imageLayout = depthLayout;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil.depth = 1;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea.extent = {64, 64};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &ca;
            ri.pDepthAttachment = &da;
            VkClearValue clears[] = {ca.clearValue, da.clearValue};
            VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rb.renderPass = pass;
            rb.framebuffer = framebuffer;
            rb.renderArea = ri.renderArea;
            rb.clearValueCount = 2;
            rb.pClearValues = clears;
            if (legacy == 2) {
                VkSubpassBeginInfo sub{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
                auto begin = (PFN_vkCmdBeginRenderPass2KHR)vkGetDeviceProcAddr(
                    device, "vkCmdBeginRenderPass2KHR");
                begin(cmd, &rb, &sub);
            } else if (legacy) {
                auto begin =
                    (PFN_vkCmdBeginRenderPass)vkGetDeviceProcAddr(device, "vkCmdBeginRenderPass");
                begin(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
            } else
                vkCmdBeginRendering(cmd, &ri);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (with_query)
                vkCmdBeginQuery(cmd, query, 0, 0);
            for (unsigned draw = 0; draw < 12; ++draw) {
                float push[2] = {draw ? 0.7f : 0.2f, draw ? 1.0f : 0.25f};
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
                if (direct)
                    vkCmdDraw(cmd, draw ? 262140 : 3, 1, 0, 0);
                else
                    vkCmdDrawIndirect(cmd, indirect.buffer, 0, 2, sizeof(VkDrawIndirectCommand));
            }
            if (with_query) {
                vkCmdEndQuery(cmd, query, 0);
                vkCmdDrawIndirect(cmd, indirect.buffer, 0, 2, sizeof(VkDrawIndirectCommand));
            }
            if (legacy == 2) {
                VkSubpassEndInfo end{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
                auto finish = (PFN_vkCmdEndRenderPass2KHR)vkGetDeviceProcAddr(
                    device, "vkCmdEndRenderPass2KHR");
                finish(cmd, &end);
            } else if (legacy)
                vkCmdEndRenderPass(cmd);
            else
                vkCmdEndRendering(cmd);
            transition(cmd, color, legacy ? finalLayout : colorLayout,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            transition(cmd, depth, legacy ? finalLayout : depthLayout,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            if (bsl) {
                transition(cmd, secondColor, finalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy copy{};
                copy.bufferOffset = 64 * 64 * 8;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {64, 64, 1};
                vkCmdCopyImageToBuffer(cmd, secondColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback.buffer, 1, &copy);
            }
            for (auto im : {color, depth}) {
                VkBufferImageCopy copy{};
                copy.bufferOffset = im.aspect == VK_IMAGE_ASPECT_COLOR_BIT ? 0 : 64 * 64 * 4;
                copy.imageSubresource = {im.aspect, 0, 0, 1};
                copy.imageExtent = {64, 64, 1};
                vkCmdCopyImageToBuffer(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback.buffer, 1, &copy);
            }
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 1, &host, 0, nullptr, 0, nullptr);
            success(vkEndCommandBuffer(cmd), "end");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            success(vkQueueSubmit(queue, 1, &submit, fence), "submit");
            success(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ULL), "wait");
            auto pixels = (unsigned char *)readback.mapped;
            auto depths = (float *)(pixels + 64 * 64 * 4);
            for (unsigned i = 0; i < 4096; ++i) {
                if (bsl)
                    require(!memcmp(pixels + i * 4, pixels + 64 * 64 * 8 + i * 4, 4),
                            "BSL second shadow color attachment preservation");
                require(pixels[i * 4] == (direct ? 255 : 0) &&
                            pixels[i * 4 + 1] == (direct ? 0 : 255) &&
                            abs(int(pixels[i * 4 + 2]) - 64) <= 1 && pixels[i * 4 + 3] == 255,
                        "color, load preservation and DrawIndex");
                require(std::isfinite(depths[i]) &&
                            fabs(depths[i] - (direct ? 0.2f : 0.15f)) < 0.00001f,
                        "depth preservation");
            }
            if (with_query) {
                uint64_t value = 0;
                success(vkGetQueryPoolResults(device, query, 0, 1, sizeof(value), &value,
                                              sizeof(value), VK_QUERY_RESULT_64_BIT),
                        "query results");
                require(value > 0, "query sample count");
            }
        }
        printf("PASS: %u frame(s), %s%s%s; all 4096 colors/depths preserved\n", frames,
               direct ? "direct" : "multi-draw DrawIndex",
               with_query ? ", active occlusion query" : "", stress ? ", geometry stress" : "");
        printf("render path: %s, layouts: %s\n",
               legacy == 2 ? "RenderPass2"
               : legacy    ? "RenderPass"
                           : "dynamic",
               general ? "GENERAL" : "attachment -> shader read");
    }
};
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        bool direct = false, query = false, stress = false;
        unsigned legacy = 0;
        bool general = false;
        bool bsl = false;
        unsigned frames = 8;
        for (int i = 1; i < argc; ++i) {
            direct |= !strcmp(argv[i], "--direct");
            query |= !strcmp(argv[i], "--query");
            stress |= !strcmp(argv[i], "--stress");
            if (!strcmp(argv[i], "--legacy"))
                legacy = 1;
            if (!strcmp(argv[i], "--legacy2"))
                legacy = 2;
            general |= !strcmp(argv[i], "--general");
            bsl |= !strcmp(argv[i], "--bsl");
            if (!strncmp(argv[i], "--frames=", 9))
                frames = std::strtoul(argv[i] + 9, nullptr, 10);
        }
        require(frames > 0 && frames <= 1024, "frame count");
        require(!(stress && query), "stress must allow incremental rendering");
        Harness test;
        test.run(direct, query, stress, frames, bsl ? 1 : legacy, general || bsl, bsl);
        return 0;
    } catch (const std::exception &e) {
        printf("FAIL: %s\n", e.what());
        return 1;
    }
}

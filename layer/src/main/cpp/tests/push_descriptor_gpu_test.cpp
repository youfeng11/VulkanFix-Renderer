#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>

static void require(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(what);
}
static void success(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        printf("VkResult=%d: %s\n", r, what);
        throw std::runtime_error(what);
    }
}
static const char *shader_text = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %tex DescriptorSet 0
OpDecorate %tex Binding 0
OpDecorate %UBO Block
OpMemberDecorate %UBO 0 Offset 0
OpDecorate %ubo DescriptorSet 0
OpDecorate %ubo Binding 1
OpDecorate %array ArrayStride 4
OpDecorate %SSBO BufferBlock
OpMemberDecorate %SSBO 0 Offset 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 2
OpDecorate %Push Block
OpMemberDecorate %Push 0 Offset 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v2 = OpTypeVector %float 2
%v4 = OpTypeVector %float 4
%zero = OpConstant %uint 0
%four = OpConstant %uint 4
%fzero = OpConstant %float 0
%half = OpConstant %float 0.5
%thousand = OpConstant %float 1000
%coord = OpConstantComposite %v2 %half %half
%Image = OpTypeImage %float 2D 0 0 0 1 Unknown
%Sampled = OpTypeSampledImage %Image
%ptrtex = OpTypePointer UniformConstant %Sampled
%tex = OpVariable %ptrtex UniformConstant
%UBO = OpTypeStruct %uint
%ptrubo = OpTypePointer Uniform %UBO
%ptruint = OpTypePointer Uniform %uint
%ubo = OpVariable %ptrubo Uniform
%array = OpTypeArray %uint %four
%SSBO = OpTypeStruct %array
%ptrout = OpTypePointer Uniform %SSBO
%output = OpVariable %ptrout Uniform
%Push = OpTypeStruct %uint
%ptrpush = OpTypePointer PushConstant %Push
%ptrpushuint = OpTypePointer PushConstant %uint
%push = OpVariable %ptrpush PushConstant
%main = OpFunction %void None %fn
%entry = OpLabel
%s = OpLoad %Sampled %tex
%pixel = OpImageSampleExplicitLod %v4 %s %coord Lod %fzero
%depth = OpCompositeExtract %float %pixel 0
%scaled = OpFMul %float %depth %thousand
%asuint = OpConvertFToU %uint %scaled
%uptr = OpAccessChain %ptruint %ubo %zero
%u = OpLoad %uint %uptr
%sum = OpIAdd %uint %asuint %u
%iptr = OpAccessChain %ptrpushuint %push %zero
%index = OpLoad %uint %iptr
%dst = OpAccessChain %ptruint %output %zero %index
OpStore %dst %sum
OpReturn
OpFunctionEnd
)";

struct Harness {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkShaderModule shader{};
    VkSampler sampler{};
    VkFence fence{};
    VkDescriptorUpdateTemplate descriptor_template{};
    struct Buffer {
        VkBuffer handle;
        VkDeviceMemory memory;
        void *data;
    };
    struct Image {
        VkImage handle;
        VkDeviceMemory memory;
        VkImageView view;
    };
    std::vector<Buffer> buffers;
    std::vector<Image> images;
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("memory type unavailable");
    }
    Buffer buffer(uint32_t value, VkBufferUsageFlags usage) {
        Buffer b{};
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = 16;
        ci.usage = usage;
        success(vkCreateBuffer(device, &ci, nullptr, &b.handle), "create buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, b.handle, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            memory_type(req.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        success(vkAllocateMemory(device, &ai, nullptr, &b.memory), "allocate buffer");
        success(vkBindBufferMemory(device, b.handle, b.memory, 0), "bind buffer");
        success(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.data), "map buffer");
        memset(b.data, 0, 16);
        memcpy(b.data, &value, 4);
        buffers.push_back(b);
        return b;
    }
    Image image() {
        Image im{};
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_D32_SFLOAT;
        ci.extent = {4, 4, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        success(vkCreateImage(device, &ci, nullptr, &im.handle), "create depth image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, im.handle, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
        success(vkAllocateMemory(device, &ai, nullptr, &im.memory), "allocate depth image");
        success(vkBindImageMemory(device, im.handle, im.memory, 0), "bind depth image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.handle;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        success(vkCreateImageView(device, &vi, nullptr, &im.view), "create depth view");
        images.push_back(im);
        return im;
    }
    ~Harness() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (pool)
                vkDestroyCommandPool(device, pool, nullptr);
            if (fence)
                vkDestroyFence(device, fence, nullptr);
            if (descriptor_template)
                vkDestroyDescriptorUpdateTemplate(device, descriptor_template, nullptr);
            if (pipeline)
                vkDestroyPipeline(device, pipeline, nullptr);
            if (shader)
                vkDestroyShaderModule(device, shader, nullptr);
            if (layout)
                vkDestroyPipelineLayout(device, layout, nullptr);
            if (set_layout)
                vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            for (auto b : buffers) {
                vkUnmapMemory(device, b.memory);
                vkDestroyBuffer(device, b.handle, nullptr);
                vkFreeMemory(device, b.memory, nullptr);
            }
            for (auto im : images) {
                vkDestroyImageView(device, im.view, nullptr);
                vkDestroyImage(device, im.handle, nullptr);
                vkFreeMemory(device, im.memory, nullptr);
            }
            if (sampler)
                vkDestroySampler(device, sampler, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
    void run() {
        setenv("VULKAN_FIX_EMULATE_PUSH_DESCRIPTOR", "force", 1);
        setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
        setenv("VULKAN_FIX_LOG_PATH", "/data/local/tmp/vulkanfix-core-goal/push-gpu-layer.log", 1);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        success(vkCreateInstance(&ici, nullptr, &instance), "create instance");
        uint32_t count = 0;
        success(vkEnumeratePhysicalDevices(instance, &count, nullptr), "count GPUs");
        require(count > 0, "no GPU");
        std::vector<VkPhysicalDevice> gpus(count);
        success(vkEnumeratePhysicalDevices(instance, &count, gpus.data()), "get GPU");
        physical = gpus[0];
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        printf("Device: %s\n", props.deviceName);
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D32_SFLOAT, &fp);
        require((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
                "D32 sampling unavailable");
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i)
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                family = i;
                break;
            }
        require(family != UINT32_MAX, "no compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        const char *extension = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &extension;
        success(vkCreateDevice(physical, &dci, nullptr, &device), "create device");
        vkGetDeviceQueue(device, family, 0, &queue);
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{
            {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
              nullptr},
             {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
             {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sci.bindingCount = uint32_t(bindings.size());
        sci.pBindings = bindings.data();
        success(vkCreateDescriptorSetLayout(device, &sci, nullptr, &set_layout),
                "create push layout");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &set_layout;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &range;
        success(vkCreatePipelineLayout(device, &lci, nullptr, &layout), "create pipeline layout");
        VkDescriptorUpdateTemplateEntry entry{
            1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, sizeof(VkDescriptorBufferInfo)};
        VkDescriptorUpdateTemplateCreateInfo ti{
            VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
        ti.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
        ti.pipelineLayout = layout;
        ti.descriptorUpdateEntryCount = 1;
        ti.pDescriptorUpdateEntries = &entry;
        success(vkCreateDescriptorUpdateTemplate(device, &ti, nullptr, &descriptor_template),
                "create compute push template");
        spvtools::SpirvTools compiler(SPV_ENV_VULKAN_1_0);
        compiler.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &,
                                       const char *m) { printf("SPIR-V: %s\n", m); });
        std::vector<uint32_t> words;
        require(compiler.Assemble(shader_text, &words) && compiler.Validate(words),
                "assemble/validate shader");
        VkShaderModuleCreateInfo sh{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sh.codeSize = words.size() * 4;
        sh.pCode = words.data();
        success(vkCreateShaderModule(device, &sh, nullptr, &shader), "create shader");
        VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pc.layout = layout;
        pc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.stage.module = shader;
        pc.stage.pName = "main";
        success(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline),
                "create pipeline");
        // Keep the early-destruction case as the default. A separate retained
        // layout run isolates VVL lifetime diagnostics from the sampling test.
        if (!getenv("VULKAN_FIX_TEST_KEEP_SET_LAYOUT")) {
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            set_layout = VK_NULL_HANDLE;
        }
        auto a = buffer(7, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        auto b = buffer(19, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        auto output = buffer(0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto depth_a = image(), depth_b = image();
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        success(vkCreateSampler(device, &si, nullptr, &sampler), "create sampler");
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.queueFamilyIndex = family;
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        success(vkCreateCommandPool(device, &cp, nullptr, &pool), "create command pool");
        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        success(vkAllocateCommandBuffers(device, &ca, &cmd), "allocate command");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        success(vkCreateFence(device, &fi, nullptr, &fence), "create fence");
        auto push =
            (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
        auto push_template = (PFN_vkCmdPushDescriptorSetWithTemplateKHR)vkGetDeviceProcAddr(
            device, "vkCmdPushDescriptorSetWithTemplateKHR");
        require(push && push_template, "push entry points unavailable");
        for (uint32_t frame = 0; frame < 1000; ++frame) {
            success(vkResetCommandBuffer(cmd, 0), "reset command");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            success(vkBeginCommandBuffer(cmd, &bi), "begin command");
            if (frame == 0)
                for (uint32_t i = 0; i < 2; ++i) {
                    auto im = i ? depth_b : depth_a;
                    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    barrier.image = im.handle;
                    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
                        VK_QUEUE_FAMILY_IGNORED;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                         1, &barrier);
                    VkClearDepthStencilValue clear{i ? 0.75f : 0.25f, 0};
                    vkCmdClearDepthStencilImage(cmd, im.handle,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                                &barrier.subresourceRange);
                    barrier.oldLayout = barrier.newLayout;
                    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &barrier);
                }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            VkDescriptorImageInfo image_info{sampler, depth_a.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorBufferInfo uniform_info{a.handle, 0, 16}, out_info{output.handle, 0, 16};
            VkWriteDescriptorSet w[3]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            for (uint32_t n = 0; n < 3; ++n) {
                w[n].dstBinding = n;
                w[n].descriptorCount = 1;
                w[n].descriptorType = bindings[n].descriptorType;
            }
            w[0].pImageInfo = &image_info;
            w[1].pBufferInfo = &uniform_info;
            w[2].pBufferInfo = &out_info;
            auto dispatch = [&](uint32_t index) {
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &index);
                vkCmdDispatch(cmd, 1, 1, 1);
            };
            push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 3, w);
            dispatch(0);
            uniform_info.buffer = b.handle;
            push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, w + 1);
            dispatch(1);
            image_info.imageView = depth_b.view;
            push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, w);
            dispatch(2);
            uniform_info.buffer = a.handle;
            push_template(cmd, descriptor_template, layout, 0, &uniform_info);
            dispatch(3);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
            success(vkEndCommandBuffer(cmd), "end command");
            success(vkResetFences(device, 1, &fence), "reset fence");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            success(vkQueueSubmit(queue, 1, &submit, fence), "submit");
            success(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ULL), "wait GPU");
            const auto *values = (const uint32_t *)output.data;
            if (values[0] != 257 || values[1] != 269 || values[2] != 769 || values[3] != 757) {
                printf("frame=%u actual=%u,%u,%u,%u expected=257,269,769,757\n", frame, values[0],
                       values[1], values[2], values[3]);
                throw std::runtime_error("depth-image/partial descriptor snapshot result mismatch");
            }
        }
        printf("PASS: 1000 GPU frames sample two D32 depth images correctly across partial "
               "uniform/image and compute-template updates\n");
        printf("PASS: earlier dispatches retain their original image/uniform bindings and command reset remains valid\n");
        printf("Set-layout lifetime mode: %s\n", getenv("VULKAN_FIX_TEST_KEEP_SET_LAYOUT") ?
            "retained until teardown" : "application layout destroyed before dispatch");
    }
};
int main() {
    try {
        Harness test;
        test.run();
        return 0;
    } catch (const std::exception &e) {
        printf("FAIL: %s\n", e.what());
        return 1;
    }
}

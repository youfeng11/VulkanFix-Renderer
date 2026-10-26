#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <spirv-tools/libspirv.hpp>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>

static void success(VkResult result, const char* name) {
    if (result != VK_SUCCESS) {
        printf("FAIL: %s (%d)\n", name, result);
        throw std::runtime_error(name);
    }
}
static uint32_t align(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

static std::vector<uint32_t> shader(std::vector<uint32_t>& offsets) {
    std::ostringstream s;
    s << R"(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %Frame Block
OpDecorate %Output Block
OpMemberDecorate %Output 0 Offset 0
OpDecorate %frame DescriptorSet 0
OpDecorate %frame Binding 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 1
)";
    uint32_t offset = 0;
    for (unsigned i = 0; i < 305; ++i) {
        const unsigned type = i % 5;
        offset = align(offset, type == 1 || type == 3 ? 16 : type == 4 ? 8 : 4);
        offsets.push_back(offset);
        s << "OpMemberDecorate %Frame " << i << " Offset " << offset << "\n";
        if (type == 3)
            s << "OpMemberDecorate %Frame " << i << " ColMajor\n"
              << "OpMemberDecorate %Frame " << i << " MatrixStride 16\n";
        offset += type == 1 ? 12 : type == 3 ? 64 : type == 4 ? 8 : 4;
    }
    s << R"(%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%v2 = OpTypeVector %float 2
%v3 = OpTypeVector %float 3
%v4 = OpTypeVector %float 4
%mat4 = OpTypeMatrix %v4 4
%Frame = OpTypeStruct)";
    for (unsigned i = 0; i < 305; ++i)
        s << (i % 5 == 1 ? " %v3" : i % 5 == 3 ? " %mat4" : i % 5 == 4 ? " %v2" : " %float");
    s << R"(
%Output = OpTypeStruct %v4
%frameptr = OpTypePointer Uniform %Frame
%outputptr = OpTypePointer StorageBuffer %Output
%uniform2 = OpTypePointer Uniform %v2
%uniform1 = OpTypePointer Uniform %float
%storage4 = OpTypePointer StorageBuffer %v4
%zero = OpConstant %uint 0
%two = OpConstant %uint 2
%last = OpConstant %uint 299
%frame = OpVariable %frameptr Uniform
%output = OpVariable %outputptr StorageBuffer
%main = OpFunction %void None %fn
%entry = OpLabel
%wp = OpAccessChain %uniform2 %frame %last
%window = OpLoad %v2 %wp
%xp = OpAccessChain %uniform1 %frame %zero
%yp = OpAccessChain %uniform1 %frame %two
%x = OpLoad %float %xp
%y = OpLoad %float %yp
%w0 = OpCompositeExtract %float %window 0
%w1 = OpCompositeExtract %float %window 1
%result = OpCompositeConstruct %v4 %w0 %w1 %x %y
%dest = OpAccessChain %storage4 %output %zero
OpStore %dest %result
OpReturn
OpFunctionEnd
)";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* text) {
        printf("SPIR-V: %s\n", text);
    });
    std::vector<uint32_t> words;
    if (!tools.Assemble(s.str(), &words) || !tools.Validate(words))
        throw std::runtime_error("invalid GPU fixture");
    return words;
}

int main() try {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance{};
    success(vkCreateInstance(&ici, nullptr, &instance), "create instance");
    uint32_t count = 0;
    success(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate physical devices");
    if (!count) throw std::runtime_error("no GPU");
    std::vector<VkPhysicalDevice> devices(count);
    success(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPU");
    auto physical = devices.front();
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == count) throw std::runtime_error("no compute queue");
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice device{};
    success(vkCreateDevice(physical, &dci, nullptr, &device), "create device");
    VkQueue queue{}; vkGetDeviceQueue(device, family, 0, &queue);
    std::vector<uint32_t> offsets;
    auto words = shader(offsets), original = words;
    const uint32_t uniform_bytes = offsets.back() + 8;
    const uint32_t dynamic_offset = std::max(256u, uint32_t(props.limits.minUniformBufferOffsetAlignment));
    const uint32_t output_offset = align(dynamic_offset + uniform_bytes,
        std::max(16u, uint32_t(props.limits.minStorageBufferOffsetAlignment)));
    const uint32_t bytes = output_offset + 16;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buffer{}; success(vkCreateBuffer(device, &bci, nullptr, &buffer), "create buffer");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties memprops{}; vkGetPhysicalDeviceMemoryProperties(physical, &memprops);
    uint32_t type = 0;
    for (; type < memprops.memoryTypeCount; ++type)
        if ((req.memoryTypeBits & (1u << type)) &&
            (memprops.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    if (type == memprops.memoryTypeCount) throw std::runtime_error("no coherent host memory");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    VkDeviceMemory memory{}; success(vkAllocateMemory(device, &mai, nullptr, &memory), "allocate buffer");
    success(vkBindBufferMemory(device, buffer, memory, 0), "bind buffer");
    void* mapped{}; success(vkMapMemory(device, memory, 0, bytes, 0, &mapped), "map buffer");
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 2; lci.pBindings = bindings;
    VkDescriptorSetLayout layout{};
    success(vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout), "create descriptor layout");
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &layout;
    VkPipelineLayout pipeline_layout{};
    success(vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout), "create pipeline layout");
    auto create_shader = reinterpret_cast<PFN_vkCreateShaderModule>(vkGetDeviceProcAddr(device, "vkCreateShaderModule"));
    if (!create_shader) throw std::runtime_error("shader interception missing");
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = words.size() * 4; smci.pCode = words.data();
    VkShaderModule module{}; success(create_shader(device, &smci, nullptr, &module), "create same-version large UBO shader");
    if (words != original || smci.pCode != words.data() || smci.codeSize != words.size() * 4)
        throw std::runtime_error("application shader input was modified");
    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pci.layout = pipeline_layout;
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module = module; pci.stage.pName = "main";
    VkPipeline pipeline{};
    success(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline), "create compute pipeline");
    vkDestroyShaderModule(device, module, nullptr);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = sizes;
    VkDescriptorPool pool{}; success(vkCreateDescriptorPool(device, &dpci, nullptr, &pool), "create descriptor pool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &layout;
    VkDescriptorSet set{}; success(vkAllocateDescriptorSets(device, &dsai, &set), "allocate descriptor set");
    VkDescriptorBufferInfo infos[] = {{buffer, 0, uniform_bytes}, {buffer, output_offset, 16}};
    VkWriteDescriptorSet writes[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType; writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = family; cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool command_pool{};
    success(vkCreateCommandPool(device, &cpci, nullptr, &command_pool), "create command pool");
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = command_pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd{}; success(vkAllocateCommandBuffers(device, &cbai, &cmd), "allocate command buffer");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{}; success(vkCreateFence(device, &fci, nullptr, &fence), "create fence");
    for (unsigned frame = 0; frame < 8; ++frame) {
        std::memset(mapped, 0xcd, bytes);
        const float expected[] = {0.25f + frame * 0.125f, 0.5f, 0.75f, 1.0f};
        auto* ubo = static_cast<char*>(mapped) + dynamic_offset;
        std::memcpy(ubo + offsets[299], expected, 8);
        std::memcpy(ubo + offsets[0], expected + 2, 4);
        std::memcpy(ubo + offsets[2], expected + 3, 4);
        success(vkResetCommandBuffer(cmd, 0), "reset command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        success(vkBeginCommandBuffer(cmd, &begin), "begin command buffer");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 1, &dynamic_offset);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer; barrier.offset = output_offset; barrier.size = 16;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        success(vkEndCommandBuffer(cmd), "end command buffer");
        success(vkResetFences(device, 1, &fence), "reset fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        success(vkQueueSubmit(queue, 1, &submit, fence), "submit");
        success(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ULL), "wait");
        float actual[4]; std::memcpy(actual, static_cast<char*>(mapped) + output_offset, 16);
        for (unsigned i = 0; i < 4; ++i)
            if (!std::isfinite(actual[i]) || std::fabs(actual[i] - expected[i]) > 0.0001f) {
                printf("frame=%u component=%u actual=%f expected=%f\n", frame, i, actual[i], expected[i]);
                throw std::runtime_error("large UBO GPU result differs from CPU");
            }
    }
    printf("PASS: same-version 305-member UBO, dynamic descriptor offset, eight updated GPU results\n");
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, command_pool, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(device, layout, nullptr);
    vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
    return 0;
} catch (const std::exception& error) {
    printf("FAIL: %s\n", error.what());
    return 1;
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <sstream>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
static unsigned failures = 0;
static void check(bool ok, const char *name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
static bool success(VkResult result, const char *name) {
    if (result == VK_SUCCESS)
        return true;
    printf("FAIL: %s (%d)\n", name, result);
    ++failures;
    return false;
}
static std::vector<uint32_t> shader(bool dynamic, unsigned source, unsigned quad, unsigned local,
                                    bool divergent, bool include_quad,
                                    bool static_vectors = false) {
    std::ostringstream s;
    s << "OpCapability Shader\nOpCapability GroupNonUniform\nOpCapability "
         "GroupNonUniformBallot\nOpCapability GroupNonUniformQuad\n"
      << "OpMemoryModel Logical GLSL450\nOpEntryPoint GLCompute %main \"main\" %lane %subgroup "
         "%global";
    if (dynamic)
        s << " %push %data";
    s << "\nOpExecutionMode %main LocalSize " << local << " 1 1\n"
      << R"(
OpDecorate %lane BuiltIn SubgroupLocalInvocationId
OpDecorate %subgroup BuiltIn SubgroupId
OpDecorate %global BuiltIn GlobalInvocationId
OpDecorate %Push Block
OpMemberDecorate %Push 0 Offset 0
OpMemberDecorate %Push 1 Offset 4
OpMemberDecorate %Push 2 Offset 8
OpDecorate %array ArrayStride 4
OpDecorate %Block Block
OpMemberDecorate %Block 0 Offset 0
OpDecorate %data DescriptorSet 0
OpDecorate %data Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%bool = OpTypeBool
%v3 = OpTypeVector %uint 3
%v4 = OpTypeVector %uint 4
%array = OpTypeRuntimeArray %uint
%Block = OpTypeStruct %array
%Push = OpTypeStruct %uint %uint %uint
%ptrdata = OpTypePointer StorageBuffer %Block
%ptrout = OpTypePointer StorageBuffer %uint
%ptrpush = OpTypePointer PushConstant %Push
%ptrpushuint = OpTypePointer PushConstant %uint
%ptrin = OpTypePointer Input %uint
%ptrin3 = OpTypePointer Input %v3
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%scope = OpConstant %uint 3
%seventeen = OpConstant %uint 17
%stride = OpConstant %uint 20
)";
    s << "%choice = OpConstant %uint " << source << "\n%quadchoice = OpConstant %uint " << quad
      << "\n";
    for (unsigned i = 0; i < 20; ++i)
        s << "%c" << i << " = OpConstant %uint " << i << "\n";
    s << R"(
%lane = OpVariable %ptrin Input
%subgroup = OpVariable %ptrin Input
%global = OpVariable %ptrin3 Input
%push = OpVariable %ptrpush PushConstant
%data = OpVariable %ptrdata StorageBuffer
%main = OpFunction %void None %fn
%entry = OpLabel
%l = OpLoad %uint %lane
%sg = OpLoad %uint %subgroup
%g = OpLoad %v3 %global
%x = OpCompositeExtract %uint %g 0
%base = OpIMul %uint %x %stride
%sp = OpAccessChain %ptrpushuint %push %zero
%qp = OpAccessChain %ptrpushuint %push %one
%hp = OpAccessChain %ptrpushuint %push %two
%source = OpLoad %uint %sp
%quadsource = OpLoad %uint %qp
%half = OpLoad %uint %hp
)";
    if (divergent)
        s << "%active = OpULessThan %bool %l %half\nOpSelectionMerge %end "
             "None\nOpBranchConditional %active %work %end\n%work = OpLabel\n";
    s << R"(
%scaled = OpIMul %uint %l %seventeen
%value = OpIAdd %uint %scaled %sg
%parity = OpBitwiseAnd %uint %l %one
%boolean = OpIEqual %bool %parity %zero
%floatvalue = OpConvertUToF %float %value
%vector = OpCompositeConstruct %v4 %l %value %sg %parity
)";
    const std::string id = dynamic ? "%source" : "%choice";
    const std::string qid = dynamic ? "%quadsource" : "%quadchoice";
    s << "%b = OpGroupNonUniformBroadcast %uint %scope %value " << id << "\n"
      << "%bb = OpGroupNonUniformBroadcast %bool %scope %boolean " << id << "\n"
      << "%bf = OpGroupNonUniformBroadcast %float %scope %floatvalue " << id << "\n"
      << "%bbool = OpSelect %uint %bb %one %zero\n%bfloat = OpBitcast %uint %bf\n";
    if (dynamic || static_vectors)
        s << "%bv = OpGroupNonUniformBroadcast %v4 %scope %vector " << id << "\n";
    else {
        const char *components[]{"%l", "%value", "%sg", "%parity"};
        for (unsigned i = 0; i < 4; ++i)
            s << "%bscalar" << i << " = OpGroupNonUniformBroadcast %uint %scope " << components[i]
              << " " << id << "\n";
        s << "%bv = OpCompositeConstruct %v4 %bscalar0 %bscalar1 %bscalar2 %bscalar3\n";
    }
    if (include_quad)
        s << "%q = OpGroupNonUniformQuadBroadcast %uint %scope %value " << qid << "\n"
          << "%qb = OpGroupNonUniformQuadBroadcast %bool %scope %boolean " << qid << "\n"
          << "%qf = OpGroupNonUniformQuadBroadcast %float %scope %floatvalue " << qid << "\n"
          << "%qbool = OpSelect %uint %qb %one %zero\n%qfloat = OpBitcast %uint %qf\n";
    if (include_quad) {
        if (dynamic || static_vectors)
            s << "%qv = OpGroupNonUniformQuadBroadcast %v4 %scope %vector " << qid << "\n";
        else {
            const char *components[]{"%l", "%value", "%sg", "%parity"};
            for (unsigned i = 0; i < 4; ++i)
                s << "%qscalar" << i << " = OpGroupNonUniformQuadBroadcast %uint %scope "
                  << components[i] << " " << qid << "\n";
            s << "%qv = OpCompositeConstruct %v4 %qscalar0 %qscalar1 %qscalar2 %qscalar3\n";
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        s << "%bc" << i << " = OpCompositeExtract %uint %bv " << i << "\n";
        if (include_quad)
            s << "%qc" << i << " = OpCompositeExtract %uint %qv " << i << "\n";
    }
    std::vector<std::string> outputs = {"%l",
                                        "%sg",
                                        "%source",
                                        "%quadsource",
                                        "%b",
                                        include_quad ? "%q" : "%zero",
                                        "%bbool",
                                        include_quad ? "%qbool" : "%zero",
                                        "%bc0",
                                        "%bc1",
                                        "%bc2",
                                        "%bc3",
                                        include_quad ? "%qc0" : "%zero",
                                        include_quad ? "%qc1" : "%zero",
                                        include_quad ? "%qc2" : "%zero",
                                        include_quad ? "%qc3" : "%zero",
                                        "%bfloat",
                                        include_quad ? "%qfloat" : "%zero",
                                        "%value",
                                        "%parity"};
    for (unsigned i = 0; i < outputs.size(); ++i)
        s << "%index" << i << " = OpIAdd %uint %base %c" << i << "\n%out" << i
          << " = OpAccessChain %ptrout %data %zero %index" << i << "\nOpStore %out" << i << " "
          << outputs[i] << "\n";
    if (divergent)
        s << "OpBranch %end\n%end = OpLabel\n";
    s << "OpReturn\nOpFunctionEnd\n";
    spvtools::SpirvTools tools(dynamic ? SPV_ENV_VULKAN_1_2 : SPV_ENV_VULKAN_1_1);
    tools.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &,
                                const char *e) { printf("SPIR-V: %s\n", e); });
    std::vector<uint32_t> words;
    if (!tools.Assemble(s.str(), &words) || !tools.Validate(words)) {
        ++failures;
        return {};
    }
    return words;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance{};
    if (!success(vkCreateInstance(&ici, nullptr, &instance), "create instance"))
        return 1;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    if (!count || !success(vkEnumeratePhysicalDevices(instance, &count, physicals.data()),
                           "enumerate device"))
        return 1;
    auto physical = physicals.front();
    VkPhysicalDeviceVulkan12Features features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features};
    vkGetPhysicalDeviceFeatures2(physical, &f2);
    check(features.subgroupBroadcastDynamicId, "dynamic subgroup broadcast is reported available");
    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
    vkGetPhysicalDeviceProperties2(physical, &p2);
    printf("Subgroup size=%u operations=0x%x\n", subgroup.subgroupSize,
           subgroup.supportedOperations);
    if (!(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
        !(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) ||
        !(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) ||
        subgroup.subgroupSize < 4) {
        printf("FAIL: test needs native compute ballot and quad support\n");
        return 1;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    uint32_t family = 0;
    while (family < count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        ++family;
    if (family == count)
        return 1;
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled.subgroupBroadcastDynamicId = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    const auto saved = enabled;
    VkDevice device{};
    if (!success(vkCreateDevice(physical, &dci, nullptr, &device), "enable dynamic broadcast"))
        return 1;
    check(!memcmp(&enabled, &saved, sizeof(enabled)),
          "feature enabling preserves application input");
    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);
    const uint32_t local = subgroup.subgroupSize * 2;
    const VkDeviceSize bytes = VkDeviceSize(local) * 2 * 20 * sizeof(uint32_t);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buffer{};
    if (!success(vkCreateBuffer(device, &bci, nullptr, &buffer), "create output buffer"))
        return 1;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    unsigned type = 0;
    for (; type < memory_props.memoryTypeCount; ++type)
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (memory_props.memoryTypes[type].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            break;
    if (type == memory_props.memoryTypeCount)
        return 1;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = requirements.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory{};
    if (!success(vkAllocateMemory(device, &mai, nullptr, &memory), "allocate output memory"))
        return 1;
    vkBindBufferMemory(device, buffer, memory, 0);
    void *mapped{};
    vkMapMemory(device, memory, 0, bytes, 0, &mapped);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 1;
    dlci.pBindings = &binding;
    VkDescriptorSetLayout layout{};
    vkCreateDescriptorSetLayout(device, &dlci, nullptr, &layout);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    VkPipelineLayout pipeline_layout{};
    vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout);
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;
    VkDescriptorPool pool{};
    vkCreateDescriptorPool(device, &dpci, nullptr, &pool);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout;
    VkDescriptorSet set{};
    vkAllocateDescriptorSets(device, &dsai, &set);
    VkDescriptorBufferInfo buffer_info{buffer, 0, bytes};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = family;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool command_pool{};
    vkCreateCommandPool(device, &cpci, nullptr, &command_pool);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = command_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    vkAllocateCommandBuffers(device, &cbai, &cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{};
    vkCreateFence(device, &fci, nullptr, &fence);
    auto pipeline = [&](bool dynamic, unsigned source, unsigned quad, unsigned workgroup,
                        bool divergent, bool include_quad, bool static_vectors = false) {
        VkPipeline result{};
        auto words =
            shader(dynamic, source, quad, workgroup, divergent, include_quad, static_vectors);
        if (words.empty())
            return result;
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = words.size() * 4;
        smci.pCode = words.data();
        VkShaderModule module{};
        if (!success(vkCreateShaderModule(device, &smci, nullptr, &module),
                     "create broadcast shader"))
            return result;
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.layout = pipeline_layout;
        pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pci.stage.module = module;
        pci.stage.pName = "main";
        success(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &result),
                "create broadcast compute pipeline");
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    };
    auto execute = [&](VkPipeline pipe, const uint32_t *values) {
        std::vector<uint32_t> result(size_t(bytes) / 4);
        if (!pipe)
            return result;
        memset(mapped, 0xcd, size_t(bytes));
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &begin);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, values);
        vkCmdDispatch(cmd, 2, 1, 1);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        if (!success(vkEndCommandBuffer(cmd), "record broadcast dispatch"))
            return result;
        vkResetFences(device, 1, &fence);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        if (!success(vkQueueSubmit(queue, 1, &submit, fence), "submit broadcast dispatch") ||
            !success(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ULL),
                     "wait broadcast dispatch"))
            return result;
        memcpy(result.data(), mapped, size_t(bytes));
        return result;
    };
    auto run = [&](bool divergent, bool partial) {
        unsigned workgroup = partial ? subgroup.subgroupSize + 1 : local;
        unsigned sources = partial     ? 1
                           : divergent ? subgroup.subgroupSize / 2
                                       : subgroup.subgroupSize;
        auto dynamic = pipeline(true, 0, 0, workgroup, divergent, !partial);
        for (unsigned i = 0; i < sources; ++i) {
            uint32_t values[]{i, i % 4, subgroup.subgroupSize / 2};
            auto reference = pipeline(false, i, i % 4, workgroup, divergent, !partial);
            auto actual = execute(dynamic, values), expected = execute(reference, values);
            bool equal = actual == expected;
            if (!equal)
                for (unsigned j = 0, shown = 0; j < actual.size() && shown < 12; ++j)
                    if (actual[j] != expected[j]) {
                        printf("Mismatch source=%u word=%u actual=%u expected=%u\n", i, j,
                               actual[j], expected[j]);
                        ++shown;
                    }
            std::string name = std::string(partial     ? "partial subgroup"
                                           : divergent ? "divergent active subgroup"
                                                       : "full subgroup") +
                               ": dynamic and native constant outputs agree for source " +
                               std::to_string(i);
            check(dynamic && reference && equal, name.c_str());
            bool cpu = true;
            unsigned written = 0;
            for (unsigned j = 0; j < workgroup * 2; ++j) {
                auto p = actual.data() + j * 20;
                if (p[0] == 0xcdcdcdcd)
                    continue;
                ++written;
                cpu &= p[4] == i * 17 + p[1] && p[6] == (i % 2 == 0) && p[8] == i &&
                       p[9] == i * 17 + p[1];
            }
            cpu &= written == (divergent ? (workgroup / subgroup.subgroupSize) *
                                               (subgroup.subgroupSize / 2) * 2
                                         : workgroup * 2);
            check(cpu, "scalar, boolean, and vector broadcast results also match the CPU oracle");
            vkDestroyPipeline(device, reference, nullptr);
        }
        vkDestroyPipeline(device, dynamic, nullptr);
    };
    run(false, false);
    run(true, false);
    run(false, true);
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t values[]{i, i, subgroup.subgroupSize / 2};
        auto vectors = pipeline(false, i, i, local, false, true, true);
        auto reference = pipeline(false, i, i, local, false, true);
        check(vectors && reference && execute(vectors, values) == execute(reference, values),
              "SPIR-V 1.3 mixed constant vector broadcasts also match independent scalar native "
              "operations");
        vkDestroyPipeline(device, vectors, nullptr);
        vkDestroyPipeline(device, reference, nullptr);
    }
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, command_pool, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(device, layout, nullptr);
    vkUnmapMemory(device, memory);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    printf("Subgroup GPU failures: %u\n", failures);
    return failures ? 1 : 0;
}

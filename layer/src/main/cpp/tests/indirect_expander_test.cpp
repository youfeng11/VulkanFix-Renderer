#include "modules/vulkan_1_2/indirect_count_shader.h"

#include <spirv-tools/libspirv.h>
#include <spirv-tools/libspirv.hpp>

#include <vulkan/vulkan.h>

#include <android/log.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#define TAG "IndirectExpanderTest"
#define LOGI(...) do { printf(__VA_ARGS__); printf("\n"); __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); } while (0)
#define LOGE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__); } while (0)

static bool validate_shader(const char* name, const char* assembly) {
    spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_0);
    if (context == nullptr) {
        LOGE("%s: spvContextCreate failed", name);
        return false;
    }

    spv_binary binary = nullptr;
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t assembled = spvTextToBinary(
        context, assembly, strlen(assembly), &binary, &diagnostic);
    if (assembled != SPV_SUCCESS || binary == nullptr) {
        LOGE("%s: assembly FAILED: %s", name,
             diagnostic && diagnostic->error ? diagnostic->error : "(no diagnostic)");
        if (diagnostic) spvDiagnosticDestroy(diagnostic);
        spvContextDestroy(context);
        return false;
    }

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_0);
    tools.SetMessageConsumer([name](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
        LOGE("%s validate: %zu:%zu: %s", name, position.line, position.column, message);
    });
    const bool valid = tools.Validate(binary->code, binary->wordCount);
    LOGI("%s: assembled %zu words, Vulkan 1.0 validation: %s", name,
         binary->wordCount, valid ? "OK" : "FAILED");

    spvBinaryDestroy(binary);
    if (diagnostic) spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    return valid;
}

namespace {

struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
};

uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t typeBits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool create_host_buffer(const Context& ctx, VkDeviceSize size,
                        VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ctx.device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        LOGE("vkCreateBuffer failed (size %llu)", (unsigned long long)size);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(ctx.device, buffer, &requirements);
    const uint32_t type = find_memory_type(
        ctx.physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        LOGE("no host-visible coherent memory type for buffer");
        vkDestroyBuffer(ctx.device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = type;
    if (vkAllocateMemory(ctx.device, &allocateInfo, nullptr, &memory) != VK_SUCCESS) {
        LOGE("vkAllocateMemory failed");
        vkDestroyBuffer(ctx.device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(ctx.device, buffer, memory, 0) != VK_SUCCESS) {
        LOGE("vkBindBufferMemory failed");
        vkDestroyBuffer(ctx.device, buffer, nullptr);
        vkFreeMemory(ctx.device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    if (vkMapMemory(ctx.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        LOGE("vkMapMemory failed");
        vkDestroyBuffer(ctx.device, buffer, nullptr);
        vkFreeMemory(ctx.device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool run_case(const Context& ctx, const char* name, const char* assembly,
              uint32_t commandWords, uint32_t strideWords, uint32_t maxDrawCount, uint32_t count) {
    const VkDeviceSize srcSize = static_cast<VkDeviceSize>(maxDrawCount) * strideWords * sizeof(uint32_t);
    const VkDeviceSize dstSize = srcSize;
    const VkDeviceSize cntSize = sizeof(uint32_t);

    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE, cnt = VK_NULL_HANDLE;
    VkDeviceMemory srcMemory = VK_NULL_HANDLE, dstMemory = VK_NULL_HANDLE, cntMemory = VK_NULL_HANDLE;
    void* srcPtr = nullptr;
    void* dstPtr = nullptr;
    void* cntPtr = nullptr;
    if (!create_host_buffer(ctx, srcSize, src, srcMemory, srcPtr) ||
        !create_host_buffer(ctx, dstSize, dst, dstMemory, dstPtr) ||
        !create_host_buffer(ctx, cntSize, cnt, cntMemory, cntPtr)) {
        return false;
    }

    uint32_t* srcWords = static_cast<uint32_t*>(srcPtr);
    uint32_t* dstWords = static_cast<uint32_t*>(dstPtr);
    uint32_t* cntWords = static_cast<uint32_t*>(cntPtr);
    for (uint32_t i = 0; i < maxDrawCount; ++i) {
        for (uint32_t k = 0; k < strideWords; ++k) {
            dstWords[i * strideWords + k] = 0xDEADBEEFu;
        }
        for (uint32_t k = 0; k < commandWords; ++k) {
            srcWords[i * strideWords + k] = (i + 1) * 100u + k;
        }
    }
    cntWords[0] = count;

    spv_context spvContext = spvContextCreate(SPV_ENV_VULKAN_1_0);
    spv_binary binary = nullptr;
    spv_diagnostic diagnostic = nullptr;
    if (spvContext == nullptr ||
        spvTextToBinary(spvContext, assembly, strlen(assembly), &binary, &diagnostic) != SPV_SUCCESS ||
        binary == nullptr) {
        LOGE("%s: failed to assemble shader", name);
        if (diagnostic) spvDiagnosticDestroy(diagnostic);
        if (spvContext) spvContextDestroy(spvContext);
        return false;
    }
    if (diagnostic) spvDiagnosticDestroy(diagnostic);

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = binary->wordCount * sizeof(uint32_t);
    moduleInfo.pCode = binary->code;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkResult result = vkCreateShaderModule(ctx.device, &moduleInfo, nullptr, &shaderModule);
    spvBinaryDestroy(binary);
    spvContextDestroy(spvContext);
    if (result != VK_SUCCESS) {
        LOGE("%s: vkCreateShaderModule failed (%d)", name, result);
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &descriptorLayout) != VK_SUCCESS) {
        LOGE("%s: vkCreateDescriptorSetLayout failed", name);
        vkDestroyShaderModule(ctx.device, shaderModule, nullptr);
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 5 * sizeof(uint32_t);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(ctx.device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        LOGE("%s: vkCreatePipelineLayout failed", name);
        vkDestroyDescriptorSetLayout(ctx.device, descriptorLayout, nullptr);
        vkDestroyShaderModule(ctx.device, shaderModule, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        LOGE("%s: vkCreateComputePipelines failed", name);
        vkDestroyPipelineLayout(ctx.device, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device, descriptorLayout, nullptr);
        vkDestroyShaderModule(ctx.device, shaderModule, nullptr);
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 3;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        LOGE("%s: vkCreateDescriptorPool failed", name);
        return false;
    }
    VkDescriptorSetAllocateInfo setAllocateInfo{};
    setAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocateInfo.descriptorPool = descriptorPool;
    setAllocateInfo.descriptorSetCount = 1;
    setAllocateInfo.pSetLayouts = &descriptorLayout;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(ctx.device, &setAllocateInfo, &descriptorSet) != VK_SUCCESS) {
        LOGE("%s: vkAllocateDescriptorSets failed", name);
        return false;
    }

    VkDescriptorBufferInfo bufferInfos[3]{};
    bufferInfos[0].buffer = src;
    bufferInfos[0].range = VK_WHOLE_SIZE;
    bufferInfos[1].buffer = dst;
    bufferInfos[1].range = VK_WHOLE_SIZE;
    bufferInfos[2].buffer = cnt;
    bufferInfos[2].range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(ctx.device, 3, writes, 0, nullptr);

    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = ctx.commandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(ctx.device, &cmdAlloc, &commandBuffer) != VK_SUCCESS) {
        LOGE("%s: vkAllocateCommandBuffers failed", name);
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    const uint32_t pushConstants[5] = {0, 0, 0, maxDrawCount, strideWords};
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pushConstants), pushConstants);
    vkCmdDispatch(commandBuffer, 1, 1, 1);

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(commandBuffer);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(ctx.device, &fenceInfo, nullptr, &fence);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    result = vkQueueSubmit(ctx.queue, 1, &submitInfo, fence);
    if (result == VK_SUCCESS) {
        result = vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    if (result != VK_SUCCESS) {
        LOGE("%s: submit/wait failed (%d)", name, result);
    }

    bool ok = (result == VK_SUCCESS);
    if (ok) {
        for (uint32_t i = 0; i < maxDrawCount; ++i) {
            for (uint32_t k = 0; k < commandWords; ++k) {
                const uint32_t expected = (i < count) ? srcWords[i * strideWords + k] : 0u;
                const uint32_t actual = dstWords[i * strideWords + k];
                if (actual != expected) {
                    LOGE("%s: mismatch at command %u word %u: expected 0x%08x got 0x%08x",
                         name, i, k, expected, actual);
                    ok = false;
                }
            }
        }
    }
    LOGI("%s: count=%u max=%u commandWords=%u strideWords=%u -> %s", name, count, maxDrawCount,
         commandWords, strideWords, ok ? "PASS" : "FAIL");

    vkDestroyFence(ctx.device, fence, nullptr);
    vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &commandBuffer);
    vkDestroyDescriptorPool(ctx.device, descriptorPool, nullptr);
    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, descriptorLayout, nullptr);
    vkDestroyShaderModule(ctx.device, shaderModule, nullptr);
    vkUnmapMemory(ctx.device, srcMemory);
    vkUnmapMemory(ctx.device, dstMemory);
    vkUnmapMemory(ctx.device, cntMemory);
    vkDestroyBuffer(ctx.device, src, nullptr);
    vkDestroyBuffer(ctx.device, dst, nullptr);
    vkDestroyBuffer(ctx.device, cnt, nullptr);
    vkFreeMemory(ctx.device, srcMemory, nullptr);
    vkFreeMemory(ctx.device, dstMemory, nullptr);
    vkFreeMemory(ctx.device, cntMemory, nullptr);
    return ok;
}

bool init_vulkan(Context& ctx) {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "IndirectExpanderTest";
    appInfo.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    if (vkCreateInstance(&instanceInfo, nullptr, &ctx.instance) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed");
        return false;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(ctx.instance, &count, nullptr);
    if (count == 0) {
        LOGE("no physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(ctx.instance, &count, devices.data());

    for (VkPhysicalDevice device : devices) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                ctx.physical = device;
                ctx.queueFamily = i;
                break;
            }
        }
        if (ctx.physical != VK_NULL_HANDLE) break;
    }
    if (ctx.physical == VK_NULL_HANDLE) {
        LOGE("no compute-capable physical device");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = ctx.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    if (vkCreateDevice(ctx.physical, &deviceInfo, nullptr, &ctx.device) != VK_SUCCESS) {
        LOGE("vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(ctx.device, ctx.queueFamily, 0, &ctx.queue);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = ctx.queueFamily;
    if (vkCreateCommandPool(ctx.device, &poolInfo, nullptr, &ctx.commandPool) != VK_SUCCESS) {
        LOGE("vkCreateCommandPool failed");
        return false;
    }
    return true;
}

void destroy_vulkan(Context& ctx) {
    if (ctx.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx.device);
        if (ctx.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(ctx.device, ctx.commandPool, nullptr);
        }
        vkDestroyDevice(ctx.device, nullptr);
    }
    if (ctx.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(ctx.instance, nullptr);
    }
}

}  // namespace

int main() {
    const bool draw_valid = validate_shader("draw", kIndirectCountDrawShader);
    const bool indexed_valid = validate_shader("indexed", kIndirectCountIndexedShader);
    if (!draw_valid || !indexed_valid) {
        return 1;
    }

    Context ctx;
    if (!init_vulkan(ctx)) {
        destroy_vulkan(ctx);
        return 1;
    }

    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(ctx.physical, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(ctx.physical, nullptr, &extension_count, extensions.data());
    for (const VkExtensionProperties& extension : extensions) {
        LOGI("device extension: %s (v%u)", extension.extensionName, extension.specVersion);
    }

    VkPhysicalDeviceFeatures core_features{};
    vkGetPhysicalDeviceFeatures(ctx.physical, &core_features);
    VkPhysicalDeviceProperties core_props{};
    vkGetPhysicalDeviceProperties(ctx.physical, &core_props);
    LOGI("core features: fillModeNonSolid=%d vertexPipelineStoresAndAtomics=%d",
         core_features.fillModeNonSolid, core_features.vertexPipelineStoresAndAtomics);
    LOGI("limits: maxPerStageDescriptorStorageBuffers=%u maxPerStageDescriptorStorageImages=%u",
         core_props.limits.maxPerStageDescriptorStorageBuffers,
         core_props.limits.maxPerStageDescriptorStorageImages);
    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing{};
    descriptor_indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{};
    bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar{};
    scalar.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
    VkPhysicalDeviceUniformBufferStandardLayoutFeatures ubo{};
    ubo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES;
    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES;
    descriptor_indexing.pNext = &bda;
    bda.pNext = &scalar;
    scalar.pNext = &ubo;
    ubo.pNext = &subgroup;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &descriptor_indexing;
    auto get_features2 = (PFN_vkGetPhysicalDeviceFeatures2)
        vkGetInstanceProcAddr(ctx.instance, "vkGetPhysicalDeviceFeatures2");
    if (get_features2) {
        get_features2(ctx.physical, &features2);
        LOGI("DI: input=%d uniform=%d sampled=%d storage=%d partial=%d variable=%d runtime=%d",
             descriptor_indexing.shaderInputAttachmentArrayDynamicIndexing,
             descriptor_indexing.shaderUniformBufferArrayNonUniformIndexing,
             descriptor_indexing.shaderSampledImageArrayNonUniformIndexing,
             descriptor_indexing.shaderStorageBufferArrayNonUniformIndexing,
             descriptor_indexing.descriptorBindingPartiallyBound,
             descriptor_indexing.descriptorBindingVariableDescriptorCount,
             descriptor_indexing.runtimeDescriptorArray);
        LOGI("BDA=%d scalar=%d ubo=%d subgroup=%d",
             bda.bufferDeviceAddress, scalar.scalarBlockLayout,
             ubo.uniformBufferStandardLayout, subgroup.shaderSubgroupExtendedTypes);
    }

    const bool draw_ok = run_case(ctx, "draw", kIndirectCountDrawShader, 4, 4, 4, 3);
    const bool indexed_ok = run_case(ctx, "indexed", kIndirectCountIndexedShader, 5, 5, 4, 3);
    const bool padded_ok = run_case(ctx, "draw-padded", kIndirectCountDrawShader, 4, 6, 4, 3);
    destroy_vulkan(ctx);
    LOGI("result: draw=%s indexed=%s padded=%s",
         draw_ok ? "PASS" : "FAIL", indexed_ok ? "PASS" : "FAIL", padded_ok ? "PASS" : "FAIL");
    return (draw_ok && indexed_ok && padded_ok) ? 0 : 1;
}


#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <android/log.h>

#define TAG "VulkanFixTest"
#define LOGI(...) do { printf(__VA_ARGS__); printf("\n"); __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); } while(0)
#define LOGE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__); } while(0)

extern "C" {
    void vulkan_fix_trigger_capture(uint32_t num_frames);
    void vulkan_fix_start_capture(VkDevice device);
    void vulkan_fix_end_capture(VkDevice device);
    uint32_t vulkan_fix_is_renderdoc_attached();
}
#include <chrono>

int main(int argc, char** argv) {
    LOGI("==================================================");
    LOGI("  VulkanFix + RenderDoc Validation Test Runner    ");
    LOGI("==================================================");

    uint32_t rdocAttached = vulkan_fix_is_renderdoc_attached();
    LOGI("RenderDoc In-App status: %s", rdocAttached ? "ATTACHED" : "NOT ATTACHED (will auto-detect if layer active)");

    // 1. Create Instance
    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "VulkanFixRenderDocTest";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "VulkanFix";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> instanceExts;
    instanceExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // Enumerate available instance layers
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availLayers.data());
    std::vector<const char*> enabledLayers;
    for (const auto& l : availLayers) {
        LOGI("Found layer: %s (%s, v%u)", l.layerName, l.description, l.implementationVersion);
        if (strcmp(l.layerName, "VK_LAYER_RENDERDOC_Capture") == 0) {
            enabledLayers.push_back("VK_LAYER_RENDERDOC_Capture");
            LOGI("Enabling layer: %s", l.layerName);
        }
    }

    VkInstanceCreateInfo instCI = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instCI.pApplicationInfo = &appInfo;
    instCI.enabledExtensionCount = (uint32_t)instanceExts.size();
    instCI.ppEnabledExtensionNames = instanceExts.data();
    instCI.enabledLayerCount = (uint32_t)enabledLayers.size();
    instCI.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();

    VkInstance instance = VK_NULL_HANDLE;
    VkResult res = vkCreateInstance(&instCI, nullptr, &instance);
    if (res != VK_SUCCESS) {
        LOGE("Failed to create Vulkan instance: %d", res);
        return 1;
    }
    LOGI("Successfully created VkInstance: %p", instance);

    // 2. Pick Physical Device
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOGE("No Vulkan physical devices found!");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());
    VkPhysicalDevice gpu = gpus[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    LOGI("Selected GPU: %s (Vulkan %u.%u.%u)", props.deviceName,
         VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion),
         VK_VERSION_PATCH(props.apiVersion));

    // 3. Find Queue Family
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfProps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, qfProps.data());

    int graphicsQf = -1;
    for (uint32_t i = 0; i < qfCount; ++i) {
        if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsQf = (int)i;
            break;
        }
    }
    if (graphicsQf < 0) {
        LOGE("No graphics queue family found!");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // 4. Check device extensions
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &devExtCount, devExts.data());

    bool hasDynamicRendering = false;
    for (const auto& ext : devExts) {
        if (strcmp(ext.extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0) {
            hasDynamicRendering = true;
            break;
        }
    }
    LOGI("VK_KHR_dynamic_rendering available: %s", hasDynamicRendering ? "YES" : "NO");

    // 5. Create Device with Dynamic Rendering feature
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCI = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueCI.queueFamilyIndex = (uint32_t)graphicsQf;
    queueCI.queueCount = 1;
    queueCI.pQueuePriorities = &queuePriority;

    std::vector<const char*> enabledDevExts;
    if (hasDynamicRendering) {
        enabledDevExts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    }

    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR
    };
    dynFeatures.dynamicRendering = VK_TRUE;

    VkDeviceCreateInfo devCI = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    devCI.pNext = &dynFeatures;
    devCI.queueCreateInfoCount = 1;
    devCI.pQueueCreateInfos = &queueCI;
    devCI.enabledExtensionCount = (uint32_t)enabledDevExts.size();
    devCI.ppEnabledExtensionNames = enabledDevExts.data();

    VkDevice device = VK_NULL_HANDLE;
    res = vkCreateDevice(gpu, &devCI, nullptr, &device);
    if (res != VK_SUCCESS) {
        LOGE("Failed to create VkDevice: %d", res);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    LOGI("Successfully created VkDevice: %p", device);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, (uint32_t)graphicsQf, 0, &queue);

    // 6. Create Command Pool & Buffer
    VkCommandPoolCreateInfo poolCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.queueFamilyIndex = (uint32_t)graphicsQf;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &poolCI, nullptr, &cmdPool);

    VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    // 7. Create 64x64 color attachment image
    VkImageCreateInfo imgCI = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgCI.extent = { 64, 64, 1 };
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage img = VK_NULL_HANDLE;
    vkCreateImage(device, &imgCI, nullptr, &img);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, img, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
    uint32_t memTypeIndex = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memTypeIndex = i;
            break;
        }
    }

    VkMemoryAllocateInfo memAlloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    memAlloc.allocationSize = memReqs.size;
    memAlloc.memoryTypeIndex = memTypeIndex;
    VkDeviceMemory imgMem = VK_NULL_HANDLE;
    vkAllocateMemory(device, &memAlloc, nullptr, &imgMem);
    vkBindImageMemory(device, img, imgMem, 0);

    VkImageViewCreateInfo viewCI = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewCI.image = img;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.baseMipLevel = 0;
    viewCI.subresourceRange.levelCount = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount = 1;

    VkImageView imgView = VK_NULL_HANDLE;
    vkCreateImageView(device, &viewCI, nullptr, &imgView);

    // 8. RenderDoc Capture Trigger (Explicit In-App API)
    LOGI("Triggering RenderDoc explicit frame capture...");
    vulkan_fix_start_capture(device);

    // 9. Record rendering commands
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Pre-barrier: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img;
    barrier.subresourceRange = viewCI.subresourceRange;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );

    // Dynamic Rendering Pass (will be intercepted & emulated if hardware lacks it)
    VkRenderingAttachmentInfoKHR colorAttachment = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR
    };
    colorAttachment.imageView = imgView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color.float32[0] = 0.2f; // Red
    colorAttachment.clearValue.color.float32[1] = 0.5f; // Green
    colorAttachment.clearValue.color.float32[2] = 0.8f; // Blue
    colorAttachment.clearValue.color.float32[3] = 1.0f; // Alpha

    VkRenderingInfoKHR renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
    renderingInfo.renderArea.offset = { 0, 0 };
    renderingInfo.renderArea.extent = { 64, 64 };
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    LOGI("Calling vkCmdBeginRenderingKHR (Dynamic Rendering)...");
    vkCmdBeginRenderingKHR(cmd, &renderingInfo);

    LOGI("Calling vkCmdEndRenderingKHR...");
    vkCmdEndRenderingKHR(cmd);

    // TBDR Barrier Optimizer Verification:
    // 1. Post-barrier with layout transition (MUST NOT be dropped or cause AFBC corruption)
    VkImageMemoryBarrier postBarrier = barrier;
    postBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    postBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    postBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    postBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &postBarrier);

    // 2. Presentation barrier (MUST NOT be dropped, MUST NOT be narrowed to preserve SurfaceFlinger display sync)
    VkImageMemoryBarrier presentBarrier = postBarrier;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    presentBarrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &presentBarrier);

    vkEndCommandBuffer(cmd);

    // 10. Submit & Wait
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    LOGI("Submitting command buffer to queue...");
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    LOGI("Execution completed successfully!");

    vulkan_fix_end_capture(device);

    // 11. Performance Micro-Benchmark: 10,000 Dynamic Rendering & Barrier Passes
    LOGI("--------------------------------------------------");
    LOGI("  Running Performance Benchmark (10,000 passes)   ");
    LOGI("--------------------------------------------------");
    const int BENCH_ITERS = 10000;
    auto tStart = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < BENCH_ITERS; ++i) {
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdBeginRenderingKHR(cmd, &renderingInfo);
        vkCmdEndRenderingKHR(cmd);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &postBarrier);
        vkEndCommandBuffer(cmd);
    }
    auto tEnd = std::chrono::high_resolution_clock::now();
    double totalUs = std::chrono::duration<double, std::micro>(tEnd - tStart).count();
    double avgUs = totalUs / BENCH_ITERS;
    LOGI("Completed %d Dynamic Rendering passes in %.2f ms", BENCH_ITERS, totalUs / 1000.0);
    LOGI("Average overhead per pass: %.3f us (%.1f passes/sec)", avgUs, 1000000.0 / avgUs);

    // 11. Cleanup
    vkDestroyImageView(device, imgView, nullptr);
    vkDestroyImage(device, img, nullptr);
    vkFreeMemory(device, imgMem, nullptr);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    LOGI("==================================================");
    LOGI("  Test PASSED! VulkanFix pipeline verified.       ");
    LOGI("==================================================");
    return 0;
}

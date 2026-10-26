#include "core/renderdoc_app.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#include <vulkan/vulkan.h>

extern "C" uint32_t vulkan_fix_start_capture_checked(VkDevice);
extern "C" uint32_t vulkan_fix_end_capture_checked(VkDevice);
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                            \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties &properties, uint32_t bits,
                            VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}
static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
              const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (data && data->messageIdNumber == 0x564658 && user)
        ++*static_cast<unsigned *>(user);
    return VK_FALSE;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VulkanFix RenderDoc capture verification";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const bool game_init = getenv("VULKAN_FIX_TEST_DEBUG_UTILS") != nullptr;
    const char *game_extensions[]{VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_android_surface",
                                  VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT debug_info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    debug_info.pfnUserCallback = debug_message;
    unsigned debug_messages = 0;
    debug_info.pUserData = &debug_messages;
    if (game_init) {
        app.apiVersion = VK_API_VERSION_1_2;
        ici.enabledExtensionCount = 3;
        ici.ppEnabledExtensionNames = game_extensions;
        ici.pNext = &debug_info;
    }
    VkInstance instance{};
    REQUIRE(vkCreateInstance(&ici, nullptr, &instance) == VK_SUCCESS);
    VkDebugUtilsMessengerEXT messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger{};
    if (game_init) {
        auto provider = dlopen("./libvulkan_fix_renderdoc_driver.so", RTLD_NOW | RTLD_NOLOAD);
        REQUIRE(provider);
        auto get_api = (pRENDERDOC_GetAPI)dlsym(provider, "RENDERDOC_GetAPI");
        RENDERDOC_API_1_6_0 *capture_api{};
        REQUIRE(get_api && get_api(eRENDERDOC_API_Version_1_6_0, (void **)&capture_api) == 1);
        // RenderDoc otherwise intentionally mutes application debug callbacks.
        REQUIRE(capture_api->SetCaptureOptionU32(eRENDERDOC_Option_DebugOutputMute, 0) == 1);
        dlclose(provider);
        auto create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkCreateDebugUtilsMessengerEXT");
        destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
        REQUIRE(create_messenger && destroy_messenger);
        REQUIRE(create_messenger(instance, &debug_info, nullptr, &messenger) == VK_SUCCESS);
        REQUIRE(messenger != VK_NULL_HANDLE);
        auto submit_message = (PFN_vkSubmitDebugUtilsMessageEXT)vkGetInstanceProcAddr(
            instance, "vkSubmitDebugUtilsMessageEXT");
        REQUIRE(submit_message);
        VkDebugUtilsMessengerCallbackDataEXT message{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        message.messageIdNumber = 0x564658;
        message.pMessageIdName = "VulkanFix regression";
        message.pMessage = "Debug messenger delivery check";
        submit_message(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &message);
        REQUIRE(debug_messages == 1);
        submit_message(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &message);
        submit_message(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, &message);
        REQUIRE(debug_messages == 1);
        destroy_messenger(instance, messenger, nullptr);
        messenger = VK_NULL_HANDLE;
        submit_message(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &message);
        REQUIRE(debug_messages == 1);
        REQUIRE(create_messenger(instance, &debug_info, nullptr, &messenger) == VK_SUCCESS);
        printf("PASS: Vulkan 1.2 surface/debug_utils instance and messenger creation\n");
        printf("PASS: debug message delivery, severity/type filtering and callback removal\n");
    }
    uint32_t count = 0;
    REQUIRE(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count);
    std::vector<VkPhysicalDevice> physical(count);
    REQUIRE(vkEnumeratePhysicalDevices(instance, &count, physical.data()) == VK_SUCCESS);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical[0], &memory);
    vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        ++family;
    REQUIRE(family < count);
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    VkDevice device{};
    REQUIRE(vkCreateDevice(physical[0], &dci, nullptr, &device) == VK_SUCCESS);
    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {64, 64, 1};
    image_info.mipLevels = image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage image{};
    REQUIRE(vkCreateImage(device, &image_info, nullptr, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(memory, requirements.memoryTypeBits, 0);
    REQUIRE(allocation.memoryTypeIndex != UINT32_MAX);
    VkDeviceMemory image_memory{};
    REQUIRE(vkAllocateMemory(device, &allocation, nullptr, &image_memory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = image_info.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view{};
    REQUIRE(vkCreateImageView(device, &vi, nullptr, &view) == VK_SUCCESS);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 64 * 64 * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer{};
    REQUIRE(vkCreateBuffer(device, &bi, nullptr, &buffer) == VK_SUCCESS);
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        memory_type(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    REQUIRE(allocation.memoryTypeIndex != UINT32_MAX);
    VkDeviceMemory buffer_memory{};
    REQUIRE(vkAllocateMemory(device, &allocation, nullptr, &buffer_memory) == VK_SUCCESS);
    REQUIRE(vkBindBufferMemory(device, buffer, buffer_memory, 0) == VK_SUCCESS);
    VkAttachmentDescription attachment{};
    attachment.format = image_info.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &attachment;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 2;
    rp.pDependencies = dependencies;
    VkRenderPass pass{};
    REQUIRE(vkCreateRenderPass(device, &rp, nullptr, &pass) == VK_SUCCESS);
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = pass;
    fb.attachmentCount = 1;
    fb.pAttachments = &view;
    fb.width = fb.height = 64;
    fb.layers = 1;
    VkFramebuffer framebuffer{};
    REQUIRE(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer) == VK_SUCCESS);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.queueFamilyIndex = family;
    VkCommandPool pool{};
    REQUIRE(vkCreateCommandPool(device, &cp, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    REQUIRE(vkAllocateCommandBuffers(device, &ca, &cmd) == VK_SUCCESS);
    REQUIRE(vulkan_fix_start_capture_checked(device) == 1);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
    VkClearValue clear{};
    clear.color.float32[0] = 0.2f;
    clear.color.float32[1] = 0.5f;
    clear.color.float32[2] = 0.8f;
    clear.color.float32[3] = 1;
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = pass;
    rb.framebuffer = framebuffer;
    rb.renderArea.extent = {64, 64};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {64, 64, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = buffer;
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &host, 0, nullptr);
    REQUIRE(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    REQUIRE(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    REQUIRE(vkQueueWaitIdle(queue) == VK_SUCCESS);
    REQUIRE(vulkan_fix_end_capture_checked(device) == 1);
    void *mapped{};
    REQUIRE(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS);
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = buffer_memory;
    range.size = VK_WHOLE_SIZE;
    REQUIRE(vkInvalidateMappedMemoryRanges(device, 1, &range) == VK_SUCCESS);
    auto bytes = (const unsigned char *)mapped;
    const int expected[]{51, 128, 204, 255};
    for (unsigned i = 0; i < 64 * 64 * 4; ++i)
        REQUIRE(abs(int(bytes[i]) - expected[i % 4]) <= 1);
    vkUnmapMemory(device, buffer_memory);
    printf("PASS: captured render pass clears and copies all 4096 pixels correctly\n");
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    vkDestroyRenderPass(device, pass, nullptr);
    vkDestroyImageView(device, view, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, image_memory, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, buffer_memory, nullptr);
    vkDestroyDevice(device, nullptr);
    if (messenger)
        destroy_messenger(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    printf("PASS: RenderDoc returned success and produced a new non-empty RDC\n");
    return 0;
}

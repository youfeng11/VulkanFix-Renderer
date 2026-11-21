#include "core/native_probe_resources.h"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
using vkfix::nativeprobe::check;
namespace {
struct Context {
    VkInstance instance{}; VkDevice device{}; bool pending = false, retain_device = false;
    PFN_vkGetInstanceProcAddr gipa{}; PFN_vkGetDeviceProcAddr gdpa{};
    template<class T> T get(const char* name) { auto fn = (T)gipa(instance,name); if (!fn) throw std::runtime_error(name); return fn; }
    template<class T> T dev(const char* name) { auto fn = (T)gdpa(device,name); if (!fn) throw std::runtime_error(name); return fn; }
} p;
using Resources = vkfix::nativeprobe::Resources<Context>;
}
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if (argc < 2) { printf("Usage: singleton_group_gpu_test <layer.so> [--execute-zero-negative-control]\n"); return 2; }
    try {
        const bool khr = argc > 2 && !std::strcmp(argv[2],"--khr-bindings");
        auto library = dlopen(argv[1],RTLD_NOW | RTLD_LOCAL); if (!library) throw std::runtime_error(dlerror());
        p.gipa = (PFN_vkGetInstanceProcAddr)dlsym(library,"vkGetInstanceProcAddr");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
        check(p.get<PFN_vkCreateInstance>("vkCreateInstance")(&ici,nullptr,&p.instance),"instance");
        uint32_t count = 0; auto enumerate = p.get<PFN_vkEnumeratePhysicalDeviceGroups>("vkEnumeratePhysicalDeviceGroups");
        check(enumerate(p.instance,&count,nullptr),"device group count"); if (!count) throw std::runtime_error("no device group");
        std::vector<VkPhysicalDeviceGroupProperties> devices(count);
        for (auto& group : devices) group.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
        check(p.get<PFN_vkEnumeratePhysicalDeviceGroupsKHR>("vkEnumeratePhysicalDeviceGroupsKHR")(p.instance,&count,devices.data()),"device group aliases");
        if (devices[0].physicalDeviceCount != 1) throw std::runtime_error("non-native device-group operations exposed a multi-device group");
        VkPhysicalDeviceGroupProperties untouched{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES}; untouched.physicalDeviceCount = 99;
        uint32_t zero_capacity = 0;
        if (enumerate(p.instance,&zero_capacity,&untouched) != VK_INCOMPLETE || zero_capacity || untouched.physicalDeviceCount != 99)
            throw std::runtime_error("zero-capacity group enumeration changed caller payload");
        auto physical = devices[0].physicalDevices[0];
        auto queues = p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties"); queues(physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count); queues(physical,&count,families.data()); uint32_t family = count;
        for (uint32_t i = 0; i < count; ++i) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
        if (family == count) throw std::runtime_error("no graphics queue");
        float priority = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        VkDeviceGroupDeviceCreateInfo requested_group{VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO}; requested_group.physicalDeviceCount = 1; requested_group.pPhysicalDevices = &physical; dci.pNext = &requested_group;
        const char* binding_extension = VK_KHR_BIND_MEMORY_2_EXTENSION_NAME;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = &binding_extension;
        check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dci,nullptr,&p.device),"device"); p.gdpa = p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        if (dci.pNext != &requested_group || requested_group.pPhysicalDevices != &physical || requested_group.physicalDeviceCount != 1)
            throw std::runtime_error("group creation input mutated");
        {
            Resources gpu(p,physical,family);
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = 16; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            Resources::Buffer data{}; check(p.dev<PFN_vkCreateBuffer>("vkCreateBuffer")(p.device,&bci,nullptr,&data.handle),"data buffer");
            gpu.objects.push_back([=] { p.dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(p.device,data.handle,nullptr); });
            VkMemoryRequirements mr{}; p.dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(p.device,data.handle,&mr);
            VkMemoryAllocateFlagsInfo memory_group{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; memory_group.flags = VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT; memory_group.deviceMask = 1;
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.pNext = &memory_group; mai.allocationSize = mr.size; mai.memoryTypeIndex = gpu.memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            check(p.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(p.device,&mai,nullptr,&data.memory),"masked memory allocation");
            gpu.allocations.push_back([=] { p.dev<PFN_vkFreeMemory>("vkFreeMemory")(p.device,data.memory,nullptr); });
            uint32_t local_index = 0;
            VkBindBufferMemoryDeviceGroupInfo buffer_group{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO}; buffer_group.deviceIndexCount = 1; buffer_group.pDeviceIndices = &local_index;
            VkBindBufferMemoryInfo buffer_binding{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO}; buffer_binding.pNext = &buffer_group; buffer_binding.buffer = data.handle; buffer_binding.memory = data.memory;
            check(p.dev<PFN_vkBindBufferMemory2>(khr ? "vkBindBufferMemory2KHR" : "vkBindBufferMemory2")(p.device,1,&buffer_binding),"masked buffer binding");
            if (buffer_binding.pNext != &buffer_group || buffer_group.pDeviceIndices != &local_index) throw std::runtime_error("buffer group input mutated");
            check(p.dev<PFN_vkMapMemory>("vkMapMemory")(p.device,data.memory,0,VK_WHOLE_SIZE,0,&data.mapped),"map data");
            gpu.allocations.push_back([=] { p.dev<PFN_vkUnmapMemory>("vkUnmapMemory")(p.device,data.memory); });
            std::memset(data.mapped,0,16); VkMappedMemoryRange mapped{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; mapped.memory = data.memory; mapped.size = VK_WHOLE_SIZE;
            check(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,1,&mapped),"flush data");
            VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image_ci.flags = VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT;
            image_ci.imageType = VK_IMAGE_TYPE_2D; image_ci.format = VK_FORMAT_R8G8B8A8_UNORM; image_ci.extent = {16,16,1}; image_ci.mipLevels = image_ci.arrayLayers = 1;
            image_ci.samples = VK_SAMPLE_COUNT_1_BIT; image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VkImageFormatProperties format_properties{};
            check(p.get<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties")(physical,image_ci.format,image_ci.imageType,image_ci.tiling,image_ci.usage,image_ci.flags,&format_properties),"split image format query");
            VkPhysicalDeviceImageFormatInfo2 format_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
            format_info.format = image_ci.format; format_info.type = image_ci.imageType; format_info.tiling = image_ci.tiling; format_info.usage = image_ci.usage; format_info.flags = image_ci.flags;
            for (const char* name : {"vkGetPhysicalDeviceImageFormatProperties2","vkGetPhysicalDeviceImageFormatProperties2KHR"}) {
                VkImageFormatProperties2 output{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
                check(p.get<PFN_vkGetPhysicalDeviceImageFormatProperties2>(name)(physical,&format_info,&output),"split image format2 query");
                if (std::memcmp(&output.imageFormatProperties,&format_properties,sizeof(format_properties)) || format_info.flags != image_ci.flags) throw std::runtime_error("split image format queries differ or mutate input");
            }
            Resources::Image image{}; check(p.dev<PFN_vkCreateImage>("vkCreateImage")(p.device,&image_ci,nullptr,&image.handle),"split-capable image");
            gpu.objects.push_back([=] { p.dev<PFN_vkDestroyImage>("vkDestroyImage")(p.device,image.handle,nullptr); });
            if (image_ci.flags != VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) throw std::runtime_error("image creation input mutated");
            p.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(p.device,image.handle,&mr);
            auto image_memory = gpu.allocate(mr,0);
            VkRect2D whole_image{{0,0},{16,16}};
            VkBindImageMemoryDeviceGroupInfo image_group{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO}; image_group.splitInstanceBindRegionCount = 1; image_group.pSplitInstanceBindRegions = &whole_image;
            VkBindImageMemoryInfo image_binding{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO}; image_binding.pNext = &image_group; image_binding.image = image.handle; image_binding.memory = image_memory;
            check(p.dev<PFN_vkBindImageMemory2>(khr ? "vkBindImageMemory2KHR" : "vkBindImageMemory2")(p.device,1,&image_binding),"singleton whole-image split binding");
            if (image_group.splitInstanceBindRegionCount != 1 || image_binding.pNext != &image_group) throw std::runtime_error("image binding input mutated");
            VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view_ci.image = image.handle; view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D; view_ci.format = image_ci.format; view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            check(p.dev<PFN_vkCreateImageView>("vkCreateImageView")(p.device,&view_ci,nullptr,&image.view),"image view");
            gpu.objects.push_back([=] { p.dev<PFN_vkDestroyImageView>("vkDestroyImageView")(p.device,image.view,nullptr); });
            auto pixels = gpu.buffer(16*16*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VkAttachmentDescription attachment{}; attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL; attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
            VkSubpassDependency dependencies[] = {{VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_DEPENDENCY_DEVICE_GROUP_BIT},
                {0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_DEPENDENCY_DEVICE_GROUP_BIT}};
            VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rci.attachmentCount = rci.subpassCount = 1; rci.pAttachments = &attachment; rci.pSubpasses = &subpass; rci.dependencyCount = 2; rci.pDependencies = dependencies;
            VkRenderPass pass{}; check(p.dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(p.device,&rci,nullptr,&pass),"ordinary group-dependency pass");
            gpu.objects.push_back([=] { p.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(p.device,pass,nullptr); });
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fci.renderPass = pass; fci.attachmentCount = 1; fci.pAttachments = &image.view; fci.width = fci.height = 16; fci.layers = 1;
            VkFramebuffer fb{}; check(p.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(p.device,&fci,nullptr,&fb),"framebuffer");
            gpu.objects.push_back([=] { p.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(p.device,fb,nullptr); });
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = family;
            VkCommandPool pool{}; check(p.dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(p.device,&pci,nullptr,&pool),"command pool");
            gpu.objects.push_back([=] { p.dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(p.device,pool,nullptr); });
            VkCommandBuffer cmds[4]{}; VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.commandBufferCount = 4; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            check(p.dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(p.device,&cai,cmds),"commands");
            auto barrier = p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
            for (uint32_t i = 0; i < 4; ++i) {
                VkDeviceGroupCommandBufferBeginInfo initial{VK_STRUCTURE_TYPE_DEVICE_GROUP_COMMAND_BUFFER_BEGIN_INFO}; initial.deviceMask = 1;
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.pNext = &initial;
                if (i == 1) bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
                check(p.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cmds[i],&bi),"masked command begin");
                p.dev<PFN_vkCmdSetDeviceMask>("vkCmdSetDeviceMask")(cmds[i],1); p.dev<PFN_vkCmdSetDeviceMaskKHR>("vkCmdSetDeviceMaskKHR")(cmds[i],1);
                p.dev<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(cmds[i],data.handle,i*4,4,11*(i+1));
                if (i == 0) {
                    VkImageMemoryBarrier t{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; t.image = image.handle; t.newLayout = VK_IMAGE_LAYOUT_GENERAL; t.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    t.srcQueueFamilyIndex = t.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; t.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                    barrier(cmds[i],VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_DEPENDENCY_DEVICE_GROUP_BIT,0,nullptr,0,nullptr,1,&t);
                    VkClearColorValue zero{}; p.dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmds[i],image.handle,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&t.subresourceRange);
                    const VkRect2D area{{4,4},{8,8}};
                    VkDeviceGroupRenderPassBeginInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO}; group.deviceMask = group.deviceRenderAreaCount = 1; group.pDeviceRenderAreas = &area;
                    VkClearValue clear{}; clear.color.float32[1] = clear.color.float32[3] = 1;
                    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.pNext = &group; rp.renderPass = pass; rp.framebuffer = fb; rp.renderArea.extent = {16,16}; rp.clearValueCount = 1; rp.pClearValues = &clear;
                    p.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cmds[i],&rp,VK_SUBPASS_CONTENTS_INLINE); p.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cmds[i]);
                    if (group.deviceMask != 1 || group.pDeviceRenderAreas != &area || rp.pNext != &group) throw std::runtime_error("render group input mutated");
                }
                if (i == 2) {
                    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {16,16,1};
                    p.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmds[i],image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,pixels.handle,1,&copy);
                }
                gpu.host_barrier(cmds[i],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
                check(p.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cmds[i]),"command end");
                if (initial.deviceMask != 1 || bi.pNext != &initial) throw std::runtime_error("command group input mutated");
            }
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore semaphore{};
            check(p.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(p.device,&sci,nullptr,&semaphore),"semaphore"); gpu.objects.push_back([=] { p.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(p.device,semaphore,nullptr); });
            uint32_t indices[] = {0}, masks[] = {1,0,1}, zero = 0, one = 1;
            if (argc > 2 && !std::strcmp(argv[2],"--execute-zero-negative-control")) masks[1] = 1;
            VkDeviceGroupSubmitInfo groups[3]{}; VkSubmitInfo submits[3]{};
            for (uint32_t i = 0; i < 3; ++i) { groups[i].sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO; submits[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submits[i].pNext = &groups[i]; }
            submits[0].commandBufferCount = groups[0].commandBufferCount = 3; submits[0].pCommandBuffers = cmds; groups[0].pCommandBufferDeviceMasks = masks;
            submits[1].commandBufferCount = groups[1].commandBufferCount = 1; submits[1].pCommandBuffers = &cmds[1]; groups[1].pCommandBufferDeviceMasks = &zero;
            submits[1].signalSemaphoreCount = groups[1].signalSemaphoreCount = 1; submits[1].pSignalSemaphores = &semaphore; groups[1].pSignalSemaphoreDeviceIndices = indices;
            submits[2].commandBufferCount = groups[2].commandBufferCount = 1; submits[2].pCommandBuffers = &cmds[3]; groups[2].pCommandBufferDeviceMasks = &one;
            submits[2].waitSemaphoreCount = groups[2].waitSemaphoreCount = 1; submits[2].pWaitSemaphores = &semaphore; groups[2].pWaitSemaphoreDeviceIndices = indices;
            VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT; submits[2].pWaitDstStageMask = &wait;
            VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(p.dev<PFN_vkCreateFence>("vkCreateFence")(p.device,&fence_ci,nullptr,&fence),"fence");
            gpu.objects.push_back([=] { p.dev<PFN_vkDestroyFence>("vkDestroyFence")(p.device,fence,nullptr); });
            VkQueue queue{}; p.dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(p.device,family,0,&queue);
            check(p.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,3,submits,fence),"masked submit batch"); p.pending = true;
            check(p.dev<PFN_vkWaitForFences>("vkWaitForFences")(p.device,1,&fence,VK_TRUE,3000000000ull),"fence (3 seconds)"); p.pending = false;
            gpu.invalidate(data); gpu.invalidate(pixels);
            const uint32_t expected[] = {11,0,33,44};
            if (std::memcmp(data.mapped,expected,sizeof(expected))) {
                auto values = static_cast<const uint32_t*>(data.mapped); char message[160];
                std::snprintf(message,sizeof(message),"masked GPU writes got=(%u,%u,%u,%u), expected=(11,0,33,44)",values[0],values[1],values[2],values[3]);
                throw std::runtime_error(message);
            }
            const auto bytes = static_cast<const uint8_t*>(pixels.mapped);
            for (uint32_t pixel = 0; pixel < 256; ++pixel) {
                const bool inside = pixel%16 >= 4 && pixel%16 < 12 && pixel/16 >= 4 && pixel/16 < 12;
                const uint8_t expected_pixel[] = {0,uint8_t(inside ? 255 : 0),0,uint8_t(inside ? 255 : 0)};
                if (std::memcmp(bytes+pixel*4,expected_pixel,4)) throw std::runtime_error("ordinary singleton render area pixel mismatch");
            }
            if (submits[0].commandBufferCount != 3 || groups[0].pCommandBufferDeviceMasks != masks || mai.pNext != &memory_group || memory_group.flags != VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT)
                throw std::runtime_error("caller memory or submission input mutated");
            auto driver = dlopen(std::getenv("VULKAN_FIX_DRIVER_PATH"),RTLD_NOW | RTLD_LOCAL);
            auto rejected = (unsigned(*)())dlsym(driver,"singleton_group_test_rejected"); auto seen = (unsigned(*)())dlsym(driver,"singleton_group_test_seen");
            auto bind_calls = (unsigned(*)())dlsym(driver,"singleton_group_test_bind2_calls");
            auto group_enumerations = (unsigned(*)())dlsym(driver,"singleton_group_test_group_enumerations");
            auto group_creations = (unsigned(*)())dlsym(driver,"singleton_group_test_group_creations");
            const auto* bind1 = std::getenv("VULKANFIX_TEST_BIND1_ONLY"); const bool legacy_bind = bind1 && !std::strcmp(bind1,"1");
            if (!rejected || !seen || !bind_calls || !group_enumerations || !group_creations || rejected() || seen() != 1023 ||
                bind_calls() != (legacy_bind ? 0u : 2u) || !group_enumerations() || group_creations() != 1)
                throw std::runtime_error("strict API1.0 boundary rejected input, chose the wrong binding route or was not fully exercised");
            printf("PASS: exported layer, both group enumeration spellings, zero-capacity output, native singleton creation, %s binding spellings via %s backend, group allocation/buffer/image binding, split-image creation/3 format queries,256 pixels, ordinary render area/barrier, mixed/zero masks, binary signal/wait and fence\n",
                khr ? "KHR" : "core",legacy_bind ? "legacy bind1" : "native KHR bind2");
        }
        p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device,nullptr); p.device = VK_NULL_HANDLE;
        p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance,nullptr); return 0;
    } catch (const std::exception& e) { printf("FAIL: %s\n",e.what()); if (p.pending) std::_Exit(1); return 1; }
}

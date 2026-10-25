#include "core/vertex_store_emulator.h"
#include "vertex_store_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <spirv-tools/libspirv.hpp>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>

static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static void success(VkResult result, const char *message) {
    if (result != VK_SUCCESS) {
        std::printf("%s: VkResult %d\n", message, result);
        throw std::runtime_error(message);
    }
}
static VertexStoreEmulator::MemoryStats vertex_stats(VkDevice device) {
    VertexStoreEmulator::MemoryStats stats;
    require(vulkanfixGetVertexStoreMemoryStats(device, &stats) == VK_TRUE,
            "query active vertex emulation memory statistics");
    return stats;
}
struct Test {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkSemaphore timeline{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkDescriptorSetLayout set_layout{}, empty_layout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool descriptors{};
    struct Buffer {
        VkBuffer handle{};
        VkDeviceMemory memory{};
        void *mapped{};
    };
    struct Image {
        VkImage handle{};
        VkDeviceMemory memory{};
        VkImageView view{};
        uint32_t width, height;
    };
    std::vector<Buffer> buffers;
    std::vector<Image> images;
    std::vector<VkShaderModule> shaders;
    uint32_t memory(uint32_t allowed, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p{};
        vkGetPhysicalDeviceMemoryProperties(physical, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
            if ((allowed & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("memory type unavailable");
    }
    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void *data = nullptr) {
        Buffer b;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        success(vkCreateBuffer(device, &ci, nullptr, &b.handle), "create buffer");
        VkMemoryRequirements r{};
        vkGetBufferMemoryRequirements(device, b.handle, &r);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = r.size;
        ai.memoryTypeIndex = memory(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        success(vkAllocateMemory(device, &ai, nullptr, &b.memory), "allocate buffer");
        success(vkBindBufferMemory(device, b.handle, b.memory, 0), "bind buffer");
        success(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "map buffer");
        memset(b.mapped, 0, size);
        if (data)
            memcpy(b.mapped, data, size);
        buffers.push_back(b);
        return b;
    }
    Image image(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage) {
        Image im{};
        im.width = width;
        im.height = height;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {width, height, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        success(vkCreateImage(device, &ci, nullptr, &im.handle), "create image");
        VkMemoryRequirements r{};
        vkGetImageMemoryRequirements(device, im.handle, &r);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = r.size;
        ai.memoryTypeIndex = memory(r.memoryTypeBits, 0);
        success(vkAllocateMemory(device, &ai, nullptr, &im.memory), "allocate image");
        success(vkBindImageMemory(device, im.handle, im.memory, 0), "bind image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.handle;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        success(vkCreateImageView(device, &vi, nullptr, &im.view), "create image view");
        images.push_back(im);
        return im;
    }
    VkShaderModule shader(const char *text) {
        spvtools::SpirvTools compiler(SPV_ENV_VULKAN_1_0);
        compiler.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &,
                                       const char *m) { std::printf("SPIR-V: %s\n", m); });
        std::vector<uint32_t> words;
        require(compiler.Assemble(text, &words) && compiler.Validate(words),
                "assemble/validate fixture");
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = words.size() * 4;
        ci.pCode = words.data();
        VkShaderModule result{};
        success(vkCreateShaderModule(device, &ci, nullptr, &result), "create shader");
        shaders.push_back(result);
        return result;
    }
    void transition(VkCommandBuffer cmd, const Image &image, VkImageLayout old_layout,
                    VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = old_layout;
        b.newLayout = new_layout;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.handle;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    ~Test() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (timeline)
                vkDestroySemaphore(device, timeline, nullptr);
            if (pool)
                vkDestroyCommandPool(device, pool, nullptr);
            if (pipeline)
                vkDestroyPipeline(device, pipeline, nullptr);
            if (descriptors)
                vkDestroyDescriptorPool(device, descriptors, nullptr);
            if (layout)
                vkDestroyPipelineLayout(device, layout, nullptr);
            if (set_layout)
                vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            if (empty_layout)
                vkDestroyDescriptorSetLayout(device, empty_layout, nullptr);
            for (auto shader : shaders)
                vkDestroyShaderModule(device, shader, nullptr);
            for (auto b : buffers) {
                if (b.mapped)
                    vkUnmapMemory(device, b.memory);
                vkDestroyBuffer(device, b.handle, nullptr);
                vkFreeMemory(device, b.memory, nullptr);
            }
            for (auto im : images) {
                vkDestroyImageView(device, im.view, nullptr);
                vkDestroyImage(device, im.handle, nullptr);
                vkFreeMemory(device, im.memory, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
    void run(bool image_atomics = false, bool async_timeline = false, bool automatic = false,
             uint32_t frames = 1, bool optimized = false) {
        if (automatic) {
            unsetenv("VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE");
            unsetenv("VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS");
            unsetenv("FORCE_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE");
            unsetenv("FORCE_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS");
        } else {
            setenv("VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE", "force", 1);
        }
        setenv("VULKAN_FIX_EMULATE_DYNAMIC_RENDERING", "force", 1);
        setenv("VULKAN_FIX_EMULATE_DYNAMIC_RENDERING_FEATURE", "force", 1);
        setenv("VULKAN_FIX_OPTIMIZE_BARRIER", optimized ? "1" : "0", 1);
        setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
        if (async_timeline)
            setenv("VULKAN_FIX_EMULATE_TIMELINE_SEMAPHORE", "force", 1);
        setenv("VULKAN_FIX_LOG_PATH", "/data/local/tmp/vulkanfix-vertex-gpu.log", 1);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        success(vkCreateInstance(&ici, nullptr, &instance), "create instance");
        uint32_t count = 0;
        success(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPUs");
        require(count > 0, "no GPU");
        std::vector<VkPhysicalDevice> gpus(count);
        success(vkEnumeratePhysicalDevices(instance, &count, gpus.data()), "enumerate GPUs");
        physical = gpus[0];
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::printf("Device: %s\n", properties.deviceName);
        if (automatic) {
            auto driver = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
            require(driver != nullptr, "load native Vulkan feature provider");
            auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(driver, "vkGetInstanceProcAddr");
            require(gipa != nullptr, "native instance proc lookup");
            auto query =
                (PFN_vkGetPhysicalDeviceFeatures)gipa(instance, "vkGetPhysicalDeviceFeatures");
            require(query != nullptr, "native vertex feature query");
            VkPhysicalDeviceFeatures native{};
            query(physical, &native);
            dlclose(driver);
            std::printf("Native vertexPipelineStoresAndAtomics: %u\n",
                        native.vertexPipelineStoresAndAtomics);
            if (native.vertexPipelineStoresAndAtomics) {
                std::printf("SKIP: automatic fallback GPU case requires missing native support\n");
                return;
            }
            VkPhysicalDeviceFeatures exposed{};
            vkGetPhysicalDeviceFeatures(physical, &exposed);
            require(exposed.vertexPipelineStoresAndAtomics == VK_TRUE,
                    "default feature query enables missing vertex atomics");
            VkPhysicalDeviceFeatures2 exposed2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            vkGetPhysicalDeviceFeatures2(physical, &exposed2);
            require(exposed2.features.vertexPipelineStoresAndAtomics == VK_TRUE,
                    "default features2 query enables missing vertex atomics");
            std::printf(
                "PASS: both feature queries automatically supply missing native vertex atomics\n");
        }
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i)
            if ((queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                family = i;
                break;
            }
        require(family != UINT32_MAX, "no graphics+compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures features{};
        features.vertexPipelineStoresAndAtomics = VK_TRUE;
        VkPhysicalDeviceDynamicRenderingFeatures dynamic{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        dynamic.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        timeline_feature.timelineSemaphore = VK_TRUE;
        if (async_timeline)
            dynamic.pNext = &timeline_feature;
        const char *extensions[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                                    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &dynamic;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        dci.pEnabledFeatures = &features;
        dci.enabledExtensionCount = async_timeline ? 2 : 1;
        dci.ppEnabledExtensionNames = extensions;
        success(vkCreateDevice(physical, &dci, nullptr, &device), "create emulated device");
        if (async_timeline) {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
            success(vkCreateSemaphore(device, &create, nullptr, &timeline), "create draw timeline");
        }
        vkGetDeviceQueue(device, family, 0, &queue);
        require(vkGetDeviceProcAddr(device, "vkCmdBindDescriptorSets") ==
                    reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindDescriptorSets),
                "descriptor tracking via device proc lookup");
        require(vkGetDeviceProcAddr(device, "vkCmdDrawIndexed") ==
                    reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndexed),
                "draw tracking via device proc lookup");
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        success(vkCreateCommandPool(device, &pci, nullptr, &pool), "create command pool");
        const uint32_t initial[] = {0, 123};
        auto counters = buffer(sizeof(initial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, initial);
        const float positions[] = {0, 99, 99, -1, -1, 3, -1, -1, 3};
        auto vertices = buffer(sizeof(positions), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, positions);
        const uint32_t colors[] = {0, 0, 0xff00ff00, 0xff00ff00};
        auto color = buffer(sizeof(colors), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, colors);
        const uint16_t indices[] = {99, 2, 0, 1};
        auto index = buffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices);
        auto target = image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto storage = image(VK_FORMAT_R32_UINT, 3, 2,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        auto pixels = buffer(4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto values = buffer(24, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                       nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 2;
        sl.pBindings = bindings;
        success(vkCreateDescriptorSetLayout(device, &sl, nullptr, &set_layout),
                "create set layout");
        VkDescriptorSetLayoutCreateInfo empty{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        success(vkCreateDescriptorSetLayout(device, &empty, nullptr, &empty_layout),
                "create unused empty layout");
        VkDescriptorSetLayout original_sets[] = {set_layout, empty_layout};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2;
        pl.pSetLayouts = original_sets;
        success(vkCreatePipelineLayout(device, &pl, nullptr, &layout), "create pipeline layout");
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = sizes;
        success(vkCreateDescriptorPool(device, &dp, nullptr, &descriptors),
                "create descriptor pool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptors;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &set_layout;
        VkDescriptorSet set{};
        success(vkAllocateDescriptorSets(device, &da, &set), "allocate descriptors");
        VkDescriptorBufferInfo db{counters.handle, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo di{};
        di.imageView = storage.view;
        di.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = bindings[i].descriptorType;
        }
        writes[0].pBufferInfo = &db;
        writes[1].pImageInfo = &di;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto &stage : stages) {
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        std::string vertex_text = vertex_store_source;
        if (image_atomics) {
            auto replace = [&](const std::string &from, const std::string &to) {
                const auto where = vertex_text.find(from);
                require(where != std::string::npos, "image atomic fixture substitution");
                vertex_text.replace(where, from.size(), to);
            };
            replace("OpDecorate %image NonReadable", "");
            replace("%ptrimage = OpTypePointer UniformConstant %Image",
                    "%ptrimage = OpTypePointer UniformConstant %Image\n%ptrimageuint = "
                    "OpTypePointer Image %uint");
            replace("OpImageWrite %img %coord %texel",
                    "%atomicptr = OpImageTexelPointer %ptrimageuint %image %coord %zero\n%imageold "
                    "= OpAtomicIAdd %uint %atomicptr %one %zero %value");
        }
        stages[0].module = shader(vertex_text.c_str());
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = shader(vertex_store_fragment);
        VkVertexInputBindingDescription vb[] = {{0, 8, VK_VERTEX_INPUT_RATE_VERTEX},
                                                {1, 4, VK_VERTEX_INPUT_RATE_INSTANCE}};
        VkVertexInputAttributeDescription va[] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                  {1, 1, VK_FORMAT_R8G8B8A8_UNORM, 0}};
        VkPipelineVertexInputStateCreateInfo vi{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 2;
        vi.pVertexBindingDescriptions = vb;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions = va;
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, 1, 1, 0, 1};
        VkRect2D scissor{{0, 0}, {1, 1}};
        VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vs.viewportCount = vs.scissorCount = 1;
        vs.pViewports = &viewport;
        vs.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo bs{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        bs.attachmentCount = 1;
        bs.pAttachments = &ba;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &rendering;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vs;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &bs;
        ci.layout = layout;
        success(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline),
                "create compute/replay pipeline");
        const auto baseline = vertex_stats(device);
        if (frames > 1) {
            for (unsigned i = 0; i < 1000; ++i) {
                VkDescriptorSetLayout temporary_set{};
                success(vkCreateDescriptorSetLayout(device, &empty, nullptr, &temporary_set),
                        "create churn set layout");
                VkPipelineLayoutCreateInfo temporary_info{
                    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                temporary_info.setLayoutCount = 1;
                temporary_info.pSetLayouts = &temporary_set;
                VkPipelineLayout temporary_layout{};
                success(vkCreatePipelineLayout(device, &temporary_info, nullptr, &temporary_layout),
                        "create churn pipeline layout");
                vkDestroyDescriptorSetLayout(device, temporary_set, nullptr);
                vkDestroyPipelineLayout(device, temporary_layout, nullptr);
            }
            const auto after = vertex_stats(device);
            require(after.cloned_set_layouts == baseline.cloned_set_layouts &&
                        after.cloned_pipeline_layouts == baseline.cloned_pipeline_layouts,
                    "native clone count stays bounded after layout churn");
            std::printf("PASS: 1000 real layout create/destroy cycles return to baseline\n");
        }
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        success(vkAllocateCommandBuffers(device, &ca, &cmd), "allocate command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = target.view;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue.color.float32[2] = 1;
        attachment.clearValue.color.float32[3] = 1;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = scissor;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &attachment;
        uint64_t scratch_peak = 0;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            memcpy(counters.mapped, initial, sizeof(initial));
            success(vkBeginCommandBuffer(cmd, &begin), "begin command buffer");
            transition(cmd, target,
                       frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            transition(cmd, storage,
                       frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue cleared{};
            VkImageSubresourceRange storage_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, storage.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &cleared, 1, &storage_range);
            transition(cmd, storage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBeginRendering(cmd, &ri);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VkPipelineLayout recording_layout = layout;
            if (frames > 1)
                success(vkCreatePipelineLayout(device, &pl, nullptr, &recording_layout),
                        "create frame descriptor layout");
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, recording_layout, 0, 1,
                                    &set, 0, nullptr);
            if (frames > 1)
                vkDestroyPipelineLayout(device, recording_layout, nullptr);
            VkBuffer vertex_buffers[] = {vertices.handle, color.handle};
            VkDeviceSize offsets[] = {4, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, vertex_buffers, offsets);
            vkCmdDraw(cmd, 0, 2, 1, 2);
            vkCmdDraw(cmd, 3, 2, 1, 2);
            vkCmdBindIndexBuffer(cmd, index.handle, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cmd, 3, 2, 1, 1, 2);
            vkCmdEndRendering(cmd);
            transition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT);
            transition(cmd, storage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(cmd, target.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   pixels.handle, 1, &copy);
            copy.imageExtent = {3, 2, 1};
            vkCmdCopyImageToBuffer(cmd, storage.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   values.handle, 1, &copy);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
            success(vkEndCommandBuffer(cmd), "end command buffer");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            uint64_t draw_value = frame + 1;
            VkTimelineSemaphoreSubmitInfo timeline_submit{
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            if (async_timeline) {
                timeline_submit.signalSemaphoreValueCount = 1;
                timeline_submit.pSignalSemaphoreValues = &draw_value;
                submit.pNext = &timeline_submit;
                submit.signalSemaphoreCount = 1;
                submit.pSignalSemaphores = &timeline;
            }
            success(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit");
            success(vkQueueWaitIdle(queue), "wait GPU");
            if (async_timeline) {
                uint64_t observed = 0;
                success(vkGetSemaphoreCounterValue(device, timeline, &observed),
                        "query draw timeline");
                require(observed == draw_value, "compute/replay draw timeline completion");
                if (frame + 1 == frames)
                    std::printf("PASS: asynchronous timeline completes the compute/replay draw\n");
            }
            const auto counter = static_cast<uint32_t *>(counters.mapped);
            if (frame + 1 == frames)
                std::printf("SSBO counters: %u, %u\n", counter[0], counter[1]);
            require(counter[0] == 12 && counter[1] == 123, "SSBO atomic side effects");
            const auto pixel = static_cast<uint8_t *>(pixels.mapped);
            if (frame + 1 == frames)
                std::printf("Replay pixel: %u,%u,%u,%u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
            require(
                pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255,
                "vertex input fetch, normalized instance color and atomic-return output replay");
            const auto image_values = static_cast<uint32_t *>(values.mapped);
            for (uint32_t y = 0; y < 2; ++y)
                for (uint32_t x = 0; x < 3; ++x) {
                    const auto expected = ((y + 2) * 100 + x + 1) * (image_atomics ? 2 : 1);
                    if (frame + 1 == frames)
                        std::printf("Storage image %u,%u: %u\n", x, y, image_values[y * 3 + x]);
                    require(image_values[y * 3 + x] == expected,
                            "storage image writes, base vertex/index offset and instance index");
                }
            auto recorded = vertex_stats(device);
            if (!frame)
                scratch_peak = recorded.peak_scratch_bytes;
            require(recorded.live_draws == 2 && recorded.scratch_bytes != 0 &&
                        recorded.peak_scratch_bytes == scratch_peak,
                    "recorded GPU scratch allocation remains bounded across repeated frames");
            success(vkResetCommandBuffer(cmd, 0), "reset command buffer and scratch resources");
            auto reset = vertex_stats(device);
            require(reset.scratch_bytes == 0 && reset.live_draws == 0 &&
                        reset.cloned_pipeline_layouts == baseline.cloned_pipeline_layouts &&
                        reset.cloned_set_layouts == baseline.cloned_set_layouts,
                    "reset retires GPU scratch and destroyed recording-layout references");
            if (frames > 1 && ((frame + 1) % 1000 == 0 || frame + 1 == frames))
                std::printf(
                    "Stress frames=%u scratch=%llu peak=%llu clonedSets=%llu clonedLayouts=%llu\n",
                    frame + 1, (unsigned long long)reset.scratch_bytes,
                    (unsigned long long)reset.peak_scratch_bytes,
                    (unsigned long long)reset.cloned_set_layouts,
                    (unsigned long long)reset.cloned_pipeline_layouts);
        }
        std::printf(
            "PASS: %u GPU frames retain correct data and release scratch/layout resources\n",
            frames);
        success(vkBeginCommandBuffer(cmd, &begin), "begin rejection test");
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const uint32_t indirect_words[] = {3, 1, 0, 0};
        auto indirect =
            buffer(sizeof(indirect_words), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, indirect_words);
        vkCmdDrawIndirect(cmd, indirect.handle, 0, 1, sizeof(indirect_words));
        vkCmdEndRendering(cmd);
        require(vkEndCommandBuffer(cmd) == VK_ERROR_FEATURE_NOT_PRESENT,
                "unsupported indirect draw must fail recording");
        std::printf("PASS: unsupported indirect draw rejected without submission\n");
        std::printf("PASS: actual GPU compute/replay, SSBO atomics and storage image writes\n");
        vkDestroyCommandPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        require(vertex_stats(device).command_buffers == 0,
                "destroying a native command pool removes tracked commands");
        vkDestroyPipelineLayout(device, layout, nullptr);
        layout = VK_NULL_HANDLE;
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        set_layout = VK_NULL_HANDLE;
        vkDestroyDescriptorSetLayout(device, empty_layout, nullptr);
        empty_layout = VK_NULL_HANDLE;
        auto retained = vertex_stats(device);
        require(retained.cloned_pipeline_layouts == baseline.cloned_pipeline_layouts &&
                    retained.cloned_set_layouts == baseline.cloned_set_layouts,
                "live replay pipelines retain their original layout definitions");
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
        auto retired = vertex_stats(device);
        require(retired.cloned_pipeline_layouts == 0 && retired.cloned_set_layouts == 0,
                "destroying the last pipeline releases cloned layouts before device shutdown");
        std::printf(
            "PASS: native layout and command-pool lifetimes retire every final reference\n");
    }
};
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        Test test;
        bool image_atomics = false, async_timeline = false, automatic = false, optimized = false;
        uint32_t frames = 1;
        for (int i = 1; i < argc; ++i) {
            image_atomics |= !strcmp(argv[i], "--image-atomics");
            async_timeline |= !strcmp(argv[i], "--timeline");
            automatic |= !strcmp(argv[i], "--auto");
            optimized |= !strcmp(argv[i], "--optimized");
            if (!strncmp(argv[i], "--frames=", 9)) {
                char *end = nullptr;
                auto count = strtoul(argv[i] + 9, &end, 10);
                require(end && !*end && count > 0 && count <= 100000, "invalid stress frame count");
                frames = uint32_t(count);
            }
        }
        test.run(image_atomics, async_timeline, automatic, frames, optimized);
        return 0;
    } catch (const std::exception &error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}

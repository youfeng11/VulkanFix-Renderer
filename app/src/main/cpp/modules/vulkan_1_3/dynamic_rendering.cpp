#include "dynamic_rendering.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"
#include <string.h>
#include <algorithm>

REGISTER_LAYER_MODULE(DynamicRenderingModule);

static inline void hash_combine(size_t& seed, size_t v) {
    seed ^= v + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

size_t DynamicRenderingModule::PipelineRenderPassKeyHash::operator()(const PipelineRenderPassKey& k) const {
    size_t seed = 0;
    for (VkFormat f : k.colorFormats) {
        hash_combine(seed, std::hash<uint32_t>()((uint32_t)f));
    }
    hash_combine(seed, std::hash<uint32_t>()((uint32_t)k.depthFormat));
    hash_combine(seed, std::hash<uint32_t>()((uint32_t)k.stencilFormat));
    hash_combine(seed, std::hash<uint32_t>()((uint32_t)k.samples));
    hash_combine(seed, std::hash<uint32_t>()(k.viewMask));
    return seed;
}

size_t DynamicRenderingModule::DynamicRenderPassKeyHash::operator()(const DynamicRenderPassKey& k) const {
    size_t seed = 0;
    for (const auto& a : k.colorAttachments) {
        hash_combine(seed, (size_t)a.format);
        hash_combine(seed, (size_t)a.samples);
        hash_combine(seed, (size_t)a.loadOp);
        hash_combine(seed, (size_t)a.storeOp);
        hash_combine(seed, (size_t)a.initialLayout);
        hash_combine(seed, (size_t)a.finalLayout);
    }
    if (k.has_depth_stencil) {
        hash_combine(seed, (size_t)k.depthStencilAttachment.format);
        hash_combine(seed, (size_t)k.depthStencilAttachment.samples);
        hash_combine(seed, (size_t)k.depthStencilAttachment.loadOp);
        hash_combine(seed, (size_t)k.depthStencilAttachment.storeOp);
        hash_combine(seed, (size_t)k.depthStencilAttachment.stencilLoadOp);
        hash_combine(seed, (size_t)k.depthStencilAttachment.stencilStoreOp);
        hash_combine(seed, (size_t)k.depthStencilAttachment.initialLayout);
        hash_combine(seed, (size_t)k.depthStencilAttachment.finalLayout);
    }
    for (const auto& a : k.resolveAttachments) {
        hash_combine(seed, (size_t)a.format);
        hash_combine(seed, (size_t)a.initialLayout);
    }
    return seed;
}

size_t DynamicRenderingModule::FramebufferKeyHash::operator()(const FramebufferKey& k) const {
    size_t seed = 0;
    hash_combine(seed, (size_t)(uintptr_t)k.renderPass);
    for (VkImageView v : k.views) {
        hash_combine(seed, (size_t)(uintptr_t)v);
    }
    hash_combine(seed, (size_t)k.width);
    hash_combine(seed, (size_t)k.height);
    hash_combine(seed, (size_t)k.layers);
    return seed;
}

static VkImageLayout sanitize_color_layout(VkImageLayout layout) {
    if (layout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR) {
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR) {
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if (layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_GENERAL ||
        layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        return layout;
    }
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

static VkImageLayout sanitize_depth_layout(VkImageLayout layout) {
    if (layout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if (layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    if (layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_GENERAL ||
        layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        return layout;
    }
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

static bool is_tbdr_opt_enabled() {
    const char* env = getenv("VULKAN_FIX_OPTIMIZE_TBDR");
    if (env && (strcmp(env, "0") == 0 || strcasecmp(env, "false") == 0)) {
        return false;
    }
    return true;
}

static bool is_aggressive_depth_store_opt() {
    const char* env = getenv("FORCE_OPTIMIZE_DEPTH_STORE");
    return env && (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0);
}

DynamicRenderingModule::DynamicRenderingModule() {
    LOGI("Initialized Vulkan VK_KHR_dynamic_rendering emulation module");
}

bool DynamicRenderingModule::is_phys_device_native(VkPhysicalDevice physDev) {
    {
        std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
        auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
        if (it != m_phys_native_support.end()) {
            return it->second;
        }
    }

    const char* force_emu = getenv("FORCE_EMULATE_DYNAMIC_RENDERING");
    if (force_emu && (strcmp(force_emu, "1") == 0 || strcasecmp(force_emu, "true") == 0)) {
        LOGI("FORCE_EMULATE_DYNAMIC_RENDERING set, enabling emulation for physical device %p", physDev);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }

    bool native = false;
    PFN_vkEnumerateDeviceExtensionProperties real_ext_fn =
        (PFN_vkEnumerateDeviceExtensionProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    if (real_ext_fn) {
        uint32_t count = 0;
        if (real_ext_fn(physDev, NULL, &count, NULL) == VK_SUCCESS && count > 0) {
            std::vector<VkExtensionProperties> exts(count);
            if (real_ext_fn(physDev, NULL, &count, exts.data()) == VK_SUCCESS) {
                for (const auto& e : exts) {
                    if (strcmp(e.extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0) {
                        native = true;
                        break;
                    }
                }
            }
        }
    }

    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native VK_KHR_dynamic_rendering, enabling emulation layer!", physDev);
    } else {
        LOGI("Physical device %p natively supports VK_KHR_dynamic_rendering", physDev);
    }
    return native;
}

bool DynamicRenderingModule::is_device_native(VkDevice device) {
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    if (__builtin_expect(device == m_primary_dev.load(std::memory_order_relaxed), 1)) {
        return m_primary_native.load(std::memory_order_relaxed);
    }
    std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
    auto it = m_device_native_support.find((uint64_t)(uintptr_t)device);
    if (it != m_device_native_support.end()) {
        return it->second;
    }
    return false;
}

VkDevice DynamicRenderingModule::get_device_for_cmd(VkCommandBuffer cmd) {
    return LayerManager::get().get_device_for_cmd(cmd);
}

DynamicRenderingModule::ImageViewMeta DynamicRenderingModule::get_image_view_meta(VkImageView view) {
    std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
    auto it = m_image_views.find((uint64_t)(uintptr_t)view);
    if (it != m_image_views.end()) {
        return it->second;
    }
    return ImageViewMeta{};
}

VkFormat DynamicRenderingModule::get_image_view_format(VkImageView view) {
    std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
    auto it = m_image_views.find((uint64_t)(uintptr_t)view);
    if (it != m_image_views.end() && it->second.format != VK_FORMAT_UNDEFINED) {
        return it->second.format;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

VkSampleCountFlagBits DynamicRenderingModule::get_image_view_samples(VkImageView view) {
    std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
    auto it = m_image_views.find((uint64_t)(uintptr_t)view);
    if (it != m_image_views.end()) {
        return it->second.samples;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

VkExtent2D DynamicRenderingModule::get_image_view_extent(VkImageView view) {
    std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
    auto it = m_image_views.find((uint64_t)(uintptr_t)view);
    if (it != m_image_views.end()) {
        return it->second.extent;
    }
    return {0, 0};
}

void DynamicRenderingModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    bool has_ext = vku::has_extension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

    if (!has_ext) {
        VkExtensionProperties prop{};
        memset(&prop, 0, sizeof(prop));
        strncpy(prop.extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
        prop.specVersion = VK_KHR_DYNAMIC_RENDERING_SPEC_VERSION;
        extensions.push_back(prop);
        LOGI("Injected extension: %s (spec version %u)", VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, prop.specVersion);
    }
}

void DynamicRenderingModule::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    pUserData = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR);
    if (pUserData) {
        LOG_OPT_DEBUG("DynamicRendering: unlinked VkPhysicalDeviceDynamicRenderingFeaturesKHR from pFeatures2->pNext");
    }
}

void DynamicRenderingModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures) return;
    if (is_phys_device_native(physicalDevice)) return;

    if (pUserData) {
        auto* dynFeatures = vku::relink_pnext<VkPhysicalDeviceDynamicRenderingFeaturesKHR>(
            pFeatures->pNext, pUserData);
        dynFeatures->dynamicRendering = VK_TRUE;
        LOG_OPT_DEBUG("DynamicRendering: supplied dynamicRendering = VK_TRUE in VkPhysicalDeviceDynamicRenderingFeaturesKHR");
    } else {
        auto* dyn = vku::find_pnext_mut<VkPhysicalDeviceDynamicRenderingFeaturesKHR>(
            pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR);
        if (dyn) {
            dyn->dynamicRendering = VK_TRUE;
            LOG_OPT_DEBUG("DynamicRendering: supplied dynamicRendering = VK_TRUE in VkPhysicalDeviceDynamicRenderingFeaturesKHR");
        }
    }

    auto* v13 = vku::find_pnext_mut<VkPhysicalDeviceVulkan13Features>(
        pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    if (v13) {
        v13->dynamicRendering = VK_TRUE;
        LOG_OPT_DEBUG("DynamicRendering: supplied dynamicRendering = VK_TRUE in VkPhysicalDeviceVulkan13Features");
    }
}

void DynamicRenderingModule::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // 1. Strip extension from enabledExtensions
    if (vku::strip_extension(enabledExtensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
        LOGI("vkCreateDevice: stripped %s from enabledExtensions", VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    }

    // 2. Unlink VkPhysicalDeviceDynamicRenderingFeaturesKHR from pCreateInfo->pNext
    pUserData = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR);
    if (pUserData) {
        LOGI("vkCreateDevice: unlinked VkPhysicalDeviceDynamicRenderingFeaturesKHR from pNext");
    }

    // 3. Disable dynamicRendering in VkPhysicalDeviceVulkan13Features if chained
    auto* v13 = vku::find_pnext_mut<VkPhysicalDeviceVulkan13Features>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    if (v13) {
        v13->dynamicRendering = VK_FALSE;
        LOGI("vkCreateDevice: disabled dynamicRendering in VkPhysicalDeviceVulkan13Features");
    }
}

void DynamicRenderingModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_device_native_support[(uint64_t)(uintptr_t)device] = native;
        if (m_primary_dev.load(std::memory_order_relaxed) == VK_NULL_HANDLE) {
            m_primary_native.store(native, std::memory_order_relaxed);
            m_primary_dev.store(device, std::memory_order_release);
        }
    }
}

void DynamicRenderingModule::on_destroy_device(VkDevice device) {
    if (m_primary_dev.load(std::memory_order_relaxed) == device) {
        m_primary_dev.store(VK_NULL_HANDLE, std::memory_order_relaxed);
        m_primary_native.store(false, std::memory_order_relaxed);
    }

    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);

    PFN_vkDestroyFramebuffer real_destroy_fb = (PFN_vkDestroyFramebuffer)
        get_real_proc(get_last_instance(), device, "vkDestroyFramebuffer");
    PFN_vkDestroyRenderPass real_destroy_rp = (PFN_vkDestroyRenderPass)
        get_real_proc(get_last_instance(), device, "vkDestroyRenderPass");

    auto it = m_device_resources.find((uint64_t)(uintptr_t)device);
    if (it != m_device_resources.end()) {
        if (real_destroy_fb) {
            for (VkFramebuffer fb : it->second.framebuffers) {
                real_destroy_fb(device, fb, NULL);
            }
        }
        if (real_destroy_rp) {
            for (VkRenderPass rp : it->second.renderPasses) {
                real_destroy_rp(device, rp, NULL);
            }
        }
        m_device_resources.erase(it);
    }

    m_device_native_support.erase((uint64_t)(uintptr_t)device);
    m_pipeline_rp_cache.clear();
    m_dynamic_rp_cache.clear();
    m_framebuffer_cache.clear();
}

bool DynamicRenderingModule::needs_pipeline_interception(
    VkDevice device,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos
) {
    if (is_device_native(device)) return false;
    if (!pCreateInfos || createInfoCount == 0) return false;

    for (uint32_t i = 0; i < createInfoCount; ++i) {
        if (pCreateInfos[i].renderPass == VK_NULL_HANDLE) {
            if (vku::has_pnext(pCreateInfos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR)) {
                return true;
            }
        }
    }
    return false;
}

VkRenderPass DynamicRenderingModule::get_or_create_pipeline_render_pass(
    VkDevice device,
    uint32_t colorAttachmentCount,
    const VkFormat* pColorAttachmentFormats,
    VkFormat depthAttachmentFormat,
    VkFormat stencilAttachmentFormat,
    VkSampleCountFlagBits samples,
    uint32_t viewMask
) {
    PipelineRenderPassKey key;
    if (colorAttachmentCount > 0 && pColorAttachmentFormats != NULL) {
        key.colorFormats.assign(pColorAttachmentFormats, pColorAttachmentFormats + colorAttachmentCount);
    }
    key.depthFormat = depthAttachmentFormat;
    key.stencilFormat = stencilAttachmentFormat;
    key.samples = samples;
    key.viewMask = viewMask;

    {
        std::shared_lock<std::shared_mutex> readLock(m_rw_mutex);
        auto it = m_pipeline_rp_cache.find(key);
        if (it != m_pipeline_rp_cache.end()) {
            return it->second;
        }
    }

    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> colorRefs;
    VkAttachmentReference depthRef{};
    bool has_depth = false;

    for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
        VkFormat fmt = pColorAttachmentFormats ? pColorAttachmentFormats[i] : VK_FORMAT_UNDEFINED;
        if (fmt != VK_FORMAT_UNDEFINED) {
            VkAttachmentDescription desc{};
            desc.format = fmt;
            desc.samples = samples;
            desc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            desc.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkAttachmentReference ref{};
            ref.attachment = (uint32_t) attachments.size();
            ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            attachments.push_back(desc);
            colorRefs.push_back(ref);
        } else {
            VkAttachmentReference ref{};
            ref.attachment = VK_ATTACHMENT_UNUSED;
            ref.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            colorRefs.push_back(ref);
        }
    }

    VkFormat dsFormat = (depthAttachmentFormat != VK_FORMAT_UNDEFINED) ? depthAttachmentFormat : stencilAttachmentFormat;
    if (dsFormat != VK_FORMAT_UNDEFINED) {
        has_depth = true;
        VkAttachmentDescription desc{};
        desc.format = dsFormat;
        desc.samples = samples;
        desc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        desc.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        desc.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        desc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        depthRef.attachment = (uint32_t) attachments.size();
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        attachments.push_back(desc);
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = (uint32_t) colorRefs.size();
    subpass.pColorAttachments = colorRefs.empty() ? NULL : colorRefs.data();
    subpass.pDepthStencilAttachment = has_depth ? &depthRef : NULL;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = (uint32_t) attachments.size();
    rpInfo.pAttachments = attachments.empty() ? NULL : attachments.data();
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;

    PFN_vkCreateRenderPass real_create_rp = (PFN_vkCreateRenderPass)
        get_real_proc(get_last_instance(), device, "vkCreateRenderPass");
    if (!real_create_rp) return VK_NULL_HANDLE;

    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult res = real_create_rp(device, &rpInfo, NULL, &rp);
    if (res == VK_SUCCESS && rp != VK_NULL_HANDLE) {
        std::unique_lock<std::shared_mutex> writeLock(m_rw_mutex);
        m_pipeline_rp_cache[key] = rp;
        m_device_resources[(uint64_t)(uintptr_t)device].renderPasses.push_back(rp);
        LOGI("Created cached pipeline compatible renderPass %p", (void*)(uintptr_t)rp);
        return rp;
    }

    LOGE("Failed to create cached pipeline compatible renderPass: %d", res);
    return VK_NULL_HANDLE;
}

void DynamicRenderingModule::on_modify_pipeline_create_info(
    VkDevice device,
    uint32_t index,
    VkGraphicsPipelineCreateInfo& createInfo,
    VkPipelineVertexInputStateCreateInfo& viState,
    std::vector<void*>& allocationsToFree
) {
    if (is_device_native(device)) return;
    if (createInfo.renderPass != VK_NULL_HANDLE) return;

    const auto* renderingCreateInfo = vku::find_pnext<VkPipelineRenderingCreateInfoKHR>(
        createInfo.pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR);
    if (!renderingCreateInfo) return;

    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    if (createInfo.pMultisampleState) {
        samples = createInfo.pMultisampleState->rasterizationSamples;
    }

    VkRenderPass compatRP = get_or_create_pipeline_render_pass(
        device,
        renderingCreateInfo->colorAttachmentCount,
        renderingCreateInfo->pColorAttachmentFormats,
        renderingCreateInfo->depthAttachmentFormat,
        renderingCreateInfo->stencilAttachmentFormat,
        samples,
        renderingCreateInfo->viewMask
    );

    if (compatRP != VK_NULL_HANDLE) {
        createInfo.renderPass = compatRP;
        createInfo.subpass = 0;
        LOG_OPT_DEBUG("Pipeline %u: assigned compatible renderPass %p", index, (void*)(uintptr_t)compatRP);
    }

    // Unlink VkPipelineRenderingCreateInfoKHR from pNext
    if (vku::remove_pnext(createInfo.pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR)) {
        LOG_OPT_DEBUG("Pipeline %u: unlinked VkPipelineRenderingCreateInfoKHR from pNext", index);
    }
}

void DynamicRenderingModule::on_post_allocate_command_buffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkResult result,
    VkCommandBuffer* pCommandBuffers
) {
    if (result == VK_SUCCESS && pAllocateInfo && pCommandBuffers) {
        std::lock_guard<std::mutex> lock(m_cmd_mutex);
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            m_cmd_devices[(uint64_t)(uintptr_t)pCommandBuffers[i]] = device;
        }
    }
}

void DynamicRenderingModule::on_free_command_buffers(
    VkDevice device,
    uint32_t count,
    const VkCommandBuffer* pCommandBuffers
) {
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        m_cmd_devices.erase((uint64_t)(uintptr_t)pCommandBuffers[i]);
        m_cmd_rendering_states.erase((uint64_t)(uintptr_t)pCommandBuffers[i]);
    }
}

void DynamicRenderingModule::on_begin_command_buffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo
) {
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    m_cmd_rendering_states.erase((uint64_t)(uintptr_t)commandBuffer);
}

void DynamicRenderingModule::on_reset_command_buffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags
) {
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    m_cmd_rendering_states.erase((uint64_t)(uintptr_t)commandBuffer);
}

void DynamicRenderingModule::on_pre_begin_command_buffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo,
    VkCommandBufferBeginInfo& modBeginInfo,
    VkCommandBufferInheritanceInfo& modInheritanceInfo,
    bool& modifiedInheritance
) {
    modifiedInheritance = false;
    if (!pBeginInfo || !pBeginInfo->pInheritanceInfo) return;

    VkDevice device = get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return;

    const VkCommandBufferInheritanceInfo* inInherit = pBeginInfo->pInheritanceInfo;
    if (inInherit->renderPass == VK_NULL_HANDLE) {
        const auto* rInfo = vku::find_pnext<VkCommandBufferInheritanceRenderingInfoKHR>(
            inInherit->pNext, VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO_KHR);
        if (rInfo) {
            VkRenderPass compatRP = get_or_create_pipeline_render_pass(
                device,
                rInfo->colorAttachmentCount,
                rInfo->pColorAttachmentFormats,
                rInfo->depthAttachmentFormat,
                rInfo->stencilAttachmentFormat,
                rInfo->rasterizationSamples,
                rInfo->viewMask
            );

            if (compatRP != VK_NULL_HANDLE) {
                modInheritanceInfo = *inInherit;
                modInheritanceInfo.renderPass = compatRP;
                modInheritanceInfo.subpass = 0;

                vku::remove_pnext(modInheritanceInfo.pNext, VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO_KHR);

                modBeginInfo = *pBeginInfo;
                modBeginInfo.pInheritanceInfo = &modInheritanceInfo;
                modifiedInheritance = true;
                LOG_OPT_DEBUG("Secondary cmd %p: substituted inheritance renderPass %p", (void*)commandBuffer, (void*)(uintptr_t)compatRP);
            }
        }
    }
}

void DynamicRenderingModule::on_post_create_image(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    VkResult result,
    VkImage image
) {
    if (result == VK_SUCCESS && pCreateInfo && image != VK_NULL_HANDLE) {
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        ImageMeta meta;
        meta.format = pCreateInfo->format;
        meta.samples = pCreateInfo->samples;
        meta.extent = pCreateInfo->extent;
        meta.usage = pCreateInfo->usage;
        m_images[(uint64_t)(uintptr_t)image] = meta;
    }
}

void DynamicRenderingModule::on_destroy_image(
    VkDevice device,
    VkImage image
) {
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_images.erase((uint64_t)(uintptr_t)image);
}

void DynamicRenderingModule::on_post_create_image_view(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    VkResult result,
    VkImageView imageView
) {
    if (result == VK_SUCCESS && pCreateInfo && imageView != VK_NULL_HANDLE) {
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        ImageViewMeta meta;
        meta.format = pCreateInfo->format;
        meta.image = pCreateInfo->image;
        meta.samples = VK_SAMPLE_COUNT_1_BIT;
        meta.extent = {0, 0};
        meta.usage = 0;

        auto it = m_images.find((uint64_t)(uintptr_t)pCreateInfo->image);
        if (it != m_images.end()) {
            meta.samples = it->second.samples;
            meta.extent.width = it->second.extent.width;
            meta.extent.height = it->second.extent.height;
            meta.usage = it->second.usage;
        }

        m_image_views[(uint64_t)(uintptr_t)imageView] = meta;
    }
}

void DynamicRenderingModule::on_destroy_image_view(
    VkDevice device,
    VkImageView imageView
) {
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_image_views.erase((uint64_t)(uintptr_t)imageView);

    PFN_vkDestroyFramebuffer real_destroy_fb = (PFN_vkDestroyFramebuffer)
        get_real_proc(get_last_instance(), device, "vkDestroyFramebuffer");

    for (auto it = m_framebuffer_cache.begin(); it != m_framebuffer_cache.end(); ) {
        bool uses_view = false;
        for (VkImageView v : it->first.views) {
            if (v == imageView) {
                uses_view = true;
                break;
            }
        }
        if (uses_view) {
            if (real_destroy_fb) {
                real_destroy_fb(device, it->second, NULL);
            }
            it = m_framebuffer_cache.erase(it);
        } else {
            ++it;
        }
    }
}

bool DynamicRenderingModule::on_cmd_begin_rendering(
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo* pRenderingInfo
) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    if (is_device_native(device) || !pRenderingInfo) return false;

    const uint32_t colorCount = pRenderingInfo->colorAttachmentCount;

    // Fast-path MRU (Most Recently Used) Fingerprint Cache:
    // If the rendering attachments and render area match the previous call on this thread,
    // directly execute using cached renderPass & framebuffer without any heap allocation or lock.
    struct ThreadMRU {
        uint64_t fingerprint = 0;
        VkDevice device = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };
    thread_local ThreadMRU t_mru{};

    uint64_t fp = (uint64_t)colorCount;
    for (uint32_t i = 0; i < colorCount; ++i) {
        const auto& att = pRenderingInfo->pColorAttachments[i];
        fp ^= ((uint64_t)(uintptr_t)att.imageView >> 3) + 0x9e3779b97f4a7c15ULL + (fp << 6) + (fp >> 2);
        fp ^= ((uint64_t)att.loadOp << 4) ^ ((uint64_t)att.storeOp << 8) ^ ((uint64_t)att.imageLayout << 16);
        if (att.resolveImageView != VK_NULL_HANDLE) {
            fp ^= ((uint64_t)(uintptr_t)att.resolveImageView >> 3) + 0x517cc1b727220a95ULL;
        }
    }
    if (pRenderingInfo->pDepthAttachment && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE) {
        fp ^= ((uint64_t)(uintptr_t)pRenderingInfo->pDepthAttachment->imageView >> 3) + 0x9e3779b97f4a7c15ULL;
        fp ^= ((uint64_t)pRenderingInfo->pDepthAttachment->loadOp << 24) ^ ((uint64_t)pRenderingInfo->pDepthAttachment->storeOp << 28);
    }
    if (pRenderingInfo->pStencilAttachment && pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE) {
        fp ^= ((uint64_t)(uintptr_t)pRenderingInfo->pStencilAttachment->imageView >> 3) + 0x517cc1b727220a95ULL;
        fp ^= ((uint64_t)pRenderingInfo->pStencilAttachment->loadOp << 32) ^ ((uint64_t)pRenderingInfo->pStencilAttachment->storeOp << 36);
    }
    fp ^= ((uint64_t)pRenderingInfo->renderArea.offset.x << 40) ^ ((uint64_t)pRenderingInfo->renderArea.offset.y << 44);
    fp ^= ((uint64_t)pRenderingInfo->renderArea.extent.width << 48) ^ ((uint64_t)pRenderingInfo->renderArea.extent.height << 56);

    if (__builtin_expect(fp != 0 && t_mru.fingerprint == fp && t_mru.device == device &&
                         t_mru.renderPass != VK_NULL_HANDLE && t_mru.framebuffer != VK_NULL_HANDLE, 1)) {
        constexpr uint32_t SBO_CLEAR_LIMIT = 8;
        VkClearValue stackClears[SBO_CLEAR_LIMIT];
        uint32_t clearCount = 0;
        for (uint32_t i = 0; i < colorCount && clearCount < SBO_CLEAR_LIMIT; ++i) {
            if (pRenderingInfo->pColorAttachments[i].imageView != VK_NULL_HANDLE) {
                stackClears[clearCount++] = pRenderingInfo->pColorAttachments[i].clearValue;
            }
        }
        if (pRenderingInfo->pDepthAttachment && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE && clearCount < SBO_CLEAR_LIMIT) {
            VkClearValue dsClear{};
            dsClear.depthStencil.depth = pRenderingInfo->pDepthAttachment->clearValue.depthStencil.depth;
            if (pRenderingInfo->pStencilAttachment) {
                dsClear.depthStencil.stencil = pRenderingInfo->pStencilAttachment->clearValue.depthStencil.stencil;
            } else {
                dsClear.depthStencil.stencil = pRenderingInfo->pDepthAttachment->clearValue.depthStencil.stencil;
            }
            stackClears[clearCount++] = dsClear;
        }

        {
            std::lock_guard<std::mutex> cmdLock(m_cmd_mutex);
            CmdRenderingState& state = m_cmd_rendering_states[(uint64_t)(uintptr_t)commandBuffer];
            state.device = device;
            state.is_rendering = true;
            state.activeRenderPass = t_mru.renderPass;
            state.activeFramebuffer = t_mru.framebuffer;
        }

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = t_mru.renderPass;
        beginInfo.framebuffer = t_mru.framebuffer;
        beginInfo.renderArea = pRenderingInfo->renderArea;
        beginInfo.clearValueCount = clearCount;
        beginInfo.pClearValues = clearCount > 0 ? stackClears : NULL;

        VkSubpassContents contents = (pRenderingInfo->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR) ?
            VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE;

        const auto& dt = LayerManager::get().get_dispatch_table(device);
        if (dt.CmdBeginRenderPass) {
            dt.CmdBeginRenderPass(commandBuffer, &beginInfo, contents);
        } else {
            PFN_vkCmdBeginRenderPass real_begin_rp = (PFN_vkCmdBeginRenderPass)
                get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass");
            if (!real_begin_rp) return false;
            real_begin_rp(commandBuffer, &beginInfo, contents);
        }
        return true;
    }

    const uint32_t maxAttCount = colorCount * 2 + 1;

    DynamicRenderPassKey rpKey;
    rpKey.colorAttachments.reserve(colorCount);

    std::vector<VkAttachmentDescription> attachments;
    attachments.reserve(maxAttCount);

    std::vector<VkAttachmentReference> colorRefs;
    colorRefs.reserve(colorCount);

    std::vector<VkAttachmentReference> resolveRefs;
    VkAttachmentReference depthRef{};

    std::vector<VkImageView> fbViews;
    fbViews.reserve(maxAttCount);

    std::vector<VkClearValue> clearValues;
    clearValues.reserve(maxAttCount);

    VkImageView dsView = VK_NULL_HANDLE;
    if (pRenderingInfo->pDepthAttachment && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE) {
        dsView = pRenderingInfo->pDepthAttachment->imageView;
    } else if (pRenderingInfo->pStencilAttachment && pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE) {
        dsView = pRenderingInfo->pStencilAttachment->imageView;
    }

    bool has_resolves = false;
    for (uint32_t i = 0; i < colorCount; ++i) {
        if (pRenderingInfo->pColorAttachments[i].resolveImageView != VK_NULL_HANDLE) {
            has_resolves = true;
            break;
        }
    }
    if (has_resolves) {
        rpKey.resolveAttachments.reserve(colorCount);
        resolveRefs.reserve(colorCount);
    }

    // Acquire lock once for view metadata and cache lookups (concurrent multi-thread read)
    std::shared_lock<std::shared_mutex> readLock(m_rw_mutex);

    // 1. Color Attachments
    for (uint32_t i = 0; i < colorCount; ++i) {
        const auto& att = pRenderingInfo->pColorAttachments[i];
        if (att.imageView != VK_NULL_HANDLE) {
            auto it = m_image_views.find((uint64_t)(uintptr_t)att.imageView);
            VkFormat fmt = (it != m_image_views.end() && it->second.format != VK_FORMAT_UNDEFINED) ?
                           it->second.format : VK_FORMAT_R8G8B8A8_UNORM;
            VkSampleCountFlagBits samples = (it != m_image_views.end()) ?
                                           it->second.samples : VK_SAMPLE_COUNT_1_BIT;

            DynamicRenderPassKey::AttachmentDesc aDesc;
            aDesc.format = fmt;
            aDesc.samples = samples;
            aDesc.loadOp = att.loadOp;
            aDesc.storeOp = att.storeOp;
            aDesc.initialLayout = sanitize_color_layout(att.imageLayout);
            aDesc.finalLayout = aDesc.initialLayout;
            aDesc.refLayout = aDesc.initialLayout;

            // TBDR optimization: Prevent loading uninitialized DDR data into Tile memory
            if (is_tbdr_opt_enabled() && aDesc.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                if (aDesc.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
                    aDesc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                }
            }
            rpKey.colorAttachments.push_back(aDesc);

            VkAttachmentDescription vkDesc{};
            vkDesc.format = aDesc.format;
            vkDesc.samples = aDesc.samples;
            vkDesc.loadOp = aDesc.loadOp;
            vkDesc.storeOp = aDesc.storeOp;
            vkDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            vkDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            vkDesc.initialLayout = aDesc.initialLayout;
            vkDesc.finalLayout = aDesc.finalLayout;

            VkAttachmentReference ref{};
            ref.attachment = (uint32_t) attachments.size();
            ref.layout = aDesc.refLayout;

            attachments.push_back(vkDesc);
            colorRefs.push_back(ref);
            fbViews.push_back(att.imageView);
            clearValues.push_back(att.clearValue);
        } else {
            DynamicRenderPassKey::AttachmentDesc aDesc;
            aDesc.format = VK_FORMAT_UNDEFINED;
            rpKey.colorAttachments.push_back(aDesc);

            VkAttachmentReference ref{};
            ref.attachment = VK_ATTACHMENT_UNUSED;
            ref.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            colorRefs.push_back(ref);
        }
    }

    // 2. Depth/Stencil Attachment
    if (dsView != VK_NULL_HANDLE) {
        rpKey.has_depth_stencil = true;
        DynamicRenderPassKey::AttachmentDesc& dsDesc = rpKey.depthStencilAttachment;
        auto it = m_image_views.find((uint64_t)(uintptr_t)dsView);
        dsDesc.format = (it != m_image_views.end() && it->second.format != VK_FORMAT_UNDEFINED) ?
                        it->second.format : VK_FORMAT_R8G8B8A8_UNORM;
        dsDesc.samples = (it != m_image_views.end()) ?
                         it->second.samples : VK_SAMPLE_COUNT_1_BIT;

        if (pRenderingInfo->pDepthAttachment && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE) {
            dsDesc.loadOp = pRenderingInfo->pDepthAttachment->loadOp;
            dsDesc.storeOp = pRenderingInfo->pDepthAttachment->storeOp;
            dsDesc.initialLayout = sanitize_depth_layout(pRenderingInfo->pDepthAttachment->imageLayout);
        }
        if (pRenderingInfo->pStencilAttachment && pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE) {
            dsDesc.stencilLoadOp = pRenderingInfo->pStencilAttachment->loadOp;
            dsDesc.stencilStoreOp = pRenderingInfo->pStencilAttachment->storeOp;
            if (dsDesc.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                dsDesc.initialLayout = sanitize_depth_layout(pRenderingInfo->pStencilAttachment->imageLayout);
            }
        }

        // TBDR mobile optimizations for depth/stencil attachments:
        if (is_tbdr_opt_enabled()) {
            // 1. Prevent loading undefined contents from DDR into on-chip Tile memory
            if (dsDesc.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                if (dsDesc.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
                    dsDesc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                }
                if (dsDesc.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
                    dsDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                }
            }

            // 2. Intelligent DONT_CARE storeOp conversion:
            // Check if image usage indicates it is transient or will never be sampled/copied
            VkImageUsageFlags dsUsage = (it != m_image_views.end()) ? it->second.usage : 0;
            bool isTransient = (dsUsage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0;
            bool cannotBeSampled = (dsUsage != 0) &&
                ((dsUsage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) == 0);

            if (isTransient || cannotBeSampled || is_aggressive_depth_store_opt()) {
                if (dsDesc.storeOp == VK_ATTACHMENT_STORE_OP_STORE) {
                    dsDesc.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                }
                if (dsDesc.stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE) {
                    dsDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                }
            }
        }

        dsDesc.finalLayout = dsDesc.initialLayout;
        dsDesc.refLayout = dsDesc.initialLayout;

        VkAttachmentDescription vkDesc{};
        vkDesc.format = dsDesc.format;
        vkDesc.samples = dsDesc.samples;
        vkDesc.loadOp = dsDesc.loadOp;
        vkDesc.storeOp = dsDesc.storeOp;
        vkDesc.stencilLoadOp = dsDesc.stencilLoadOp;
        vkDesc.stencilStoreOp = dsDesc.stencilStoreOp;
        vkDesc.initialLayout = dsDesc.initialLayout;
        vkDesc.finalLayout = dsDesc.finalLayout;

        depthRef.attachment = (uint32_t) attachments.size();
        depthRef.layout = dsDesc.refLayout;

        attachments.push_back(vkDesc);
        fbViews.push_back(dsView);

        VkClearValue dsClear{};
        if (pRenderingInfo->pDepthAttachment) {
            dsClear.depthStencil.depth = pRenderingInfo->pDepthAttachment->clearValue.depthStencil.depth;
        }
        if (pRenderingInfo->pStencilAttachment) {
            dsClear.depthStencil.stencil = pRenderingInfo->pStencilAttachment->clearValue.depthStencil.stencil;
        } else if (pRenderingInfo->pDepthAttachment) {
            dsClear.depthStencil.stencil = pRenderingInfo->pDepthAttachment->clearValue.depthStencil.stencil;
        }
        clearValues.push_back(dsClear);
    }

    // 3. Resolve Attachments
    if (has_resolves) {
        for (uint32_t i = 0; i < colorCount; ++i) {
            const auto& att = pRenderingInfo->pColorAttachments[i];
            if (att.resolveImageView != VK_NULL_HANDLE) {
                auto it = m_image_views.find((uint64_t)(uintptr_t)att.resolveImageView);
                VkFormat fmt = (it != m_image_views.end() && it->second.format != VK_FORMAT_UNDEFINED) ?
                               it->second.format : VK_FORMAT_R8G8B8A8_UNORM;

                DynamicRenderPassKey::AttachmentDesc rDesc;
                rDesc.format = fmt;
                rDesc.samples = VK_SAMPLE_COUNT_1_BIT;
                rDesc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                rDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                rDesc.initialLayout = sanitize_color_layout(att.resolveImageLayout);
                rDesc.finalLayout = rDesc.initialLayout;
                rDesc.refLayout = rDesc.initialLayout;
                rpKey.resolveAttachments.push_back(rDesc);

                VkAttachmentDescription vkDesc{};
                vkDesc.format = rDesc.format;
                vkDesc.samples = rDesc.samples;
                vkDesc.loadOp = rDesc.loadOp;
                vkDesc.storeOp = rDesc.storeOp;
                vkDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                vkDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                vkDesc.initialLayout = rDesc.initialLayout;
                vkDesc.finalLayout = rDesc.finalLayout;

                VkAttachmentReference ref{};
                ref.attachment = (uint32_t) attachments.size();
                ref.layout = rDesc.refLayout;

                attachments.push_back(vkDesc);
                resolveRefs.push_back(ref);
                fbViews.push_back(att.resolveImageView);
                clearValues.push_back(VkClearValue{});
            } else {
                VkAttachmentReference ref{};
                ref.attachment = VK_ATTACHMENT_UNUSED;
                ref.layout = VK_IMAGE_LAYOUT_UNDEFINED;
                resolveRefs.push_back(ref);
            }
        }
    }

    // Framebuffer dimensions calculation
    uint32_t fb_w = pRenderingInfo->renderArea.offset.x + pRenderingInfo->renderArea.extent.width;
    uint32_t fb_h = pRenderingInfo->renderArea.offset.y + pRenderingInfo->renderArea.extent.height;
    if (fb_w == 0 || fb_h == 0) {
        for (VkImageView v : fbViews) {
            auto it = m_image_views.find((uint64_t)(uintptr_t)v);
            if (it != m_image_views.end()) {
                fb_w = std::max(fb_w, it->second.extent.width);
                fb_h = std::max(fb_h, it->second.extent.height);
            }
        }
        if (fb_w == 0) fb_w = 1;
        if (fb_h == 0) fb_h = 1;
    }
    uint32_t fb_layers = (pRenderingInfo->layerCount > 0) ? pRenderingInfo->layerCount : 1;

    // 4. Cache Lookup
    VkRenderPass renderPass = VK_NULL_HANDLE;
    auto rpIt = m_dynamic_rp_cache.find(rpKey);
    if (rpIt != m_dynamic_rp_cache.end()) {
        renderPass = rpIt->second;
    }

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    FramebufferKey fbKey;
    if (renderPass != VK_NULL_HANDLE) {
        fbKey.renderPass = renderPass;
        fbKey.views = fbViews;
        fbKey.width = fb_w;
        fbKey.height = fb_h;
        fbKey.layers = fb_layers;

        auto fbIt = m_framebuffer_cache.find(fbKey);
        if (fbIt != m_framebuffer_cache.end()) {
            framebuffer = fbIt->second;
        }
    }

    // Fast-path: both renderPass and framebuffer already cached (common runtime hot path)
    if (__builtin_expect(renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE, 1)) {
        // Release read lock before proceeding
        readLock.unlock();

        {
            std::lock_guard<std::mutex> cmdLock(m_cmd_mutex);
            CmdRenderingState& state = m_cmd_rendering_states[(uint64_t)(uintptr_t)commandBuffer];
            state.device = device;
            state.is_rendering = true;
            state.activeRenderPass = renderPass;
            state.activeFramebuffer = framebuffer;
        }

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = renderPass;
        beginInfo.framebuffer = framebuffer;
        beginInfo.renderArea = pRenderingInfo->renderArea;
        beginInfo.clearValueCount = (uint32_t) clearValues.size();
        beginInfo.pClearValues = clearValues.empty() ? NULL : clearValues.data();

        VkSubpassContents contents = (pRenderingInfo->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR) ?
            VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE;

        const auto& dt = LayerManager::get().get_dispatch_table(device);
        if (dt.CmdBeginRenderPass) {
            dt.CmdBeginRenderPass(commandBuffer, &beginInfo, contents);
        } else {
            PFN_vkCmdBeginRenderPass real_begin_rp = (PFN_vkCmdBeginRenderPass)
                get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass");
            if (!real_begin_rp) return false;
            real_begin_rp(commandBuffer, &beginInfo, contents);
        }

        t_mru.fingerprint = fp;
        t_mru.device = device;
        t_mru.renderPass = renderPass;
        t_mru.framebuffer = framebuffer;

        LOG_OPT_DEBUG("DynamicRendering: emulated vkCmdBeginRenderingKHR for cmd %p (fast path)", commandBuffer);
        return true;
    }

    // Cold path: unlock read lock before calling Vulkan driver creation APIs
    readLock.unlock();

    if (renderPass == VK_NULL_HANDLE) {
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = (uint32_t) colorRefs.size();
        subpass.pColorAttachments = colorRefs.empty() ? NULL : colorRefs.data();
        subpass.pDepthStencilAttachment = (dsView != VK_NULL_HANDLE) ? &depthRef : NULL;
        subpass.pResolveAttachments = has_resolves ? resolveRefs.data() : NULL;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = (uint32_t) attachments.size();
        rpInfo.pAttachments = attachments.empty() ? NULL : attachments.data();
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;

        PFN_vkCreateRenderPass real_create_rp = (PFN_vkCreateRenderPass)
            get_real_proc(get_last_instance(), device, "vkCreateRenderPass");
        if (!real_create_rp) return false;

        VkResult res = real_create_rp(device, &rpInfo, NULL, &renderPass);
        if (res != VK_SUCCESS || renderPass == VK_NULL_HANDLE) {
            LOGE("DynamicRendering: failed to create renderPass: %d", res);
            return false;
        }

        {
            std::unique_lock<std::shared_mutex> writeLock(m_rw_mutex);
            auto [it, inserted] = m_dynamic_rp_cache.emplace(rpKey, renderPass);
            if (!inserted) {
                PFN_vkDestroyRenderPass real_destroy_rp = (PFN_vkDestroyRenderPass)
                    get_real_proc(get_last_instance(), device, "vkDestroyRenderPass");
                if (real_destroy_rp) real_destroy_rp(device, renderPass, NULL);
                renderPass = it->second;
            } else {
                m_device_resources[(uint64_t)(uintptr_t)device].renderPasses.push_back(renderPass);
            }
        }
        LOG_OPT_DEBUG("DynamicRendering: created dynamic renderPass %p", (void*)(uintptr_t)renderPass);
    }

    if (framebuffer == VK_NULL_HANDLE) {
        fbKey.renderPass = renderPass;
        fbKey.views = fbViews;
        fbKey.width = fb_w;
        fbKey.height = fb_h;
        fbKey.layers = fb_layers;

        {
            std::shared_lock<std::shared_mutex> checkLock(m_rw_mutex);
            auto it = m_framebuffer_cache.find(fbKey);
            if (it != m_framebuffer_cache.end()) {
                framebuffer = it->second;
            }
        }

        if (framebuffer == VK_NULL_HANDLE) {
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass;
            fbInfo.attachmentCount = (uint32_t) fbViews.size();
            fbInfo.pAttachments = fbViews.empty() ? NULL : fbViews.data();
            fbInfo.width = fb_w;
            fbInfo.height = fb_h;
            fbInfo.layers = fb_layers;

            PFN_vkCreateFramebuffer real_create_fb = (PFN_vkCreateFramebuffer)
                get_real_proc(get_last_instance(), device, "vkCreateFramebuffer");
            if (!real_create_fb) return false;

            VkResult res = real_create_fb(device, &fbInfo, NULL, &framebuffer);
            if (res != VK_SUCCESS || framebuffer == VK_NULL_HANDLE) {
                LOGE("DynamicRendering: failed to create framebuffer: %d", res);
                return false;
            }

            {
                std::unique_lock<std::shared_mutex> writeLock(m_rw_mutex);
                auto [it, inserted] = m_framebuffer_cache.emplace(fbKey, framebuffer);
                if (!inserted) {
                    PFN_vkDestroyFramebuffer real_destroy_fb = (PFN_vkDestroyFramebuffer)
                        get_real_proc(get_last_instance(), device, "vkDestroyFramebuffer");
                    if (real_destroy_fb) real_destroy_fb(device, framebuffer, NULL);
                    framebuffer = it->second;
                } else {
                    m_device_resources[(uint64_t)(uintptr_t)device].framebuffers.push_back(framebuffer);
                }
            }
            LOG_OPT_DEBUG("DynamicRendering: created framebuffer %p (%ux%u)", (void*)(uintptr_t)framebuffer, fb_w, fb_h);
        }
    }

    // 6. Begin RenderPass
    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = renderPass;
    beginInfo.framebuffer = framebuffer;
    beginInfo.renderArea = pRenderingInfo->renderArea;
    beginInfo.clearValueCount = (uint32_t) clearValues.size();
    beginInfo.pClearValues = clearValues.empty() ? NULL : clearValues.data();

    VkSubpassContents contents = (pRenderingInfo->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR) ?
        VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.CmdBeginRenderPass) {
        dt.CmdBeginRenderPass(commandBuffer, &beginInfo, contents);
    } else {
        PFN_vkCmdBeginRenderPass real_begin_rp = (PFN_vkCmdBeginRenderPass)
            get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass");
        if (!real_begin_rp) return false;
        real_begin_rp(commandBuffer, &beginInfo, contents);
    }

    {
        std::lock_guard<std::mutex> cmdLock(m_cmd_mutex);
        CmdRenderingState& state = m_cmd_rendering_states[(uint64_t)(uintptr_t)commandBuffer];
        state.device = device;
        state.is_rendering = true;
        state.activeRenderPass = renderPass;
        state.activeFramebuffer = framebuffer;
    }

    t_mru.fingerprint = fp;
    t_mru.device = device;
    t_mru.renderPass = renderPass;
    t_mru.framebuffer = framebuffer;

    LOG_OPT_DEBUG("DynamicRendering: emulated vkCmdBeginRenderingKHR for cmd %p", commandBuffer);
    return true;
}

bool DynamicRenderingModule::on_cmd_end_rendering(VkCommandBuffer commandBuffer) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (dt.CmdEndRenderPass) {
        dt.CmdEndRenderPass(commandBuffer);
    } else {
        PFN_vkCmdEndRenderPass real_end_rp = (PFN_vkCmdEndRenderPass)
            get_real_proc(get_last_instance(), device, "vkCmdEndRenderPass");
        if (real_end_rp) {
            real_end_rp(commandBuffer);
        }
    }

    {
        std::lock_guard<std::mutex> cmdLock(m_cmd_mutex);
        auto it = m_cmd_rendering_states.find((uint64_t)(uintptr_t)commandBuffer);
        if (it != m_cmd_rendering_states.end()) {
            it->second.is_rendering = false;
        }
    }

    LOG_OPT_DEBUG("DynamicRendering: emulated vkCmdEndRenderingKHR for cmd %p", commandBuffer);
    return true;
}

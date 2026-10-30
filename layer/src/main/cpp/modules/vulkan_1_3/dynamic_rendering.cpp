#include "dynamic_rendering.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"
#include "vk_debug_utils.h"
#include "core/layout_compat.h"
#include "core/driver_trust.h"
#include "core/native_instance_state.h"
#include <string.h>
#include <algorithm>

REGISTER_LAYER_MODULE(DynamicRenderingModule);

static inline void hash_combine(size_t& seed, size_t v) {
    seed ^= v + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

size_t DynamicRenderingModule::PipelineRenderPassKeyHash::operator()(const PipelineRenderPassKey& k) const {
    size_t seed = std::hash<VkDevice>{}(k.device);
    for (uint32_t i = 0; i < k.colorAttachmentCount; ++i) {
        hash_combine(seed, std::hash<uint32_t>()((uint32_t)k.colorFormats[i]));
    }
    hash_combine(seed, std::hash<uint32_t>()((uint32_t)k.depthFormat));
    hash_combine(seed, std::hash<uint32_t>()((uint32_t)k.stencilFormat));
    hash_combine(seed, std::hash<uint32_t>()((uint32_t)k.samples));
    hash_combine(seed, std::hash<uint32_t>()(k.viewMask));
    return seed;
}

size_t DynamicRenderingModule::DynamicRenderPassKeyHash::operator()(const DynamicRenderPassKey& k) const {
    size_t seed = std::hash<VkDevice>{}(k.device);
    for (uint32_t i = 0; i < k.colorAttachmentCount; ++i) {
        const auto& a = k.colorAttachments[i];
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
    for (uint32_t i = 0; i < k.resolveAttachmentCount; ++i) {
        const auto& a = k.resolveAttachments[i];
        hash_combine(seed, (size_t)a.format);
        hash_combine(seed, (size_t)a.initialLayout);
    }
    return seed;
}

size_t DynamicRenderingModule::FramebufferKeyHash::operator()(const FramebufferKey& k) const {
    size_t seed = std::hash<VkDevice>{}(k.device);
    hash_combine(seed, std::hash<VkRenderPass>{}(k.renderPass));
    for (uint32_t i = 0; i < k.viewCount; ++i) {
        hash_combine(seed, std::hash<VkImageView>{}(k.views[i]));
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
        layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
        layout == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR) {
        return layout;
    }
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

static VkImageLayout sanitize_depth_layout(VkImageLayout layout) {
    if (layout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR ||
        layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if (layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR ||
        layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL) {
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
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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

    EmulationMode mode = parse_emulation_mode("VULKAN_FIX_EMULATE_DYNAMIC_RENDERING", "FORCE_EMULATE_DYNAMIC_RENDERING");
    if (mode == EmulationMode::ForceEmulate) {
        LOGI("Dynamic Rendering: FORCE EMULATE enabled for physical device %p", physDev);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = false;
        return false;
    }
    if (mode == EmulationMode::Skip) {
        LOGI("Dynamic Rendering: SKIP/BYPASS enabled for physical device %p", physDev);
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_phys_native_support[(uint64_t)(uintptr_t)physDev] = true;
        return true;
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

    if (native) {
        auto instance=vkfix::native_instance_for_physical(physDev);
        if (!instance) instance=get_last_instance();
        const auto trust=vkfix::native_driver_trust(instance,physDev,get_real_instance_proc_addr());
        if (vkfix::use_bounded_tiler_guard(trust.tiler_budget,getenv("VULKAN_FIX_MALI_TILER_GUARD"))) {
            native=false;
            LOGI("Dynamic rendering uses preserving render-pass continuations: tiler pressure %s (%s)",
                vkfix::driver_probe_verdict_name(trust.tiler_budget.verdict),trust.tiler_budget.detail.c_str());
        }
    }
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p uses dynamic rendering emulation", physDev);
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

    EmulationMode featMode = parse_emulation_mode("VULKAN_FIX_EMULATE_DYNAMIC_RENDERING_FEATURE", "FORCE_EMULATE_DYNAMIC_RENDERING_FEATURE");
    bool should_inject_feature = false;
    if (featMode == EmulationMode::ForceEmulate) {
        should_inject_feature = true;
    } else if (featMode == EmulationMode::Skip) {
        should_inject_feature = false;
    } else {
        should_inject_feature = !is_phys_device_native(physicalDevice);
    }

    if (!should_inject_feature) return;

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
        auto instance=vkfix::native_instance_for_physical(physicalDevice);
        if (!instance) instance=get_last_instance();
        const auto trust=vkfix::native_driver_trust(instance,physicalDevice,get_real_instance_proc_addr());
        const char* guard = getenv("VULKAN_FIX_MALI_TILER_GUARD");
        const bool tilerGuard = !native && vkfix::use_bounded_tiler_guard(trust.tiler_budget,guard);
        LOGI("Driver probe tiler pressure: %s (%s); bounded rendering=%u",
            vkfix::driver_probe_verdict_name(trust.tiler_budget.verdict),trust.tiler_budget.detail.c_str(),unsigned(tilerGuard));
        std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
        m_device_native_support[(uint64_t)(uintptr_t)device] = native;
        if (tilerGuard) {
            m_tiler_devices.insert((uint64_t)(uintptr_t)device);
            m_has_tiler_guard.store(true, std::memory_order_release);
            LOGI("Bounded tiler rendering: legacy + emulated dynamic rendering (4 indirect commands / 128 draws / 262144 direct vertices per batch)");
        }
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

    for (auto legacy = m_legacy_continuations.begin(); legacy != m_legacy_continuations.end();) {
        if (legacy->first.device == device) {
            if (real_destroy_rp) real_destroy_rp(device, legacy->second, nullptr);
            legacy = m_legacy_continuations.erase(legacy);
        } else ++legacy;
    }

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
    m_tiler_devices.erase((uint64_t)(uintptr_t)device);
    m_has_tiler_guard.store(!m_tiler_devices.empty(), std::memory_order_release);

    // Drop per-command-buffer state belonging to this device. Without this the
    // maps keep growing across device create/destroy cycles (e.g. world reloads)
    // and stale entries shadow handles that the driver later reuses.
    {
        std::lock_guard<std::mutex> cmdLock(m_cmd_mutex);
        for (auto cmdIt = m_cmd_devices.begin(); cmdIt != m_cmd_devices.end();) {
            if (cmdIt->second == device) {
                m_cmd_rendering_states.erase(cmdIt->first);
                m_primary_commands.erase(cmdIt->first);
                m_split_blockers.erase(cmdIt->first);
                cmdIt = m_cmd_devices.erase(cmdIt);
            } else {
                ++cmdIt;
            }
        }
    }

    // Erase only this device. Cached handles from a destroyed device must never
    // be returned to a surviving/new device, even when handle values coincide.
    auto eraseDevice = [device](auto& cache) {
        for (auto entry = cache.begin(); entry != cache.end();) {
            if (entry->first.device == device) entry = cache.erase(entry);
            else ++entry;
        }
    };
    eraseDevice(m_pipeline_rp_cache);
    eraseDevice(m_dynamic_rp_cache);
    eraseDevice(m_framebuffer_cache);
    eraseDevice(m_image_view_to_framebuffers);
    eraseDevice(m_images);
    eraseDevice(m_image_views);
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
    uint32_t effectiveColorCount = 0;
    if (pColorAttachmentFormats != NULL) {
        for (uint32_t i = colorAttachmentCount; i > 0; --i) {
            if (pColorAttachmentFormats[i - 1] != VK_FORMAT_UNDEFINED) {
                effectiveColorCount = i;
                break;
            }
        }
    }

    PipelineRenderPassKey key;
    key.device = device;
    key.colorAttachmentCount = effectiveColorCount;
    key.colorFormats.resize(effectiveColorCount);
    if (effectiveColorCount > 0 && pColorAttachmentFormats != NULL) {
        for (uint32_t i = 0; i < key.colorAttachmentCount; ++i) {
            key.colorFormats[i] = pColorAttachmentFormats[i];
        }
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

    for (uint32_t i = 0; i < effectiveColorCount; ++i) {
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
        auto inserted = m_pipeline_rp_cache.emplace(key, rp);
        if (!inserted.second) {
            auto destroy = (PFN_vkDestroyRenderPass)get_real_proc(
                get_last_instance(), device, "vkDestroyRenderPass");
            if (destroy) destroy(device, rp, nullptr);
            return inserted.first->second;
        }
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
            auto key = (uint64_t)(uintptr_t)pCommandBuffers[i];
            if (pAllocateInfo->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) m_primary_commands.insert(key);
            else m_primary_commands.erase(key);
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
        m_primary_commands.erase((uint64_t)(uintptr_t)pCommandBuffers[i]);
        m_split_blockers.erase((uint64_t)(uintptr_t)pCommandBuffers[i]);
    }
}

void DynamicRenderingModule::on_begin_command_buffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo
) {
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    auto state = m_cmd_rendering_states.find((uint64_t)(uintptr_t)commandBuffer);
    if (state != m_cmd_rendering_states.end()) {
        state->second.is_rendering = false;
        state->second.continuation = VK_NULL_HANDLE;
        state->second.attachmentViews.clear();
        state->second.budget = {};
    }
    m_split_blockers.erase((uint64_t)(uintptr_t)commandBuffer);
}

void DynamicRenderingModule::on_reset_command_buffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags
) {
    on_begin_command_buffer(commandBuffer, nullptr);
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
        m_images[vkfix::DeviceObjectKey::make(device, image)] = meta;
    }
}

void DynamicRenderingModule::on_destroy_image(
    VkDevice device,
    VkImage image
) {
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    m_images.erase(vkfix::DeviceObjectKey::make(device, image));
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

        auto it = m_images.find(vkfix::DeviceObjectKey::make(device, pCreateInfo->image));
        if (it != m_images.end()) {
            meta.samples = it->second.samples;
            const uint32_t mip = pCreateInfo->subresourceRange.baseMipLevel;
            meta.extent.width = std::max(1u, mip < 32 ? it->second.extent.width >> mip : 0u);
            meta.extent.height = std::max(1u, mip < 32 ? it->second.extent.height >> mip : 0u);
            meta.usage = it->second.usage;
            if (meta.format == VK_FORMAT_UNDEFINED) {
                meta.format = it->second.format;
            }
        }

        m_image_views[vkfix::DeviceObjectKey::make(device, imageView)] = meta;
    }
}

void DynamicRenderingModule::on_destroy_image_view(
    VkDevice device,
    VkImageView imageView
) {
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    const auto viewKey = vkfix::DeviceObjectKey::make(device, imageView);
    m_image_views.erase(viewKey);

    auto listIt = m_image_view_to_framebuffers.find(viewKey);
    if (listIt != m_image_view_to_framebuffers.end()) {
        PFN_vkDestroyFramebuffer real_destroy_fb = (PFN_vkDestroyFramebuffer)
            get_real_proc(get_last_instance(), device, "vkDestroyFramebuffer");

        auto keys = std::move(listIt->second);
        m_image_view_to_framebuffers.erase(listIt);
        for (const FramebufferKey& fbKey : keys) {
            auto fbIt = m_framebuffer_cache.find(fbKey);
            if (fbIt != m_framebuffer_cache.end()) {
                if (real_destroy_fb) {
                    real_destroy_fb(device, fbIt->second, NULL);
                }
                auto resources = m_device_resources.find((uint64_t)(uintptr_t)device);
                if (resources != m_device_resources.end()) {
                    resources->second.framebuffers.erase(fbIt->second);
                }
                m_framebuffer_cache.erase(fbIt);
            }
            // Remove the framebuffer from every sibling view as well. A depth
            // view can outlive thousands of transient color/resolve targets.
            for (VkImageView sibling : fbKey.views) {
                auto peer = m_image_view_to_framebuffers.find(
                    vkfix::DeviceObjectKey::make(device, sibling));
                if (peer == m_image_view_to_framebuffers.end()) continue;
                peer->second.erase(fbKey);
                if (peer->second.empty()) m_image_view_to_framebuffers.erase(peer);
            }
        }
    }
}

void DynamicRenderingModule::on_post_get_swapchain_images(
    VkDevice device,
    VkSwapchainKHR swapchain,
    VkFormat format,
    VkExtent2D extent,
    VkImageUsageFlags usage,
    uint32_t count,
    const VkImage* pImages
) {
    if (!pImages || count == 0) return;
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        VkImage image = pImages[i];
        if (image == VK_NULL_HANDLE) continue;
        ImageMeta meta;
        meta.format = format;
        meta.samples = VK_SAMPLE_COUNT_1_BIT;
        meta.extent = {extent.width, extent.height, 1};
        meta.usage = usage;
        m_images[vkfix::DeviceObjectKey::make(device, image)] = meta;

        for (auto& pair : m_image_views) {
            if (pair.first.device == device && pair.second.image == image) {
                pair.second.extent = extent;
                pair.second.usage = usage;
                pair.second.samples = VK_SAMPLE_COUNT_1_BIT;
                if (pair.second.format == VK_FORMAT_UNDEFINED) {
                    pair.second.format = format;
                }
            }
        }
    }
}

VkRenderPass DynamicRenderingModule::continuation_render_pass(
    VkDevice device, const DynamicRenderPassKey& originalKey,
    const std::vector<VkAttachmentDescription>& attachments,
    const std::vector<VkAttachmentReference>& colorRefs,
    const VkAttachmentReference* depthRef, const VkRenderingInfo& rendering) {
    thread_local DynamicRenderPassKey key;
    key = originalKey;
    for (uint32_t i = 0; i < key.colorAttachmentCount; ++i) {
        if (colorRefs[i].attachment == VK_ATTACHMENT_UNUSED) continue;
        key.colorAttachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    if (depthRef) {
        if (rendering.pDepthAttachment && rendering.pDepthAttachment->imageView) {
            key.depthStencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
        if (rendering.pStencilAttachment && rendering.pStencilAttachment->imageView) {
            key.depthStencilAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
    }
    {
        std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
        auto it = m_dynamic_rp_cache.find(key);
        if (it != m_dynamic_rp_cache.end()) return it->second;
    }
    // Most begins hit the LOAD cache. Copy attachment storage only on a miss.
    auto preserved = attachments;
    for (uint32_t i = 0; i < key.colorAttachmentCount; ++i)
        if (colorRefs[i].attachment != VK_ATTACHMENT_UNUSED)
            preserved[colorRefs[i].attachment].loadOp = key.colorAttachments[i].loadOp;
    if (depthRef) {
        preserved[depthRef->attachment].loadOp = key.depthStencilAttachment.loadOp;
        preserved[depthRef->attachment].stencilLoadOp = key.depthStencilAttachment.stencilLoadOp;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = (uint32_t)colorRefs.size();
    subpass.pColorAttachments = colorRefs.data();
    subpass.pDepthStencilAttachment = depthRef;
    VkRenderPassCreateInfo create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    create.attachmentCount = (uint32_t)preserved.size();
    create.pAttachments = preserved.data();
    create.subpassCount = 1;
    create.pSubpasses = &subpass;
    auto real = (PFN_vkCreateRenderPass)get_real_proc(get_last_instance(), device, "vkCreateRenderPass");
    VkRenderPass pass{};
    if (!real || real(device, &create, nullptr, &pass) != VK_SUCCESS || !pass) {
        LOGE("Bounded tiler rendering: could not create LOAD continuation render pass");
        return VK_NULL_HANDLE;
    }
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    auto inserted = m_dynamic_rp_cache.emplace(key, pass);
    if (!inserted.second) {
        auto destroy = (PFN_vkDestroyRenderPass)get_real_proc(get_last_instance(), device, "vkDestroyRenderPass");
        if (destroy) destroy(device, pass, nullptr);
        return inserted.first->second;
    }
    m_device_resources[(uint64_t)(uintptr_t)device].renderPasses.push_back(pass);
    return pass;
}

void DynamicRenderingModule::block_tiler_split(VkCommandBuffer commandBuffer, bool begin) {
    if (!m_has_tiler_guard.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    auto key = (uint64_t)(uintptr_t)commandBuffer;
    if (begin) ++m_split_blockers[key];
    else {
        auto it = m_split_blockers.find(key);
        if (it != m_split_blockers.end() && !--it->second) m_split_blockers.erase(it);
    }
}

bool DynamicRenderingModule::needs_tiler_tracking(VkDevice device) {
    if (!m_has_tiler_guard.load(std::memory_order_acquire)) return false;
    std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
    return m_tiler_devices.count((uint64_t)(uintptr_t)device) != 0;
}

void DynamicRenderingModule::track_render_pass(VkDevice device,
    const VkRenderPassCreateInfo* info, VkRenderPass pass) {
    if (!m_has_tiler_guard.load(std::memory_order_acquire) || !info || !pass) return;
    {
        std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
        if (!m_tiler_devices.count((uint64_t)(uintptr_t)device)) return;
    }
    // Multi-subpass, resolves, input attachments and extension state need their
    // original tile lifetime. BSL's shadow and scene passes are single-subpass.
    if (info->pNext || info->flags || info->subpassCount != 1 || !info->pSubpasses ||
        !info->attachmentCount || !info->pAttachments) return;
    const auto& subpass = info->pSubpasses[0];
    if (subpass.flags || subpass.pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        subpass.inputAttachmentCount || subpass.pResolveAttachments ||
        subpass.preserveAttachmentCount) return;
    std::vector<VkAttachmentDescription> attachments(info->pAttachments,
        info->pAttachments + info->attachmentCount);
    auto preserve = [&](const VkAttachmentReference& ref, bool color) {
        if (ref.attachment == VK_ATTACHMENT_UNUSED) return true;
        if (ref.attachment >= attachments.size()) return false;
        auto& a = attachments[ref.attachment];
        if (a.flags || a.samples != VK_SAMPLE_COUNT_1_BIT ||
            ref.layout == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT) return false;
        const bool stencil = !color && (a.format == VK_FORMAT_S8_UINT ||
            a.format == VK_FORMAT_D16_UNORM_S8_UINT || a.format == VK_FORMAT_D24_UNORM_S8_UINT ||
            a.format == VK_FORMAT_D32_SFLOAT_S8_UINT);
        const bool depth = !color && a.format != VK_FORMAT_S8_UINT;
        if (((color || depth) && a.storeOp != VK_ATTACHMENT_STORE_OP_STORE) ||
            (stencil && a.stencilStoreOp != VK_ATTACHMENT_STORE_OP_STORE)) return false;
        // Ending the original pass performs its final layout transition. The
        // next pass starts from that layout, never from UNDEFINED / initial.
        a.initialLayout = a.finalLayout;
        if (color || depth) a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (stencil) a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        return true;
    };
    for (uint32_t i = 0; i < subpass.colorAttachmentCount; ++i)
        if (!subpass.pColorAttachments || !preserve(subpass.pColorAttachments[i], true)) return;
    if (subpass.pDepthStencilAttachment && !preserve(*subpass.pDepthStencilAttachment, false)) return;
    auto create = *info;
    create.pAttachments = attachments.data();
    auto real = (PFN_vkCreateRenderPass)get_real_proc(get_last_instance(), device, "vkCreateRenderPass");
    VkRenderPass continuation{};
    if (!real || real(device, &create, nullptr, &continuation) != VK_SUCCESS || !continuation) {
        LOGE("Bounded tiler rendering: could not create legacy LOAD continuation");
        return;
    }
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    auto inserted = m_legacy_continuations.emplace(
        vkfix::DeviceObjectKey::make(device, pass), continuation);
    if (!inserted.second) {
        auto destroy = (PFN_vkDestroyRenderPass)get_real_proc(get_last_instance(), device, "vkDestroyRenderPass");
        if (destroy) destroy(device, continuation, nullptr);
        return;
    }
    LOGI("Bounded tiler rendering: legacy render pass %p protected (%u attachments)",
        (void*)pass, info->attachmentCount);
}

void DynamicRenderingModule::track_render_pass2(VkDevice device,
    const VkRenderPassCreateInfo2* info, VkRenderPass pass) {
    if (!m_has_tiler_guard.load(std::memory_order_acquire) || !info || info->pNext ||
        info->subpassCount != 1 || !info->pSubpasses || info->correlatedViewMaskCount) return;
    const auto& src = info->pSubpasses[0];
    if (src.pNext || src.viewMask || src.inputAttachmentCount || src.pResolveAttachments) return;
    const bool sanitize = LayoutCompat::get().is_emulated(device);
    auto layout = [&](VkImageLayout value) { return sanitize ? LayoutCompat::sanitize(value) : value; };
    std::vector<VkAttachmentDescription> attachments(info->attachmentCount);
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const auto& a = info->pAttachments[i];
        if (a.pNext) return;
        attachments[i] = {a.flags, a.format, a.samples, a.loadOp, a.storeOp,
            a.stencilLoadOp, a.stencilStoreOp, layout(a.initialLayout), layout(a.finalLayout)};
    }
    std::vector<VkAttachmentReference> colors(src.colorAttachmentCount);
    for (uint32_t i = 0; i < src.colorAttachmentCount; ++i) {
        if (src.pColorAttachments[i].pNext) return;
        colors[i] = {src.pColorAttachments[i].attachment, layout(src.pColorAttachments[i].layout)};
    }
    VkAttachmentReference depth{};
    if (src.pDepthStencilAttachment) {
        if (src.pDepthStencilAttachment->pNext) return;
        depth = {src.pDepthStencilAttachment->attachment, layout(src.pDepthStencilAttachment->layout)};
    }
    std::vector<VkSubpassDependency> dependencies(info->dependencyCount);
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        const auto& d = info->pDependencies[i];
        if (d.pNext || d.viewOffset || (d.dependencyFlags & VK_DEPENDENCY_VIEW_LOCAL_BIT)) return;
        dependencies[i] = {d.srcSubpass, d.dstSubpass, d.srcStageMask, d.dstStageMask,
            d.srcAccessMask, d.dstAccessMask, d.dependencyFlags};
    }
    VkSubpassDescription subpass{};
    subpass.flags = src.flags;
    subpass.pipelineBindPoint = src.pipelineBindPoint;
    subpass.colorAttachmentCount = (uint32_t)colors.size();
    subpass.pColorAttachments = colors.data();
    subpass.pDepthStencilAttachment = src.pDepthStencilAttachment ? &depth : nullptr;
    subpass.preserveAttachmentCount = src.preserveAttachmentCount;
    VkRenderPassCreateInfo create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    create.flags = info->flags;
    create.attachmentCount = (uint32_t)attachments.size();
    create.pAttachments = attachments.data();
    create.subpassCount = 1;
    create.pSubpasses = &subpass;
    create.dependencyCount = (uint32_t)dependencies.size();
    create.pDependencies = dependencies.data();
    track_render_pass(device, &create, pass);
}

void DynamicRenderingModule::forget_render_pass(VkDevice device, VkRenderPass pass) {
    if (!m_has_tiler_guard.load(std::memory_order_acquire)) return;
    std::unique_lock<std::shared_mutex> lock(m_rw_mutex);
    auto it = m_legacy_continuations.find(vkfix::DeviceObjectKey::make(device, pass));
    if (it == m_legacy_continuations.end()) return;
    auto destroy = (PFN_vkDestroyRenderPass)get_real_proc(get_last_instance(), device, "vkDestroyRenderPass");
    if (destroy) destroy(device, it->second, nullptr);
    m_legacy_continuations.erase(it);
}

void DynamicRenderingModule::begin_legacy_rendering(VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    if (!m_has_tiler_guard.load(std::memory_order_acquire)) return;
    VkDevice device = get_device_for_cmd(commandBuffer);
    VkRenderPass continuation{};
    const VkRenderPassAttachmentBeginInfo* views = nullptr;
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (info && contents == VK_SUBPASS_CONTENTS_INLINE && dt.CmdBeginRenderPass &&
        dt.CmdEndRenderPass && dt.CmdPipelineBarrier) {
        std::shared_lock<std::shared_mutex> lock(m_rw_mutex);
        auto it = m_legacy_continuations.find(vkfix::DeviceObjectKey::make(device, info->renderPass));
        if (it != m_legacy_continuations.end()) {
            bool supported = true;
            if (info->pNext) {
                views = (const VkRenderPassAttachmentBeginInfo*)info->pNext;
                supported = views->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO &&
                    !views->pNext && (!views->attachmentCount || views->pAttachments);
            }
            if (supported) continuation = it->second;
        }
    }
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    auto key = (uint64_t)(uintptr_t)commandBuffer;
    auto& state = m_cmd_rendering_states[key];
    state.device = device;
    state.legacy = true;
    state.is_rendering = continuation != VK_NULL_HANDLE;
    state.continuation = m_primary_commands.count(key) ? continuation : VK_NULL_HANDLE;
    state.budget = {};
    state.attachmentViews.clear();
    if (state.continuation) {
        state.activeRenderPass = info->renderPass;
        state.activeFramebuffer = info->framebuffer;
        state.renderArea = info->renderArea;
        if (views && views->attachmentCount)
            state.attachmentViews.assign(views->pAttachments, views->pAttachments + views->attachmentCount);
    }
}

void DynamicRenderingModule::end_legacy_rendering(VkCommandBuffer commandBuffer) {
    if (!m_has_tiler_guard.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(m_cmd_mutex);
    auto it = m_cmd_rendering_states.find((uint64_t)(uintptr_t)commandBuffer);
    if (it != m_cmd_rendering_states.end()) {
        it->second.is_rendering = false;
        it->second.continuation = VK_NULL_HANDLE;
        it->second.attachmentViews.clear();
    }
}

void DynamicRenderingModule::before_draw(VkCommandBuffer commandBuffer, uint64_t vertices,
                                        uint32_t indirectDraws) {
    if (!m_has_tiler_guard.load(std::memory_order_acquire) || (!vertices && !indirectDraws)) return;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    VkRenderPassAttachmentBeginInfo attachments{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
    std::vector<VkImageView> views;
    bool legacy = false;
    VkDevice device{};
    {
        std::lock_guard<std::mutex> lock(m_cmd_mutex);
        auto key = (uint64_t)(uintptr_t)commandBuffer;
        auto it = m_cmd_rendering_states.find(key);
        if (it == m_cmd_rendering_states.end() || !it->second.is_rendering ||
            !it->second.continuation) return;
        auto& state = it->second;
        const bool split = !m_split_blockers.count(key) && state.budget.needs_split(vertices, indirectDraws);
        if (split) {
            begin.renderPass = state.continuation;
            begin.framebuffer = state.activeFramebuffer;
            begin.renderArea = state.renderArea;
            views = state.attachmentViews;
            device = state.device;
            legacy = state.legacy;
            state.activeRenderPass = state.continuation;
            state.budget = {};
        }
        state.budget.add(vertices, indirectDraws);
        if (!split) return;
    }
    const auto& dt = LayerManager::get().get_dispatch_table(device);
    if (!views.empty()) {
        attachments.attachmentCount = (uint32_t)views.size();
        attachments.pAttachments = views.data();
        begin.pNext = &attachments;
    }
    dt.CmdEndRenderPass(commandBuffer);
    // Finish fragment work before the next vertex/tiling batch can reuse the
    // finite tiler heap. No intermediate CLEAR and no splitting a multi-draw.
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dt.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
    dt.CmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    auto count = m_tiler_splits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || (count & 255u) == 0)
        LOGI("Bounded tiler rendering: %llu preserving render-pass continuation(s), path=%s",
            (unsigned long long)count, legacy ? "legacy" : "dynamic");
}

bool DynamicRenderingModule::on_cmd_begin_rendering(
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo* pRenderingInfo
) {
    VkDevice device = get_device_for_cmd(commandBuffer);
    if (is_device_native(device) || !pRenderingInfo) return false;

    uint32_t effectiveColorCount = 0;
    if (pRenderingInfo->pColorAttachments != NULL) {
        for (uint32_t i = pRenderingInfo->colorAttachmentCount; i > 0; --i) {
            if (pRenderingInfo->pColorAttachments[i - 1].imageView != VK_NULL_HANDLE ||
                pRenderingInfo->pColorAttachments[i - 1].resolveImageView != VK_NULL_HANDLE) {
                effectiveColorCount = i;
                break;
            }
        }
    }

    const uint32_t colorCount = effectiveColorCount;
    const uint32_t maxAttCount = colorCount * 2 + 1;

    struct ThreadScratchBuffers {
        DynamicRenderPassKey rpKey;
        FramebufferKey fbKey;
        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> colorRefs;
        std::vector<VkAttachmentReference> resolveRefs;
        std::vector<VkImageView> fbViews;
        std::vector<VkClearValue> clearValues;

        void clear() {
            rpKey.colorAttachmentCount = 0;
            rpKey.colorAttachments.clear();
            rpKey.has_depth_stencil = false;
            rpKey.depthStencilAttachment = {};
            rpKey.resolveAttachmentCount = 0;
            rpKey.resolveAttachments.clear();
            fbKey.views.clear();
            attachments.clear();
            colorRefs.clear();
            resolveRefs.clear();
            fbViews.clear();
            clearValues.clear();
        }
    };
    thread_local ThreadScratchBuffers scratch;
    scratch.clear();

    DynamicRenderPassKey& rpKey = scratch.rpKey;
    rpKey.device = device;
    FramebufferKey& fbKey = scratch.fbKey;
    fbKey.device = device;

    std::vector<VkAttachmentDescription>& attachments = scratch.attachments;
    attachments.reserve(maxAttCount);

    std::vector<VkAttachmentReference>& colorRefs = scratch.colorRefs;
    colorRefs.reserve(colorCount);

    std::vector<VkAttachmentReference>& resolveRefs = scratch.resolveRefs;
    VkAttachmentReference depthRef{};

    std::vector<VkImageView>& fbViews = scratch.fbViews;
    fbViews.reserve(maxAttCount);

    std::vector<VkClearValue>& clearValues = scratch.clearValues;
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
        resolveRefs.reserve(colorCount);
    }

    // Acquire lock once for view metadata and cache lookups (concurrent multi-thread read)
    std::shared_lock<std::shared_mutex> readLock(m_rw_mutex);

    // Framebuffer dimensions tracking (single-pass computation during attachment lookup)
    uint32_t min_att_w = 0;
    uint32_t min_att_h = 0;

    // 1. Color Attachments
    for (uint32_t i = 0; i < colorCount; ++i) {
        const auto& att = pRenderingInfo->pColorAttachments[i];
        if (att.imageView != VK_NULL_HANDLE) {
            auto it = m_image_views.find(vkfix::DeviceObjectKey::make(device, att.imageView));
            VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            if (it != m_image_views.end()) {
                if (it->second.format != VK_FORMAT_UNDEFINED) fmt = it->second.format;
                samples = it->second.samples;
                if (it->second.extent.width > 0 && it->second.extent.height > 0) {
                    min_att_w = (min_att_w == 0) ? it->second.extent.width : std::min(min_att_w, it->second.extent.width);
                    min_att_h = (min_att_h == 0) ? it->second.extent.height : std::min(min_att_h, it->second.extent.height);
                }
            }

            DynamicRenderPassKey::AttachmentDesc aDesc;
            aDesc.format = fmt;
            aDesc.samples = samples;
            aDesc.loadOp = att.loadOp;
            aDesc.storeOp = att.storeOp;
            VkImageLayout sanitized = sanitize_color_layout(att.imageLayout);
            aDesc.initialLayout = sanitized;
            aDesc.refLayout = (sanitized == VK_IMAGE_LAYOUT_UNDEFINED) ?
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : sanitized;
            aDesc.finalLayout = aDesc.refLayout;
            rpKey.colorAttachments.push_back(aDesc);
            ++rpKey.colorAttachmentCount;

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
            ++rpKey.colorAttachmentCount;

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
        auto it = m_image_views.find(vkfix::DeviceObjectKey::make(device, dsView));
        if (it != m_image_views.end()) {
            if (it->second.format != VK_FORMAT_UNDEFINED) dsDesc.format = it->second.format;
            dsDesc.samples = it->second.samples;
            if (it->second.extent.width > 0 && it->second.extent.height > 0) {
                min_att_w = (min_att_w == 0) ? it->second.extent.width : std::min(min_att_w, it->second.extent.width);
                min_att_h = (min_att_h == 0) ? it->second.extent.height : std::min(min_att_h, it->second.extent.height);
            }
        } else {
            dsDesc.format = VK_FORMAT_D32_SFLOAT;
            dsDesc.samples = VK_SAMPLE_COUNT_1_BIT;
        }

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

        dsDesc.refLayout = (dsDesc.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) ?
                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : dsDesc.initialLayout;
        dsDesc.finalLayout = dsDesc.refLayout;

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
                auto it = m_image_views.find(vkfix::DeviceObjectKey::make(device, att.resolveImageView));
                VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
                if (it != m_image_views.end()) {
                    if (it->second.format != VK_FORMAT_UNDEFINED) fmt = it->second.format;
                    if (it->second.extent.width > 0 && it->second.extent.height > 0) {
                        min_att_w = (min_att_w == 0) ? it->second.extent.width : std::min(min_att_w, it->second.extent.width);
                        min_att_h = (min_att_h == 0) ? it->second.extent.height : std::min(min_att_h, it->second.extent.height);
                    }
                }

                DynamicRenderPassKey::AttachmentDesc rDesc;
                rDesc.format = fmt;
                rDesc.samples = VK_SAMPLE_COUNT_1_BIT;
                rDesc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                rDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkImageLayout sanitized = sanitize_color_layout(att.resolveImageLayout);
                rDesc.initialLayout = sanitized;
                rDesc.refLayout = (sanitized == VK_IMAGE_LAYOUT_UNDEFINED) ?
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : sanitized;
                rDesc.finalLayout = rDesc.refLayout;
                rpKey.resolveAttachments.push_back(rDesc);
                ++rpKey.resolveAttachmentCount;

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
                rpKey.resolveAttachments.emplace_back();
                ++rpKey.resolveAttachmentCount;
            }
        }
    }

    // Framebuffer dimensions calculation (single-pass calculated min dimensions)
    uint32_t area_w = pRenderingInfo->renderArea.offset.x + pRenderingInfo->renderArea.extent.width;
    uint32_t area_h = pRenderingInfo->renderArea.offset.y + pRenderingInfo->renderArea.extent.height;

    uint32_t fb_w = (min_att_w > 0) ? min_att_w : area_w;
    uint32_t fb_h = (min_att_h > 0) ? min_att_h : area_h;
    if (fb_w == 0) fb_w = 1;
    if (fb_h == 0) fb_h = 1;
    uint32_t fb_layers = (pRenderingInfo->layerCount > 0) ? pRenderingInfo->layerCount : 1;

    // Clamp renderArea to within framebuffer extents to satisfy VUID-VkRenderPassBeginInfo-renderArea-00171
    VkRect2D clampedRenderArea = pRenderingInfo->renderArea;
    if (clampedRenderArea.offset.x < 0) clampedRenderArea.offset.x = 0;
    if (clampedRenderArea.offset.y < 0) clampedRenderArea.offset.y = 0;
    if ((uint32_t)clampedRenderArea.offset.x >= fb_w) {
        clampedRenderArea.offset.x = 0;
        clampedRenderArea.extent.width = fb_w;
    } else if ((uint32_t)clampedRenderArea.offset.x + clampedRenderArea.extent.width > fb_w) {
        clampedRenderArea.extent.width = fb_w - (uint32_t)clampedRenderArea.offset.x;
    }
    if ((uint32_t)clampedRenderArea.offset.y >= fb_h) {
        clampedRenderArea.offset.y = 0;
        clampedRenderArea.extent.height = fb_h;
    } else if ((uint32_t)clampedRenderArea.offset.y + clampedRenderArea.extent.height > fb_h) {
        clampedRenderArea.extent.height = fb_h - (uint32_t)clampedRenderArea.offset.y;
    }

    // Resolve through the live caches so view/command-buffer destruction cannot
    // leave a thread-local handle or state pointer pointing at freed resources.
    VkRenderPass renderPass = VK_NULL_HANDLE;
    auto rpIt = m_dynamic_rp_cache.find(rpKey);
    if (rpIt != m_dynamic_rp_cache.end()) renderPass = rpIt->second;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (renderPass != VK_NULL_HANDLE) {
        fbKey.renderPass = renderPass;
        fbKey.viewCount = (uint32_t)fbViews.size();
        fbKey.views = fbViews;
        fbKey.width = fb_w;
        fbKey.height = fb_h;
        fbKey.layers = fb_layers;

        auto fbIt = m_framebuffer_cache.find(fbKey);
        if (fbIt != m_framebuffer_cache.end()) framebuffer = fbIt->second;
    }

    // Only ordinary inline, single-sample STORE scopes are safe to split.
    // Preserve query/transform-feedback scopes and caller-owned resolve work.
    bool tilerEligible = m_tiler_devices.count((uint64_t)(uintptr_t)device) &&
        !pRenderingInfo->flags && !pRenderingInfo->pNext && !pRenderingInfo->viewMask &&
        !attachments.empty();
    auto storable = [&](const VkRenderingAttachmentInfo* attachment) {
        if (!attachment || !attachment->imageView) return true;
        auto view = m_image_views.find(vkfix::DeviceObjectKey::make(device, attachment->imageView));
        return !attachment->pNext && attachment->resolveMode == VK_RESOLVE_MODE_NONE &&
            !attachment->resolveImageView && attachment->storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
            attachment->imageLayout != VK_IMAGE_LAYOUT_UNDEFINED && view != m_image_views.end() &&
            view->second.usage != 0 && view->second.samples == VK_SAMPLE_COUNT_1_BIT &&
            !(view->second.usage & (VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT));
    };
    for (uint32_t i = 0; tilerEligible && i < colorCount; ++i)
        tilerEligible = storable(&pRenderingInfo->pColorAttachments[i]);
    tilerEligible = tilerEligible && storable(pRenderingInfo->pDepthAttachment) &&
        storable(pRenderingInfo->pStencilAttachment);
    {
        std::lock_guard<std::mutex> cmdLock(m_cmd_mutex);
        tilerEligible = tilerEligible && m_primary_commands.count((uint64_t)(uintptr_t)commandBuffer);
    }
    readLock.unlock();
    VkRenderPass continuation = VK_NULL_HANDLE;
    if (tilerEligible) {
        const auto& dt = LayerManager::get().get_dispatch_table(device);
        if (dt.CmdBeginRenderPass && dt.CmdEndRenderPass && dt.CmdPipelineBarrier)
            continuation = continuation_render_pass(device, rpKey, attachments, colorRefs,
                dsView != VK_NULL_HANDLE ? &depthRef : nullptr, *pRenderingInfo);
    }

    // Fast-path: both renderPass and framebuffer already cached (common runtime hot path)
    if (__builtin_expect(renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE, 1)) {
        {
            std::lock_guard<std::mutex> cmdLock(m_cmd_mutex);
            auto& state = m_cmd_rendering_states[(uint64_t)(uintptr_t)commandBuffer];
            state.device = device;
            state.is_rendering = true;
            state.legacy = false;
            state.activeRenderPass = renderPass;
            state.activeFramebuffer = framebuffer;
            state.continuation = continuation;
            state.renderArea = clampedRenderArea;
            state.attachmentViews.clear();
            state.budget = {};
        }

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = renderPass;
        beginInfo.framebuffer = framebuffer;
        beginInfo.renderArea = clampedRenderArea;
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

        LOG_OPT_DEBUG("DynamicRendering: emulated vkCmdBeginRenderingKHR for cmd %p (fast path)", commandBuffer);
        return true;
    }

    // Cold path: creation uses the same continuation and attachment policy.

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
                vku::set_object_name(device, VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)renderPass, "[VulkanFix] DynamicRendering RenderPass");
            }
        }
        LOG_OPT_DEBUG("DynamicRendering: created dynamic renderPass %p", (void*)(uintptr_t)renderPass);
    }

    if (framebuffer == VK_NULL_HANDLE) {
        fbKey.renderPass = renderPass;
        fbKey.viewCount = (uint32_t)fbViews.size();
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
                    m_device_resources[(uint64_t)(uintptr_t)device].framebuffers.insert(framebuffer);
                    vku::set_object_name(device, VK_OBJECT_TYPE_FRAMEBUFFER, (uint64_t)framebuffer, "[VulkanFix] DynamicRendering Framebuffer");
                    for (uint32_t i = 0; i < fbKey.viewCount; ++i) {
                        VkImageView v = fbKey.views[i];
                        if (v != VK_NULL_HANDLE) {
                            m_image_view_to_framebuffers[vkfix::DeviceObjectKey::make(device, v)].insert(fbKey);
                        }
                    }
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
    beginInfo.renderArea = clampedRenderArea;
    beginInfo.clearValueCount = (uint32_t) clearValues.size();
    beginInfo.pClearValues = clearValues.empty() ? NULL : clearValues.data();

    VkSubpassContents contents = (pRenderingInfo->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR) ?
        VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    vku::cmd_begin_label(commandBuffer, "[VulkanFix] DynamicRendering Pass");
    if (dt.CmdBeginRenderPass) {
        dt.CmdBeginRenderPass(commandBuffer, &beginInfo, contents);
    } else {
        PFN_vkCmdBeginRenderPass real_begin_rp = (PFN_vkCmdBeginRenderPass)
            get_real_proc(get_last_instance(), device, "vkCmdBeginRenderPass");
        if (!real_begin_rp) {
            vku::cmd_end_label(commandBuffer);
            return false;
        }
        real_begin_rp(commandBuffer, &beginInfo, contents);
    }

    {
        std::lock_guard<std::mutex> cmdLock(m_cmd_mutex);
        auto& state = m_cmd_rendering_states[(uint64_t)(uintptr_t)commandBuffer];
        state.device = device;
        state.is_rendering = true;
        state.legacy = false;
        state.activeRenderPass = renderPass;
        state.activeFramebuffer = framebuffer;
        state.continuation = continuation;
        state.renderArea = clampedRenderArea;
        state.attachmentViews.clear();
        state.budget = {};
    }

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
    vku::cmd_end_label(commandBuffer);

    {
        std::lock_guard<std::mutex> cmdLock(m_cmd_mutex);
        auto it = m_cmd_rendering_states.find((uint64_t)(uintptr_t)commandBuffer);
        if (it != m_cmd_rendering_states.end()) it->second.is_rendering = false;
    }

    LOG_OPT_DEBUG("DynamicRendering: emulated vkCmdEndRenderingKHR for cmd %p", commandBuffer);
    return true;
}

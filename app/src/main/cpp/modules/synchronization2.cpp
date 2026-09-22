#include "synchronization2.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "vk_pnext.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>

REGISTER_LAYER_MODULE(Synchronization2Module);

static inline VkPipelineStageFlags stage_flags2_to_stage_flags(VkPipelineStageFlags2 flags2, bool is_dst = false) {
    // Bits 0..16 match standard VkPipelineStageFlagBits in Vulkan 1.0:
    // TOP_OF_PIPE (0x1), DRAW_INDIRECT (0x2), VERTEX_INPUT (0x4), VERTEX_SHADER (0x8),
    // TESSELLATION_CONTROL_SHADER (0x10), TESSELLATION_EVALUATION_SHADER (0x20),
    // GEOMETRY_SHADER (0x40), FRAGMENT_SHADER (0x80), EARLY_FRAGMENT_TESTS (0x100),
    // LATE_FRAGMENT_TESTS (0x200), COLOR_ATTACHMENT_OUTPUT (0x400), COMPUTE_SHADER (0x800),
    // TRANSFER / ALL_TRANSFER (0x1000), BOTTOM_OF_PIPE (0x2000), HOST (0x4000),
    // ALL_GRAPHICS (0x8000), ALL_COMMANDS (0x10000)
    VkPipelineStageFlags out = (VkPipelineStageFlags)(flags2 & 0x0001FFFFULL);

    if (flags2 & (VK_PIPELINE_STAGE_2_COPY_BIT |
                  VK_PIPELINE_STAGE_2_RESOLVE_BIT |
                  VK_PIPELINE_STAGE_2_BLIT_BIT |
                  VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
        out |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (flags2 & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT)) {
        out |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    }
    if (flags2 & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) {
        out |= (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT);
    }
    if (flags2 & (VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR |
                  VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR |
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                  VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT |
                  VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT)) {
        out |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    if (out == 0 && flags2 == 0) {
        out = is_dst ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    return out;
}

static inline VkAccessFlags access_flags2_to_access_flags(VkAccessFlags2 flags2) {
    // Bits 0..16 match standard VkAccessFlagBits in Vulkan 1.0
    VkAccessFlags out = (VkAccessFlags)(flags2 & 0x0001FFFFULL);

    if (flags2 & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) {
        out |= VK_ACCESS_SHADER_READ_BIT;
    }
    if (flags2 & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) {
        out |= VK_ACCESS_SHADER_WRITE_BIT;
    }
    if (flags2 & VK_ACCESS_2_COLOR_ATTACHMENT_READ_NONCOHERENT_BIT_EXT) {
        out |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    }
    if (flags2 & (VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR |
                  VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR |
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR)) {
        out |= VK_ACCESS_MEMORY_READ_BIT;
    }
    if (flags2 & (VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR |
                  VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR |
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR)) {
        out |= VK_ACCESS_MEMORY_WRITE_BIT;
    }
    return out;
}

static inline VkImageLayout sanitize_barrier_layout(VkImageLayout layout, VkImageAspectFlags aspectMask) {
    // Map synchronization2 generic layouts to standard Vulkan 1.0 layouts
    if ((int32_t)layout == 1000314000 /* VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL */) {
        if (aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if ((int32_t)layout == 1000314001 /* VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL */) {
        if (aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    return layout;
}

Synchronization2Module::Synchronization2Module() {
    LOGI("Initialized Vulkan VK_KHR_synchronization2 & synchronization2 feature emulation module");
}

bool Synchronization2Module::is_phys_device_native(VkPhysicalDevice physDev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_phys_native_support.find((uint64_t)(uintptr_t)physDev);
    if (it != m_phys_native_support.end()) {
        return it->second;
    }

    const char* force_emu = getenv("FORCE_EMULATE_SYNCHRONIZATION2");
    if (force_emu && (strcmp(force_emu, "1") == 0 || strcasecmp(force_emu, "true") == 0)) {
        LOGI("FORCE_EMULATE_SYNCHRONIZATION2 set, enabling emulation for physical device %p", physDev);
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
                    if (strcmp(e.extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0) {
                        native = true;
                        break;
                    }
                }
            }
        }
    }

    // Also check if Vulkan 1.3 synchronization2 feature is natively enabled
    if (!native) {
        PFN_vkGetPhysicalDeviceProperties real_props_fn =
            (PFN_vkGetPhysicalDeviceProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
        if (real_props_fn) {
            VkPhysicalDeviceProperties props{};
            real_props_fn(physDev, &props);
            if (props.apiVersion >= VK_API_VERSION_1_3) {
                PFN_vkGetPhysicalDeviceFeatures2 real_gpf2 =
                    (PFN_vkGetPhysicalDeviceFeatures2) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2");
                if (real_gpf2) {
                    VkPhysicalDeviceVulkan13Features v13Features{};
                    v13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
                    v13Features.pNext = nullptr;

                    VkPhysicalDeviceFeatures2 features2{};
                    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    features2.pNext = &v13Features;

                    real_gpf2(physDev, &features2);
                    if (v13Features.synchronization2 == VK_TRUE) {
                        native = true;
                    }
                }
            }
        }
    }

    m_phys_native_support[(uint64_t)(uintptr_t)physDev] = native;
    if (!native) {
        LOGI("Physical device %p lacks native VK_KHR_synchronization2, enabling emulation layer!", physDev);
    } else {
        LOGI("Physical device %p natively supports VK_KHR_synchronization2", physDev);
    }
    return native;
}

bool Synchronization2Module::is_device_native(VkDevice device) {
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_device_needs_emulation.find((uint64_t)(uintptr_t)device);
    if (it != m_device_needs_emulation.end()) {
        return !it->second;
    }
    return false;
}

void Synchronization2Module::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    bool has_ext = vku::has_extension(extensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    if (!has_ext) {
        VkExtensionProperties prop{};
        memset(&prop, 0, sizeof(prop));
        strncpy(prop.extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
        prop.specVersion = VK_KHR_SYNCHRONIZATION_2_SPEC_VERSION;
        extensions.push_back(prop);
        LOGI("Injected extension: %s (v%u)", VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, prop.specVersion);
    }
}

void Synchronization2Module::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    pUserData = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR);
    if (pUserData) {
        LOG_OPT_DEBUG("Synchronization2: unlinked VkPhysicalDeviceSynchronization2FeaturesKHR from pFeatures2->pNext");
    }
}

void Synchronization2Module::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures) return;
    if (is_phys_device_native(physicalDevice)) return;

    if (pUserData) {
        auto* syncFeatures = vku::relink_pnext<VkPhysicalDeviceSynchronization2FeaturesKHR>(
            pFeatures->pNext, pUserData);
        if (syncFeatures) {
            syncFeatures->synchronization2 = VK_TRUE;
            LOG_OPT_DEBUG("Synchronization2: supplied synchronization2 = VK_TRUE in VkPhysicalDeviceSynchronization2FeaturesKHR");
        }
    } else {
        auto* syncFeatures = vku::find_pnext_mut<VkPhysicalDeviceSynchronization2FeaturesKHR>(
            pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR);
        if (syncFeatures) {
            syncFeatures->synchronization2 = VK_TRUE;
            LOG_OPT_DEBUG("Synchronization2: supplied synchronization2 = VK_TRUE in VkPhysicalDeviceSynchronization2FeaturesKHR");
        }
    }

    auto* v13 = vku::find_pnext_mut<VkPhysicalDeviceVulkan13Features>(
        pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    if (v13) {
        v13->synchronization2 = VK_TRUE;
        LOG_OPT_DEBUG("Synchronization2: supplied synchronization2 = VK_TRUE in VkPhysicalDeviceVulkan13Features");
    }
}

void Synchronization2Module::on_pre_create_device(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pCreateInfo) return;

    // 1. Strip extension from enabledExtensions
    if (vku::strip_extension(enabledExtensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
        LOGI("vkCreateDevice: stripped %s from enabledExtensions", VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }

    // 2. Unlink VkPhysicalDeviceSynchronization2FeaturesKHR from pCreateInfo->pNext
    pUserData = vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR);
    if (pUserData) {
        LOGI("vkCreateDevice: unlinked VkPhysicalDeviceSynchronization2FeaturesKHR from pNext");
    }

    // 3. Disable synchronization2 in VkPhysicalDeviceVulkan13Features if chained
    auto* v13 = vku::find_pnext_mut<VkPhysicalDeviceVulkan13Features>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    if (v13) {
        v13->synchronization2 = VK_FALSE;
        LOGI("vkCreateDevice: disabled synchronization2 in VkPhysicalDeviceVulkan13Features");
    }
}

void Synchronization2Module::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        bool native = is_phys_device_native(physicalDevice);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_needs_emulation[(uint64_t)(uintptr_t)device] = !native;
        LOGI("Device %p created: synchronization2 native=%d", device, native);
    }
}

void Synchronization2Module::on_destroy_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_needs_emulation.erase((uint64_t)(uintptr_t)device);
}

bool Synchronization2Module::on_cmd_set_event2(
    VkCommandBuffer commandBuffer,
    VkEvent event,
    const VkDependencyInfo* pDependencyInfo
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    VkPipelineStageFlags combinedSrc = 0;
    if (pDependencyInfo) {
        for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; ++i) {
            combinedSrc |= stage_flags2_to_stage_flags(pDependencyInfo->pMemoryBarriers[i].srcStageMask, false);
        }
        for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; ++i) {
            combinedSrc |= stage_flags2_to_stage_flags(pDependencyInfo->pBufferMemoryBarriers[i].srcStageMask, false);
        }
        for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; ++i) {
            combinedSrc |= stage_flags2_to_stage_flags(pDependencyInfo->pImageMemoryBarriers[i].srcStageMask, false);
        }
    }
    if (combinedSrc == 0) {
        combinedSrc = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    PFN_vkCmdSetEvent real_fn =
        (PFN_vkCmdSetEvent) get_real_proc(get_last_instance(), device, "vkCmdSetEvent");
    if (real_fn) {
        real_fn(commandBuffer, event, combinedSrc);
    }
    return true;
}

bool Synchronization2Module::on_cmd_reset_event2(
    VkCommandBuffer commandBuffer,
    VkEvent event,
    VkPipelineStageFlags2 stageMask
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    VkPipelineStageFlags stage = stage_flags2_to_stage_flags(stageMask, false);
    if (stage == 0) {
        stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    PFN_vkCmdResetEvent real_fn =
        (PFN_vkCmdResetEvent) get_real_proc(get_last_instance(), device, "vkCmdResetEvent");
    if (real_fn) {
        real_fn(commandBuffer, event, stage);
    }
    return true;
}

bool Synchronization2Module::on_cmd_wait_events2(
    VkCommandBuffer commandBuffer,
    uint32_t eventCount,
    const VkEvent* pEvents,
    const VkDependencyInfo* pDependencyInfos
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    if (eventCount == 0 || !pEvents) return true;

    VkPipelineStageFlags combinedSrc = 0;
    VkPipelineStageFlags combinedDst = 0;

    std::vector<VkMemoryBarrier> v1MemBarriers;
    std::vector<VkBufferMemoryBarrier> v1BufBarriers;
    std::vector<VkImageMemoryBarrier> v1ImgBarriers;

    if (pDependencyInfos) {
        for (uint32_t e = 0; e < eventCount; ++e) {
            const auto& dep = pDependencyInfos[e];
            for (uint32_t i = 0; i < dep.memoryBarrierCount; ++i) {
                const auto& b = dep.pMemoryBarriers[i];
                combinedSrc |= stage_flags2_to_stage_flags(b.srcStageMask, false);
                combinedDst |= stage_flags2_to_stage_flags(b.dstStageMask, true);

                VkMemoryBarrier mb{};
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.pNext = nullptr;
                mb.srcAccessMask = access_flags2_to_access_flags(b.srcAccessMask);
                mb.dstAccessMask = access_flags2_to_access_flags(b.dstAccessMask);
                v1MemBarriers.push_back(mb);
            }
            for (uint32_t i = 0; i < dep.bufferMemoryBarrierCount; ++i) {
                const auto& b = dep.pBufferMemoryBarriers[i];
                combinedSrc |= stage_flags2_to_stage_flags(b.srcStageMask, false);
                combinedDst |= stage_flags2_to_stage_flags(b.dstStageMask, true);

                VkBufferMemoryBarrier bb{};
                bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                bb.pNext = nullptr;
                bb.srcAccessMask = access_flags2_to_access_flags(b.srcAccessMask);
                bb.dstAccessMask = access_flags2_to_access_flags(b.dstAccessMask);
                bb.srcQueueFamilyIndex = b.srcQueueFamilyIndex;
                bb.dstQueueFamilyIndex = b.dstQueueFamilyIndex;
                bb.buffer = b.buffer;
                bb.offset = b.offset;
                bb.size = b.size;
                v1BufBarriers.push_back(bb);
            }
            for (uint32_t i = 0; i < dep.imageMemoryBarrierCount; ++i) {
                const auto& b = dep.pImageMemoryBarriers[i];
                combinedSrc |= stage_flags2_to_stage_flags(b.srcStageMask, false);
                combinedDst |= stage_flags2_to_stage_flags(b.dstStageMask, true);

                VkImageMemoryBarrier ib{};
                ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                ib.pNext = nullptr;
                ib.srcAccessMask = access_flags2_to_access_flags(b.srcAccessMask);
                ib.dstAccessMask = access_flags2_to_access_flags(b.dstAccessMask);
                ib.oldLayout = sanitize_barrier_layout(b.oldLayout, b.subresourceRange.aspectMask);
                ib.newLayout = sanitize_barrier_layout(b.newLayout, b.subresourceRange.aspectMask);
                ib.srcQueueFamilyIndex = b.srcQueueFamilyIndex;
                ib.dstQueueFamilyIndex = b.dstQueueFamilyIndex;
                ib.image = b.image;
                ib.subresourceRange = b.subresourceRange;
                v1ImgBarriers.push_back(ib);
            }
        }
    }

    if (combinedSrc == 0) combinedSrc = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (combinedDst == 0) combinedDst = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    PFN_vkCmdWaitEvents real_fn =
        (PFN_vkCmdWaitEvents) get_real_proc(get_last_instance(), device, "vkCmdWaitEvents");
    if (real_fn) {
        real_fn(
            commandBuffer,
            eventCount,
            pEvents,
            combinedSrc,
            combinedDst,
            (uint32_t)v1MemBarriers.size(),
            v1MemBarriers.data(),
            (uint32_t)v1BufBarriers.size(),
            v1BufBarriers.data(),
            (uint32_t)v1ImgBarriers.size(),
            v1ImgBarriers.data()
        );
    }
    return true;
}

bool Synchronization2Module::on_cmd_pipeline_barrier2(
    VkCommandBuffer commandBuffer,
    const VkDependencyInfo* pDependencyInfo
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    if (!pDependencyInfo) return true;

    VkPipelineStageFlags combinedSrc = 0;
    VkPipelineStageFlags combinedDst = 0;

    std::vector<VkMemoryBarrier> v1MemBarriers;
    v1MemBarriers.reserve(pDependencyInfo->memoryBarrierCount);
    for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; ++i) {
        const auto& b = pDependencyInfo->pMemoryBarriers[i];
        combinedSrc |= stage_flags2_to_stage_flags(b.srcStageMask, false);
        combinedDst |= stage_flags2_to_stage_flags(b.dstStageMask, true);

        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.pNext = nullptr;
        mb.srcAccessMask = access_flags2_to_access_flags(b.srcAccessMask);
        mb.dstAccessMask = access_flags2_to_access_flags(b.dstAccessMask);
        v1MemBarriers.push_back(mb);
    }

    std::vector<VkBufferMemoryBarrier> v1BufBarriers;
    v1BufBarriers.reserve(pDependencyInfo->bufferMemoryBarrierCount);
    for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; ++i) {
        const auto& b = pDependencyInfo->pBufferMemoryBarriers[i];
        combinedSrc |= stage_flags2_to_stage_flags(b.srcStageMask, false);
        combinedDst |= stage_flags2_to_stage_flags(b.dstStageMask, true);

        VkBufferMemoryBarrier bb{};
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.pNext = nullptr;
        bb.srcAccessMask = access_flags2_to_access_flags(b.srcAccessMask);
        bb.dstAccessMask = access_flags2_to_access_flags(b.dstAccessMask);
        bb.srcQueueFamilyIndex = b.srcQueueFamilyIndex;
        bb.dstQueueFamilyIndex = b.dstQueueFamilyIndex;
        bb.buffer = b.buffer;
        bb.offset = b.offset;
        bb.size = b.size;
        v1BufBarriers.push_back(bb);
    }

    std::vector<VkImageMemoryBarrier> v1ImgBarriers;
    v1ImgBarriers.reserve(pDependencyInfo->imageMemoryBarrierCount);
    for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; ++i) {
        const auto& b = pDependencyInfo->pImageMemoryBarriers[i];
        combinedSrc |= stage_flags2_to_stage_flags(b.srcStageMask, false);
        combinedDst |= stage_flags2_to_stage_flags(b.dstStageMask, true);

        VkImageMemoryBarrier ib{};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.pNext = nullptr;
        ib.srcAccessMask = access_flags2_to_access_flags(b.srcAccessMask);
        ib.dstAccessMask = access_flags2_to_access_flags(b.dstAccessMask);
        ib.oldLayout = sanitize_barrier_layout(b.oldLayout, b.subresourceRange.aspectMask);
        ib.newLayout = sanitize_barrier_layout(b.newLayout, b.subresourceRange.aspectMask);
        ib.srcQueueFamilyIndex = b.srcQueueFamilyIndex;
        ib.dstQueueFamilyIndex = b.dstQueueFamilyIndex;
        ib.image = b.image;
        ib.subresourceRange = b.subresourceRange;
        v1ImgBarriers.push_back(ib);
    }

    if (combinedSrc == 0) combinedSrc = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (combinedDst == 0) combinedDst = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    PFN_vkCmdPipelineBarrier real_fn =
        (PFN_vkCmdPipelineBarrier) get_real_proc(get_last_instance(), device, "vkCmdPipelineBarrier");
    if (real_fn) {
        real_fn(
            commandBuffer,
            combinedSrc,
            combinedDst,
            pDependencyInfo->dependencyFlags,
            (uint32_t)v1MemBarriers.size(),
            v1MemBarriers.data(),
            (uint32_t)v1BufBarriers.size(),
            v1BufBarriers.data(),
            (uint32_t)v1ImgBarriers.size(),
            v1ImgBarriers.data()
        );
    }
    return true;
}

bool Synchronization2Module::on_cmd_write_timestamp2(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags2 stage,
    VkQueryPool queryPool,
    uint32_t query
) {
    VkDevice device = LayerManager::get().get_device_for_cmd(commandBuffer);
    if (is_device_native(device)) return false;

    VkPipelineStageFlags flags = stage_flags2_to_stage_flags(stage, false);
    VkPipelineStageFlagBits singleStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    if (flags & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) singleStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    else if (flags & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) singleStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    else if (flags & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) singleStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    else if (flags & VK_PIPELINE_STAGE_TRANSFER_BIT) singleStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    else if (flags & VK_PIPELINE_STAGE_VERTEX_SHADER_BIT) singleStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    else if (flags & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) singleStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    else {
        uint32_t bit = flags & (-((int32_t)flags));
        if (bit) singleStage = (VkPipelineStageFlagBits)bit;
    }

    PFN_vkCmdWriteTimestamp real_fn =
        (PFN_vkCmdWriteTimestamp) get_real_proc(get_last_instance(), device, "vkCmdWriteTimestamp");
    if (real_fn) {
        real_fn(commandBuffer, singleStage, queryPool, query);
    }
    return true;
}

bool Synchronization2Module::on_queue_submit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence,
    VkResult& outResult
) {
    VkDevice device = LayerManager::get().get_device_for_queue(queue);
    if (is_device_native(device)) return false;

    if (submitCount == 0 || !pSubmits) {
        PFN_vkQueueSubmit real_fn =
            (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), device, "vkQueueSubmit");
        if (real_fn) {
            outResult = real_fn(queue, 0, nullptr, fence);
        } else {
            outResult = VK_ERROR_INITIALIZATION_FAILED;
        }
        return true;
    }

    struct SubmitStorage {
        std::vector<VkSemaphore> waitSemaphores;
        std::vector<VkPipelineStageFlags> waitDstStageMask;
        std::vector<uint64_t> waitValues;
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkSemaphore> signalSemaphores;
        std::vector<uint64_t> signalValues;
        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        VkProtectedSubmitInfo protectedInfo{};
        bool hasTimelineValues = false;
        bool hasProtected = false;
    };

    std::vector<SubmitStorage> storages(submitCount);
    std::vector<VkSubmitInfo> v1Submits(submitCount);

    for (uint32_t s = 0; s < submitCount; ++s) {
        const auto& sub2 = pSubmits[s];
        auto& storage = storages[s];

        if (sub2.pWaitSemaphoreInfos && sub2.waitSemaphoreInfoCount > 0) {
            storage.waitSemaphores.reserve(sub2.waitSemaphoreInfoCount);
            storage.waitDstStageMask.reserve(sub2.waitSemaphoreInfoCount);
            storage.waitValues.reserve(sub2.waitSemaphoreInfoCount);
            for (uint32_t i = 0; i < sub2.waitSemaphoreInfoCount; ++i) {
                const auto& w = sub2.pWaitSemaphoreInfos[i];
                storage.waitSemaphores.push_back(w.semaphore);
                VkPipelineStageFlags stage = stage_flags2_to_stage_flags(w.stageMask, true);
                if (stage == 0) stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                storage.waitDstStageMask.push_back(stage);
                storage.waitValues.push_back(w.value);
                if (w.value != 0) storage.hasTimelineValues = true;
            }
        }

        if (sub2.pCommandBufferInfos && sub2.commandBufferInfoCount > 0) {
            storage.commandBuffers.reserve(sub2.commandBufferInfoCount);
            for (uint32_t i = 0; i < sub2.commandBufferInfoCount; ++i) {
                storage.commandBuffers.push_back(sub2.pCommandBufferInfos[i].commandBuffer);
            }
        }

        if (sub2.pSignalSemaphoreInfos && sub2.signalSemaphoreInfoCount > 0) {
            storage.signalSemaphores.reserve(sub2.signalSemaphoreInfoCount);
            storage.signalValues.reserve(sub2.signalSemaphoreInfoCount);
            for (uint32_t i = 0; i < sub2.signalSemaphoreInfoCount; ++i) {
                const auto& sig = sub2.pSignalSemaphoreInfos[i];
                storage.signalSemaphores.push_back(sig.semaphore);
                storage.signalValues.push_back(sig.value);
                if (sig.value != 0) storage.hasTimelineValues = true;
            }
        }

        auto& sub1 = v1Submits[s];
        sub1.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        sub1.pNext = sub2.pNext;
        sub1.waitSemaphoreCount = (uint32_t)storage.waitSemaphores.size();
        sub1.pWaitSemaphores = storage.waitSemaphores.data();
        sub1.pWaitDstStageMask = storage.waitDstStageMask.data();
        sub1.commandBufferCount = (uint32_t)storage.commandBuffers.size();
        sub1.pCommandBuffers = storage.commandBuffers.data();
        sub1.signalSemaphoreCount = (uint32_t)storage.signalSemaphores.size();
        sub1.pSignalSemaphores = storage.signalSemaphores.data();

        if (sub2.flags & VK_SUBMIT_PROTECTED_BIT) {
            storage.hasProtected = true;
            storage.protectedInfo.sType = VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO;
            storage.protectedInfo.pNext = sub1.pNext;
            storage.protectedInfo.protectedSubmit = VK_TRUE;
            sub1.pNext = &storage.protectedInfo;
        }

        if (storage.hasTimelineValues) {
            storage.timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            storage.timelineInfo.pNext = sub1.pNext;
            storage.timelineInfo.waitSemaphoreValueCount = (uint32_t)storage.waitValues.size();
            storage.timelineInfo.pWaitSemaphoreValues = storage.waitValues.data();
            storage.timelineInfo.signalSemaphoreValueCount = (uint32_t)storage.signalValues.size();
            storage.timelineInfo.pSignalSemaphoreValues = storage.signalValues.data();
            sub1.pNext = &storage.timelineInfo;
        }
    }

    PFN_vkQueueSubmit real_fn =
        (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), device, "vkQueueSubmit");
    if (real_fn) {
        outResult = real_fn(queue, (uint32_t)v1Submits.size(), v1Submits.data(), fence);
    } else {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
    }
    return true;
}

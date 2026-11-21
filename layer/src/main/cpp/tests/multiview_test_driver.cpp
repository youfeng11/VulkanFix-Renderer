// Real-GPU provider exposing a Vulkan1.0 device without native multiview.
// Explicit test target only: never packaged with the renderer.
#include <vulkan/vulkan.h>
#include <spirv/unified1/spirv.hpp>
#include <spirv-tools/libspirv.hpp>
#include "core/extension_dependencies.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <map>
#include <new>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))
namespace {
PFN_vkGetInstanceProcAddr gipa{};
PFN_vkGetDeviceProcAddr gdpa{};
VkInstance last_instance{};
std::once_flag initialized;
std::atomic<unsigned> bad_device{0}, bad_pass{0}, bad_shader{0}, shaders{0}, passes{0};
std::atomic<unsigned> projected_extensions{0};
std::mutex image_mutex;
std::map<std::pair<VkDevice, VkImage>, VkImageType> image_types;
std::atomic<unsigned> bad_image_copy{0};
std::atomic<unsigned> bad_image_flags{0}, bad_volume_view{0};
std::atomic<unsigned> bad_image_query{0}, bad_volume_barrier{0};
bool trace_enabled() {
    static const bool enabled = [] { const auto value = std::getenv("VULKAN_FIX_TEST_TRACE_MULTIVIEW"); return value && !std::strcmp(value, "1"); }();
    return enabled;
}
bool hide_relaxed_layout() {
    const auto* value = std::getenv("VULKAN_FIX_TEST_HIDE_RELAXED_LAYOUT");
    return value && !std::strcmp(value, "1");
}
bool hide_maintenance2() {
    const auto* value = std::getenv("VULKAN_FIX_TEST_HIDE_MAINTENANCE2");
    return value && !std::strcmp(value, "1");
}
bool hide_maintenance1() {
    const auto* value = std::getenv("VULKAN_FIX_TEST_HIDE_MAINTENANCE1");
    return value && !std::strcmp(value, "1");
}
void legacy_format_bits(VkFormatProperties& out) {
    if (!hide_maintenance1()) return;
    const auto transfer = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    out.linearTilingFeatures &= ~transfer; out.optimalTilingFeatures &= ~transfer;
}
void summary() {
    std::fprintf(stderr, "Software-multiview provider: bad device/pass/shader=%u/%u/%u; native passes=%u; shaders=%u; projected extensions=%u\n",
        bad_device.load(), bad_pass.load(), bad_shader.load(), passes.load(), shaders.load(), projected_extensions.load());
    if (hide_maintenance1()) {
        std::fprintf(stderr, "Legacy maintenance1 provider: unsupported mixed image copies=%u\n", bad_image_copy.load());
        std::fprintf(stderr, "Legacy maintenance1 provider: unsupported image flags/views=%u/%u\n",
            bad_image_flags.load(), bad_volume_view.load());
        std::fprintf(stderr, "Legacy maintenance1 provider: unsupported image queries/barrier ranges=%u/%u\n",
            bad_image_query.load(),bad_volume_barrier.load());
    }
}
void init() {
    std::call_once(initialized, [] {
        auto library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (library) {
            gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
            gdpa = (PFN_vkGetDeviceProcAddr)dlsym(library, "vkGetDeviceProcAddr");
        }
        std::atexit(summary);
    });
}
template<class T> T instance_proc(const char* name) { return (T)gipa(last_instance, name); }
VKAPI_ATTR VkResult VKAPI_CALL image_properties(VkPhysicalDevice physical, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* out) {
    if (hide_maintenance1() && (flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) {
        ++bad_image_query; if (out) *out = {}; return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return instance_proc<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties")(physical,format,type,tiling,usage,flags,out);
}
VKAPI_ATTR VkResult VKAPI_CALL image_properties2(VkPhysicalDevice physical, const VkPhysicalDeviceImageFormatInfo2* info,
    VkImageFormatProperties2* out) {
    if (hide_maintenance1() && (info->flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) {
        ++bad_image_query; out->imageFormatProperties = {}; return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return instance_proc<PFN_vkGetPhysicalDeviceImageFormatProperties2>("vkGetPhysicalDeviceImageFormatProperties2")(physical,info,out);
}
VKAPI_ATTR void VKAPI_CALL format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties* out) {
    instance_proc<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical, format, out);
    legacy_format_bits(*out);
}
VKAPI_ATTR void VKAPI_CALL format_properties2(VkPhysicalDevice physical, VkFormat format, VkFormatProperties2* out) {
    instance_proc<PFN_vkGetPhysicalDeviceFormatProperties2>("vkGetPhysicalDeviceFormatProperties2")(physical, format, out);
    legacy_format_bits(out->formatProperties);
}
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* out) {
    instance_proc<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical, out);
    out->apiVersion = VK_API_VERSION_1_0;
}
VKAPI_ATTR void VKAPI_CALL properties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* out) {
    instance_proc<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(physical, out);
    out->properties.apiVersion = VK_API_VERSION_1_0;
    for (auto node = (VkBaseOutStructure*)out->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES) {
            auto value = (VkPhysicalDeviceMultiviewProperties*)node;
            value->maxMultiviewViewCount = value->maxMultiviewInstanceIndex = 0;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES) {
            auto value = (VkPhysicalDeviceVulkan11Properties*)node;
            value->maxMultiviewViewCount = value->maxMultiviewInstanceIndex = 0;
        }
    }
}
VKAPI_ATTR void VKAPI_CALL features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2* out) {
    instance_proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical, out);
    for (auto node = (VkBaseOutStructure*)out->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
            auto value = (VkPhysicalDeviceMultiviewFeatures*)node;
            value->multiview = value->multiviewGeometryShader = value->multiviewTessellationShader = VK_FALSE;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            auto value = (VkPhysicalDeviceVulkan11Features*)node;
            value->multiview = value->multiviewGeometryShader = value->multiviewTessellationShader = VK_FALSE;
        }
    }
}
VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice physical, const char* layer, uint32_t* count, VkExtensionProperties* out) {
    try {
        auto enumerate = instance_proc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            uint32_t size = 0;
            auto result = enumerate(physical, layer, &size, nullptr);
            if (result != VK_SUCCESS) return result;
            std::vector<VkExtensionProperties> values(size);
            result = size ? enumerate(physical, layer, &size, values.data()) : VK_SUCCESS;
            if (result == VK_INCOMPLETE) continue;
            if (result != VK_SUCCESS) return result;
            values.resize(size);
            values.erase(std::remove_if(values.begin(), values.end(), [](const auto& value) {
                return !std::strcmp(value.extensionName, VK_KHR_MULTIVIEW_EXTENSION_NAME) ||
                    (hide_relaxed_layout() && !std::strcmp(value.extensionName, VK_KHR_RELAXED_BLOCK_LAYOUT_EXTENSION_NAME)) ||
                    (hide_maintenance2() && !std::strcmp(value.extensionName, VK_KHR_MAINTENANCE_2_EXTENSION_NAME)) ||
                    (hide_maintenance1() && !std::strcmp(value.extensionName, VK_KHR_MAINTENANCE_1_EXTENSION_NAME));
            }), values.end());
            // A1.0 profile cannot retain extensions requiring Core1.1 or the
            // removed multiview dependency. Project availability, not renderer
            // support: its public emulated extensions are still advertised.
            vkfix::ExtensionEnvironment environment;
            environment.device_api = VK_API_VERSION_1_0;
            if (auto version = (PFN_vkEnumerateInstanceVersion)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"))
                version(&environment.instance_api);
            auto instance_extensions = (PFN_vkEnumerateInstanceExtensionProperties)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
            if (!instance_extensions) return VK_ERROR_INITIALIZATION_FAILED;
            uint32_t instanceCount = 0;
            result = instance_extensions(nullptr, &instanceCount, nullptr);
            if (result != VK_SUCCESS) return result;
            std::vector<VkExtensionProperties> instances(instanceCount);
            result = instanceCount ? instance_extensions(nullptr, &instanceCount, instances.data()) : VK_SUCCESS;
            if (result == VK_INCOMPLETE) continue;
            if (result != VK_SUCCESS) return result;
            for (uint32_t i = 0; i < instanceCount; ++i) environment.enabled_instance.insert(instances[i].extensionName);
            for (const auto& value : values) environment.available.insert(value.extensionName);
            const auto before = values.size();
            values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& value) {
                return vkfix::close_extension_dependencies({value.extensionName}, environment).result != VK_SUCCESS;
            }), values.end());
            projected_extensions = unsigned(before - values.size());
            if (!out) { *count = uint32_t(values.size()); return VK_SUCCESS; }
            const auto written = std::min(*count, uint32_t(values.size()));
            std::copy_n(values.data(), written, out); *count = written;
            return written < values.size() ? VK_INCOMPLETE : VK_SUCCESS;
        }
        return VK_INCOMPLETE;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* out) {
    bool forbidden = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        forbidden |= !std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_MULTIVIEW_EXTENSION_NAME) ||
            (hide_relaxed_layout() && !std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_RELAXED_BLOCK_LAYOUT_EXTENSION_NAME)) ||
            (hide_maintenance2() && !std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_MAINTENANCE_2_EXTENSION_NAME)) ||
            (hide_maintenance1() && !std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_MAINTENANCE_1_EXTENSION_NAME));
    for (auto node = (const VkBaseInStructure*)info->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
            const auto value = (const VkPhysicalDeviceMultiviewFeatures*)node;
            forbidden |= value->multiview || value->multiviewGeometryShader || value->multiviewTessellationShader;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            const auto value = (const VkPhysicalDeviceVulkan11Features*)node;
            forbidden |= value->multiview || value->multiviewGeometryShader || value->multiviewTessellationShader;
        }
    }
    if (forbidden) { ++bad_device; *out = VK_NULL_HANDLE; return VK_ERROR_FEATURE_NOT_PRESENT; }
    return instance_proc<PFN_vkCreateDevice>("vkCreateDevice")(physical, info, allocator, out);
}
VKAPI_ATTR VkResult VKAPI_CALL create_pass(VkDevice device, const VkRenderPassCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* out) {
    for (auto node = (const VkBaseInStructure*)info->pNext; node; node = node->pNext) {
        if (hide_maintenance2() && node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO) {
            std::fprintf(stderr, "Legacy maintenance2 provider: native input attachment aspects were forwarded\n");
            *out = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
            const auto value = (const VkRenderPassMultiviewCreateInfo*)node;
            for (uint32_t i = 0; i < value->subpassCount; ++i) if (value->pViewMasks[i]) {
                ++bad_pass; *out = VK_NULL_HANDLE; return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        }
    }
    ++passes;
    const auto result = ((PFN_vkCreateRenderPass)gdpa(device, "vkCreateRenderPass"))(device, info, allocator, out);
    if (trace_enabled() && result == VK_SUCCESS) {
        std::fprintf(stderr, "MV trace pass=%p attachments=%u\n", (void*)*out, info->attachmentCount);
        for (uint32_t a = 0; a < info->attachmentCount; ++a) {
            const auto& v = info->pAttachments[a];
            std::fprintf(stderr, "  attachment %u format=%u samples=%u load/store=%u/%u layout=%u/%u\n", a, v.format, v.samples, v.loadOp, v.storeOp, v.initialLayout, v.finalLayout);
        }
        for (uint32_t s = 0; s < info->subpassCount; ++s)
            for (uint32_t a = 0; a < info->pSubpasses[s].colorAttachmentCount; ++a)
                std::fprintf(stderr, "  subpass %u color=%u resolve=%u\n", s, info->pSubpasses[s].pColorAttachments[a].attachment,
                    info->pSubpasses[s].pResolveAttachments ? info->pSubpasses[s].pResolveAttachments[a].attachment : VK_ATTACHMENT_UNUSED);
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL create_framebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkFramebuffer* out) {
    const auto result = ((PFN_vkCreateFramebuffer)gdpa(device, "vkCreateFramebuffer"))(device, info, allocator, out);
    if (trace_enabled() && result == VK_SUCCESS) {
        std::fprintf(stderr, "MV trace framebuffer=%p pass=%p attachments=%u layers=%u\n", (void*)*out, (void*)info->renderPass, info->attachmentCount, info->layers);
        for (uint32_t a = 0; a < info->attachmentCount; ++a) std::fprintf(stderr, "  view %u=%p\n", a, (void*)info->pAttachments[a]);
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice device, const VkImageViewCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImageView* out) {
    if (hide_maintenance1() && (info->viewType == VK_IMAGE_VIEW_TYPE_2D || info->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY)) {
        std::lock_guard<std::mutex> lock(image_mutex);
        const auto image = image_types.find({device, info->image});
        if (image != image_types.end() && image->second == VK_IMAGE_TYPE_3D) {
            ++bad_volume_view; *out = VK_NULL_HANDLE; return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    const auto result = ((PFN_vkCreateImageView)gdpa(device, "vkCreateImageView"))(device, info, allocator, out);
    if (trace_enabled() && result == VK_SUCCESS) std::fprintf(stderr, "MV trace view=%p image=%p base/layers=%u/%u\n", (void*)*out, (void*)info->image, info->subresourceRange.baseArrayLayer, info->subresourceRange.layerCount);
    return result;
}
VKAPI_ATTR void VKAPI_CALL begin_pass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    if (trace_enabled()) std::fprintf(stderr, "MV trace begin pass=%p framebuffer=%p\n", (void*)info->renderPass, (void*)info->framebuffer);
    ((PFN_vkCmdBeginRenderPass)gipa(last_instance, "vkCmdBeginRenderPass"))(cmd, info, contents);
}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t first_instance) {
    if (trace_enabled()) std::fprintf(stderr, "MV trace draw=%u/%u first=%u/%u\n", vertices, instances, first, first_instance);
    ((PFN_vkCmdDraw)gipa(last_instance, "vkCmdDraw"))(cmd, vertices, instances, first, first_instance);
}
VKAPI_ATTR VkResult VKAPI_CALL create_pipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* out) {
    if (hide_maintenance2()) for (uint32_t i = 0; i < count; ++i) {
        bool has_evaluation = false;
        for (uint32_t s = 0; s < infos[i].stageCount; ++s)
            has_evaluation |= infos[i].pStages[s].stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        if (!has_evaluation || !infos[i].pTessellationState) continue;
        for (auto node = (const VkBaseInStructure*)infos[i].pTessellationState->pNext; node; node = node->pNext)
            if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO) {
                std::fprintf(stderr, "Legacy maintenance2 provider: native tessellation domain origin was forwarded\n");
                std::fill_n(out, count, VK_NULL_HANDLE); return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
    }
    const auto result = ((PFN_vkCreateGraphicsPipelines)gdpa(device, "vkCreateGraphicsPipelines"))(device, cache, count, infos, allocator, out);
    if (trace_enabled()) for (uint32_t i = 0; i < count; ++i) {
        const auto& v = infos[i];
        std::fprintf(stderr, "MV trace pipeline=%p pass=%p samples=%u discard=%u result=%d\n", (void*)out[i], (void*)v.renderPass,
            v.pMultisampleState ? v.pMultisampleState->rasterizationSamples : 0, v.pRasterizationState ? v.pRasterizationState->rasterizerDiscardEnable : 0, result);
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL bind_pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (trace_enabled()) std::fprintf(stderr, "MV trace bind pipeline=%p\n", (void*)pipeline);
    ((PFN_vkCmdBindPipeline)gipa(last_instance, "vkCmdBindPipeline"))(cmd, point, pipeline);
}
VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice device, const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImage* out) {
    if (hide_maintenance1() && (info->flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) {
        ++bad_image_flags; *out = VK_NULL_HANDLE; return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const auto result = ((PFN_vkCreateImage)gdpa(device, "vkCreateImage"))(device, info, allocator, out);
    if (hide_maintenance1() && result == VK_SUCCESS) { std::lock_guard<std::mutex> lock(image_mutex); image_types[{device,*out}] = info->imageType; }
    if (trace_enabled() && result == VK_SUCCESS) std::fprintf(stderr, "MV trace image=%p extent=%ux%u layers=%u format=%u samples=%u usage=%x\n", (void*)*out, info->extent.width, info->extent.height, info->arrayLayers, info->format, info->samples, info->usage);
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    { std::lock_guard<std::mutex> lock(image_mutex); image_types.erase({device,image}); }
    ((PFN_vkDestroyImage)gdpa(device,"vkDestroyImage"))(device,image,allocator);
}
VKAPI_ATTR void VKAPI_CALL copy_image(VkCommandBuffer cmd, VkImage src, VkImageLayout src_layout,
    VkImage dst, VkImageLayout dst_layout, uint32_t count, const VkImageCopy* regions) {
    VkImageType source = VK_IMAGE_TYPE_MAX_ENUM, target = VK_IMAGE_TYPE_MAX_ENUM;
    { std::lock_guard<std::mutex> lock(image_mutex);
      for (const auto& item : image_types) { if (item.first.second == src) source = item.second; if (item.first.second == dst) target = item.second; } }
    if ((source == VK_IMAGE_TYPE_2D && target == VK_IMAGE_TYPE_3D) || (source == VK_IMAGE_TYPE_3D && target == VK_IMAGE_TYPE_2D)) {
        ++bad_image_copy; return;
    }
    ((PFN_vkCmdCopyImage)gipa(last_instance,"vkCmdCopyImage"))(cmd,src,src_layout,dst,dst_layout,count,regions);
}
VKAPI_ATTR VkResult VKAPI_CALL bind_image(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    if (trace_enabled()) std::fprintf(stderr, "MV trace bind image=%p memory=%p offset=%llu\n", (void*)image, (void*)memory, (unsigned long long)offset);
    return ((PFN_vkBindImageMemory)gdpa(device, "vkBindImageMemory"))(device, image, memory, offset);
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t mc, const VkMemoryBarrier* m, uint32_t bc, const VkBufferMemoryBarrier* b, uint32_t ic, const VkImageMemoryBarrier* images) {
    if (hide_maintenance1()) {
        std::lock_guard<std::mutex> lock(image_mutex);
        for (uint32_t i = 0; i < ic; ++i) for (const auto& image : image_types)
            if (image.first.second == images[i].image && image.second == VK_IMAGE_TYPE_3D &&
                (images[i].subresourceRange.baseArrayLayer || (images[i].subresourceRange.layerCount != 1 &&
                 images[i].subresourceRange.layerCount != VK_REMAINING_ARRAY_LAYERS))) { ++bad_volume_barrier; return; }
    }
    if (trace_enabled()) for (uint32_t i = 0; i < ic; ++i) {
        const auto& v=images[i];
        std::fprintf(stderr, "MV trace barrier image=%p layout=%u/%u layers=%u/%u stages=%x/%x access=%x/%x\n", (void*)v.image, v.oldLayout, v.newLayout, v.subresourceRange.baseArrayLayer,v.subresourceRange.layerCount, src,dst,v.srcAccessMask,v.dstAccessMask);
    }
    ((PFN_vkCmdPipelineBarrier)gipa(last_instance, "vkCmdPipelineBarrier"))(cmd,src,dst,flags,mc,m,bc,b,ic,images);
}
VKAPI_ATTR void VKAPI_CALL copy_to_buffer(VkCommandBuffer cmd,VkImage image,VkImageLayout layout,VkBuffer buffer,uint32_t count,const VkBufferImageCopy* regions) {
    if (trace_enabled()) for(uint32_t i=0;i<count;++i) std::fprintf(stderr,"MV trace copy image=%p buffer=%p layers=%u/%u\n",(void*)image,(void*)buffer,regions[i].imageSubresource.baseArrayLayer,regions[i].imageSubresource.layerCount);
    ((PFN_vkCmdCopyImageToBuffer)gipa(last_instance,"vkCmdCopyImageToBuffer"))(cmd,image,layout,buffer,count,regions);
}
VKAPI_ATTR void VKAPI_CALL resolve_image(VkCommandBuffer cmd,VkImage source,VkImageLayout source_layout,
    VkImage target,VkImageLayout target_layout,uint32_t count,const VkImageResolve* regions) {
    if (trace_enabled()) for(uint32_t i=0;i<count;++i) std::fprintf(stderr,
        "MV trace resolve source=%p target=%p src_mip/layer=%u/%u dst_mip/layer=%u/%u\n",(void*)source,(void*)target,
        regions[i].srcSubresource.mipLevel,regions[i].srcSubresource.baseArrayLayer,
        regions[i].dstSubresource.mipLevel,regions[i].dstSubresource.baseArrayLayer);
    ((PFN_vkCmdResolveImage)gipa(last_instance,"vkCmdResolveImage"))(cmd,source,source_layout,target,target_layout,count,regions);
}
template<bool KHR> VKAPI_ATTR VkResult VKAPI_CALL create_pass2(VkDevice device, const VkRenderPassCreateInfo2* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* out) {
    for (uint32_t i = 0; i < info->subpassCount; ++i) if (info->pSubpasses[i].viewMask) {
        ++bad_pass; *out = VK_NULL_HANDLE; return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    ++passes;
    auto create = (PFN_vkCreateRenderPass2)gdpa(device, KHR ? "vkCreateRenderPass2KHR" : "vkCreateRenderPass2");
    return create ? create(device, info, allocator, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL create_shader(VkDevice device, const VkShaderModuleCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkShaderModule* out) {
    ++shaders;
    const size_t size = info->codeSize / sizeof(uint32_t);
    bool forbidden = size < 5 || info->pCode[1] > 0x10000u;
    for (size_t at = 5; !forbidden && at < size;) {
        const uint32_t count = info->pCode[at] >> 16, op = info->pCode[at] & 0xffffu;
        if (!count || count > size - at) { forbidden = true; break; }
        const auto* words = info->pCode + at;
        forbidden |= op == spv::OpCapability && count >= 2 && words[1] == spv::CapabilityMultiView;
        forbidden |= op == spv::OpDecorate && count >= 4 && words[2] == spv::DecorationBuiltIn && words[3] == spv::BuiltInViewIndex;
        forbidden |= op == spv::OpMemberDecorate && count >= 5 && words[3] == spv::DecorationBuiltIn && words[4] == spv::BuiltInViewIndex;
        if (op == spv::OpExtension && count >= 2) {
            const auto* name = (const char*)(words + 1);
            forbidden |= std::memchr(name, 0, (count - 1) * 4) && !std::strcmp(name, "SPV_KHR_multiview");
        }
        at += count;
    }
    if (!forbidden && hide_relaxed_layout()) {
        spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_0);
        validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
            if (message) std::fprintf(stderr, "Legacy-layout provider: %s\n", message);
        });
        spvtools::ValidatorOptions options;
        options.SetRelaxBlockLayout(false);
        forbidden = !validator.Validate(info->pCode, size, options);
    }
    if (forbidden) { ++bad_shader; *out = VK_NULL_HANDLE; return VK_ERROR_FEATURE_NOT_PRESENT; }
    return ((PFN_vkCreateShaderModule)gdpa(device, "vkCreateShaderModule"))(device, info, allocator, out);
}
PFN_vkVoidFunction intercept(const char* name) {
#define PROC(command, fn) if (!std::strcmp(name, #command)) return (PFN_vkVoidFunction)fn
    PROC(vkGetPhysicalDeviceProperties, properties);
    if (hide_maintenance1()) {
        PROC(vkGetPhysicalDeviceFormatProperties, format_properties);
        PROC(vkGetPhysicalDeviceFormatProperties2, format_properties2); PROC(vkGetPhysicalDeviceFormatProperties2KHR, format_properties2);
        PROC(vkGetPhysicalDeviceImageFormatProperties,image_properties);
        PROC(vkGetPhysicalDeviceImageFormatProperties2,image_properties2); PROC(vkGetPhysicalDeviceImageFormatProperties2KHR,image_properties2);
    }
    PROC(vkGetPhysicalDeviceProperties2, properties2); PROC(vkGetPhysicalDeviceProperties2KHR, properties2);
    PROC(vkGetPhysicalDeviceFeatures2, features2); PROC(vkGetPhysicalDeviceFeatures2KHR, features2);
    PROC(vkEnumerateDeviceExtensionProperties, extensions); PROC(vkCreateDevice, create_device);
    PROC(vkCreateRenderPass, create_pass); PROC(vkCreateRenderPass2, create_pass2<false>);
    if (trace_enabled()) {
        PROC(vkCreateFramebuffer, create_framebuffer); PROC(vkCreateImageView, create_view);
        PROC(vkCmdBeginRenderPass, begin_pass); PROC(vkCmdDraw, draw);
        PROC(vkCreateGraphicsPipelines, create_pipelines); PROC(vkCmdBindPipeline, bind_pipeline);
        PROC(vkCreateImage, create_image); PROC(vkBindImageMemory, bind_image);
        PROC(vkCmdPipelineBarrier, barrier); PROC(vkCmdCopyImageToBuffer, copy_to_buffer);
        PROC(vkCmdResolveImage, resolve_image);
    }
    PROC(vkCreateRenderPass2KHR, create_pass2<true>); PROC(vkCreateShaderModule, create_shader);
    if (hide_maintenance2()) { PROC(vkCreateGraphicsPipelines, create_pipelines); }
    if (hide_maintenance1()) {
        PROC(vkCreateImage, create_image); PROC(vkDestroyImage, destroy_image);
        PROC(vkCreateImageView, create_view); PROC(vkCmdCopyImage, copy_image);
        PROC(vkCmdPipelineBarrier,barrier);
    }
#undef PROC
    return nullptr;
}
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    init();
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (auto result = intercept(name)) return result;
    return gdpa ? gdpa(device, name) : nullptr;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    init(); if (instance) last_instance = instance;
    if (!std::strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (auto result = intercept(name)) return result;
    return gipa ? gipa(instance, name) : nullptr;
}

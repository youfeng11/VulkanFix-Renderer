// Negative controls for POST readbacks. Test-only; never package this library.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#include "../core/driver_trust_shaders.h"
#include "../diagnostics/e5_probe_pattern.h"
#include <array>

namespace {
PFN_vkGetInstanceProcAddr backend() {
    static auto gipa = [] {
        const char *path = std::getenv("VKFIX_POST_TEST_DRIVER");
        void *library = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : nullptr;
        return library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr")) : nullptr;
    }();
    return gipa;
}
bool drop(const char *mode) {
    const char *value = std::getenv("VKFIX_POST_TEST_DROP");
    return value && std::strcmp(value, mode) == 0;
}
PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_properties_native = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties2 queue_properties2_native = nullptr;
VKAPI_ATTR void VKAPI_CALL compute_queues(VkPhysicalDevice physical, uint32_t* count, VkQueueFamilyProperties* values) {
    queue_properties_native(physical, count, values);
    if (values) for (uint32_t i = 0; i < *count; ++i) values[i].queueFlags &= ~VK_QUEUE_GRAPHICS_BIT;
}
VKAPI_ATTR void VKAPI_CALL compute_queues2(VkPhysicalDevice physical, uint32_t* count, VkQueueFamilyProperties2* values) {
    queue_properties2_native(physical, count, values);
    if (values) for (uint32_t i = 0; i < *count; ++i) values[i].queueFamilyProperties.queueFlags &= ~VK_QUEUE_GRAPHICS_BIT;
}
VKAPI_ATTR void VKAPI_CALL copy_buffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy *) {}
VKAPI_ATTR void VKAPI_CALL reset_event2(VkCommandBuffer, VkEvent, VkPipelineStageFlags2) {}
VKAPI_ATTR void VKAPI_CALL skip_timestamp_write(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL skip_timestamp_copy(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t,
    VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
PFN_vkQueueSubmit2 native_submit2 = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL drop_submit2_consumer(VkQueue queue, uint32_t count,
    const VkSubmitInfo2 *submits, VkFence fence) {
    if (count != 2) return native_submit2(queue, count, submits, fence);
    VkSubmitInfo2 batches[]{submits[0], submits[1]};
    batches[1].commandBufferInfoCount = 0;
    batches[1].pCommandBufferInfos = nullptr;
    return native_submit2(queue, count, batches, fence);
}
VKAPI_ATTR void VKAPI_CALL copy_image(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy *) {}
PFN_vkCmdCopyImage copy_image_native = nullptr;
VKAPI_ATTR void VKAPI_CALL copy_first_slice(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkImage dst,
                                           VkImageLayout dst_layout, uint32_t count, const VkImageCopy *regions) {
    if (count != 1 || regions[0].extent.depth <= 1) {
        copy_image_native(cb, src, src_layout, dst, dst_layout, count, regions);
        return;
    }
    auto first = regions[0];
    first.srcSubresource.layerCount = 1;
    first.extent.depth = 1;
    copy_image_native(cb, src, src_layout, dst, dst_layout, 1, &first);
}
VKAPI_ATTR void VKAPI_CALL drop_image_copy(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                                           uint32_t, const VkImageCopy *) {}
VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL indirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
PFN_vkBeginCommandBuffer begin_native = nullptr;
PFN_vkCmdSetViewport viewport_native = nullptr;
PFN_vkCmdSetScissor scissor_native = nullptr;
PFN_vkCmdDraw draw_native = nullptr;
unsigned discard_probe_draws = 0;
bool viewport_reissued = false, scissor_reissued = false;
VKAPI_ATTR VkResult VKAPI_CALL discard_begin(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
    discard_probe_draws = 0;
    viewport_reissued = scissor_reissued = false;
    return begin_native(cb, info);
}
VKAPI_ATTR void VKAPI_CALL discard_viewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport *values) {
    if (discard_probe_draws == 1) viewport_reissued = true;
    viewport_native(cb, first, count, values);
}
VKAPI_ATTR void VKAPI_CALL discard_scissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D *values) {
    if (discard_probe_draws == 1) scissor_reissued = true;
    scissor_native(cb, first, count, values);
}
VKAPI_ATTR void VKAPI_CALL discard_draw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances,
                                       uint32_t first_vertex, uint32_t first_instance) {
    if (++discard_probe_draws == 2 &&
        (!(viewport_reissued && scissor_reissued) || drop("discard-state-reemit-fail"))) {
        VkRect2D clipped{{2, 3}, {0, 0}};
        if (drop("discard-state-corrupt")) clipped.extent = {1, 1};
        scissor_native(cb, 0, 1, &clipped);
    }
    draw_native(cb, vertices, instances, first_vertex, first_instance);
}
VKAPI_ATTR void VKAPI_CALL fill(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL reset_query(VkDevice, VkQueryPool, uint32_t, uint32_t) {}
PFN_vkResetQueryPool native_reset_query = nullptr;
VKAPI_ATTR void VKAPI_CALL reset_query_too_wide(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count) {
    if (first == 1 && count == 2) native_reset_query(device, pool, 0, 3);
    else native_reset_query(device, pool, first, count);
}
PFN_vkCreateGraphicsPipelines create_graphics = nullptr;
PFN_vkCreateShaderModule create_shader = nullptr;
PFN_vkGetPhysicalDeviceProperties physical_properties = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL version10(uint32_t *version) { *version=VK_API_VERSION_1_0; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL properties10(VkPhysicalDevice physical,VkPhysicalDeviceProperties *properties) {
    physical_properties(physical,properties); properties->apiVersion=VK_API_VERSION_1_0;
}
VKAPI_ATTR void VKAPI_CALL properties_other(VkPhysicalDevice physical,VkPhysicalDeviceProperties *properties) {
    physical_properties(physical,properties);
    properties->vendorID=0x1234; properties->deviceID=0x5678; properties->driverVersion=99;
    properties->pipelineCacheUUID[0]^=0x5a;
}
VKAPI_ATTR VkResult VKAPI_CALL healthy_shader(VkDevice device,const VkShaderModuleCreateInfo *info,const VkAllocationCallbacks *allocator,VkShaderModule *out) {
    auto modified=*info;
    if (info->codeSize==sizeof vkfix::nativeprobe::sampler_subject &&
        !std::memcmp(info->pCode,vkfix::nativeprobe::sampler_subject,info->codeSize)) {
        modified.pCode=vkfix::nativeprobe::sampler_control; modified.codeSize=sizeof vkfix::nativeprobe::sampler_control;
    }
    if (info->codeSize==sizeof vkfix::nativeprobe::uniform_subject &&
        !std::memcmp(info->pCode,vkfix::nativeprobe::uniform_subject,info->codeSize)) {
        modified.pCode=vkfix::nativeprobe::uniform_control; modified.codeSize=sizeof vkfix::nativeprobe::uniform_control;
    }
    return create_shader(device,&modified,allocator,out);
}
VKAPI_ATTR VkResult VKAPI_CALL no_divisor(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    std::vector<VkGraphicsPipelineCreateInfo> modified(infos,infos+count);
    std::vector<VkPipelineVertexInputStateCreateInfo> inputs(count);
    for (uint32_t i=0; i<count; ++i) if (infos[i].pVertexInputState) {
        inputs[i]=*infos[i].pVertexInputState;
        auto head=static_cast<const VkBaseInStructure *>(inputs[i].pNext);
        if (head && head->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT)
            inputs[i].pNext=head->pNext;
        modified[i].pVertexInputState=&inputs[i];
    }
    return create_graphics(device,cache,count,modified.data(),allocator,pipelines);
}
PFN_vkGetDeviceProcAddr device_proc = nullptr;
PFN_vkGetDeviceQueue ordinary_queue_native = nullptr;
VKAPI_ATTR void VKAPI_CALL queue2_bridge(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue) {
    *queue = VK_NULL_HANDLE;
    if (info && !info->flags) ordinary_queue_native(device, info->queueFamilyIndex, info->queueIndex, queue);
}
VKAPI_ATTR void VKAPI_CALL queue2_missing(VkDevice, const VkDeviceQueueInfo2 *, VkQueue *queue) {
    *queue = VK_NULL_HANDLE;
}
VKAPI_ATTR void VKAPI_CALL queue1_missing(VkDevice, uint32_t, uint32_t, VkQueue *queue) {
    *queue = VK_NULL_HANDLE;
}
PFN_vkCmdCopyImageToBuffer copy_e5_native = nullptr;
PFN_vkCmdPipelineBarrier e5_barrier = nullptr;
PFN_vkCmdUpdateBuffer e5_update = nullptr;
VKAPI_ATTR void VKAPI_CALL replace_e5_readback(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
    VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    copy_e5_native(cmd, image, layout, buffer, count, regions);
    if (count != 1 || regions[0].imageExtent.width != 30 || regions[0].imageExtent.height != 30) return;
    std::array<uint32_t, 900> words{};
    const bool corrupt = drop("e5-transfer-corrupt");
    for (uint32_t i = 0; i < words.size(); ++i) {
        const auto word = vkfix::nativeprobe::e5_probe_word(i);
        words[i] = corrupt ? word ^ uint32_t(i == 0) : vkfix::nativeprobe::canonical_e5_probe_word(word);
    }
    VkBufferMemoryBarrier write{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    write.srcAccessMask = write.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    write.srcQueueFamilyIndex = write.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    write.buffer = buffer;
    write.offset = regions[0].bufferOffset;
    write.size = sizeof words;
    e5_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
               0, nullptr, 1, &write, 0, nullptr);
    e5_update(cmd, buffer, write.offset, write.size, words.data());
}
PFN_vkCreateRenderPass2 create_render_pass2 = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL omit_depth_resolve(VkDevice device, const VkRenderPassCreateInfo2 *info,
    const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
    auto modified = *info;
    std::vector<VkSubpassDescription2> subpasses(info->pSubpasses, info->pSubpasses + info->subpassCount);
    for (auto &subpass : subpasses) {
        const auto *extension = static_cast<const VkBaseInStructure *>(subpass.pNext);
        if (extension && extension->sType == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE)
            subpass.pNext = extension->pNext;
    }
    modified.pSubpasses = subpasses.data();
    return create_render_pass2(device, &modified, allocator, pass);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device(VkDevice device, const char *name) {
    if (!name || !device_proc) return nullptr;
    const char *phase = std::getenv("VKFIX_TEST_PROBE_PHASE");
    if (phase && !std::strcmp(phase, "timestamps")) {
        if (drop("timestamp-write") && !std::strcmp(name, "vkCmdWriteTimestamp"))
            return reinterpret_cast<PFN_vkVoidFunction>(skip_timestamp_write);
        if (drop("timestamp-copy") && !std::strcmp(name, "vkCmdCopyQueryPoolResults"))
            return reinterpret_cast<PFN_vkVoidFunction>(skip_timestamp_copy);
    }
    if (phase && !std::strcmp(phase, "queue2-subject") && !std::strcmp(name, "vkGetDeviceQueue2")) {
        if (drop("queue2-null")) return reinterpret_cast<PFN_vkVoidFunction>(queue2_missing);
        if (drop("queue2-healthy")) {
            ordinary_queue_native = reinterpret_cast<PFN_vkGetDeviceQueue>(device_proc(device, "vkGetDeviceQueue"));
            return ordinary_queue_native ? reinterpret_cast<PFN_vkVoidFunction>(queue2_bridge) : nullptr;
        }
    }
    if (phase && drop("queue2-control-fail") && !std::strcmp(phase, "queue2-control") &&
        !std::strcmp(name, "vkGetDeviceQueue"))
        return reinterpret_cast<PFN_vkVoidFunction>(queue1_missing);
    if (phase && !std::strcmp(name, "vkCmdCopyImageToBuffer")) {
        if (!std::strcmp(phase, "e5-control") && drop("e5-control-fail"))
            return reinterpret_cast<PFN_vkVoidFunction>(copy_image);
        if (!std::strcmp(phase, "e5-buffer-subject") &&
            (drop("e5-transfer-canonicalize") || drop("e5-transfer-corrupt"))) {
            copy_e5_native = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(device_proc(device, name));
            e5_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(device_proc(device, "vkCmdPipelineBarrier"));
            e5_update = reinterpret_cast<PFN_vkCmdUpdateBuffer>(device_proc(device, "vkCmdUpdateBuffer"));
            if (copy_e5_native && e5_barrier && e5_update)
                return reinterpret_cast<PFN_vkVoidFunction>(replace_e5_readback);
        }
    }
    if (phase && drop("depth-resolve-omit") && !std::strcmp(phase, "depth-resolve-subject") &&
        (!std::strcmp(name, "vkCreateRenderPass2") || !std::strcmp(name, "vkCreateRenderPass2KHR"))) {
        create_render_pass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(device_proc(device, name));
        return create_render_pass2 ? reinterpret_cast<PFN_vkVoidFunction>(omit_depth_resolve) : nullptr;
    }
    if (phase && drop("depth-resolve-control-fail") && !std::strcmp(phase, "depth-resolve-control") &&
        !std::strcmp(name, "vkCmdDraw")) return reinterpret_cast<PFN_vkVoidFunction>(draw);
    if (phase && drop("storage-array-control-fail") && !std::strcmp(name, "vkCmdDispatch") &&
        (!std::strcmp(phase, "storage-array-static-control") || !std::strcmp(phase, "storage-array-dynamic-control")))
        return reinterpret_cast<PFN_vkVoidFunction>(dispatch);
    const bool discard_fault = phase &&
        ((drop("discard-state-drop") && (!std::strcmp(phase, "discard-state-subject") ||
                                       !std::strcmp(phase, "discard-state-reemit"))) ||
         (drop("discard-state-corrupt") && !std::strcmp(phase, "discard-state-subject")) ||
         (drop("discard-state-reemit-fail") && !std::strncmp(phase, "discard-state-", 14)) ||
         (drop("discard-state-rasterized-fail") && (!std::strcmp(phase, "discard-state-subject") ||
                                                  !std::strcmp(phase, "discard-state-rasterized"))));
    if (discard_fault && !std::strcmp(name, "vkBeginCommandBuffer")) {
        begin_native = reinterpret_cast<PFN_vkBeginCommandBuffer>(device_proc(device, name));
        return begin_native ? reinterpret_cast<PFN_vkVoidFunction>(discard_begin) : nullptr;
    }
    if (discard_fault && !std::strcmp(name, "vkCmdSetViewport")) {
        viewport_native = reinterpret_cast<PFN_vkCmdSetViewport>(device_proc(device, name));
        return viewport_native ? reinterpret_cast<PFN_vkVoidFunction>(discard_viewport) : nullptr;
    }
    if (discard_fault && !std::strcmp(name, "vkCmdSetScissor")) {
        scissor_native = reinterpret_cast<PFN_vkCmdSetScissor>(device_proc(device, name));
        return scissor_native ? reinterpret_cast<PFN_vkVoidFunction>(discard_scissor) : nullptr;
    }
    if (discard_fault && !std::strcmp(name, "vkCmdDraw")) {
        draw_native = reinterpret_cast<PFN_vkCmdDraw>(device_proc(device, name));
        return draw_native ? reinterpret_cast<PFN_vkVoidFunction>(discard_draw) : nullptr;
    }
    if (phase && drop("sync2-events-reset") && !std::strcmp(phase, "sync2-events-subject") &&
        (!std::strcmp(name, "vkCmdResetEvent2") || !std::strcmp(name, "vkCmdResetEvent2KHR")))
        return reinterpret_cast<PFN_vkVoidFunction>(reset_event2);
    if (phase && drop("sync2-submit-drop-command") && !std::strcmp(phase, "sync2-submit-subject") &&
        (!std::strcmp(name, "vkQueueSubmit2") || !std::strcmp(name, "vkQueueSubmit2KHR"))) {
        native_submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(device_proc(device, name));
        return native_submit2 ? reinterpret_cast<PFN_vkVoidFunction>(drop_submit2_consumer) : nullptr;
    }
    if (phase && drop("sync2-submit-copy") && !std::strcmp(phase, "sync2-submit-subject") &&
        !std::strcmp(name, "vkCmdCopyBuffer"))
        return reinterpret_cast<PFN_vkVoidFunction>(copy_buffer);
    if (phase && drop("image-slices-drop") && !std::strcmp(phase, "image-slices-subject") &&
        !std::strcmp(name, "vkCmdCopyImage")) {
        copy_image_native = reinterpret_cast<PFN_vkCmdCopyImage>(device_proc(device, name));
        return copy_image_native ? reinterpret_cast<PFN_vkVoidFunction>(copy_first_slice) : nullptr;
    }
    if (phase && !std::strcmp(name, "vkCmdCopyImage") &&
        ((drop("image-slices-corrupt") && !std::strcmp(phase, "image-slices-subject")) ||
         (drop("image-slices-control-fail") && !std::strcmp(phase, "image-slices-control"))))
        return reinterpret_cast<PFN_vkVoidFunction>(drop_image_copy);
    if (drop("trust-healthy") && !std::strcmp(name,"vkCreateShaderModule")) {
        create_shader=reinterpret_cast<PFN_vkCreateShaderModule>(device_proc(device,name));
        return reinterpret_cast<PFN_vkVoidFunction>(healthy_shader);
    }
    if (drop("fill") && std::strcmp(name,"vkCmdFillBuffer")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(fill);
    if (!std::strcmp(name,"vkResetQueryPool") || !std::strcmp(name,"vkResetQueryPoolEXT")) {
        if (drop("reset")) return reinterpret_cast<PFN_vkVoidFunction>(reset_query);
        if (drop("reset-range")) {
            native_reset_query = reinterpret_cast<PFN_vkResetQueryPool>(device_proc(device, name));
            return native_reset_query ? reinterpret_cast<PFN_vkVoidFunction>(reset_query_too_wide) : nullptr;
        }
    }
    if (drop("copy") && std::strcmp(name, "vkCmdCopyBuffer") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(copy_buffer);
    if (drop("render") && std::strcmp(name, "vkCmdCopyImageToBuffer") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(copy_image);
    if ((drop("push") || drop("trust-control")) && std::strcmp(name, "vkCmdDispatch") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(dispatch);
    if (drop("indirect") && std::strcmp(name, "vkCmdDrawIndirect") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(indirect);
    if (drop("draw") && std::strcmp(name, "vkCmdDraw") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(draw);
    if (drop("divisor") && std::strcmp(name, "vkCreateGraphicsPipelines") == 0) {
        create_graphics=reinterpret_cast<PFN_vkCreateGraphicsPipelines>(device_proc(device,name));
        return reinterpret_cast<PFN_vkVoidFunction>(no_divisor);
    }
    return device_proc(device, name);
}
}
extern "C" __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    auto gipa = backend();
    if (!gipa || !name) return nullptr;
    if (drop("compute-only") && !std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties")) {
        queue_properties_native = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(instance, name));
        return queue_properties_native ? reinterpret_cast<PFN_vkVoidFunction>(compute_queues) : nullptr;
    }
    if (drop("compute-only") && (!std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2") ||
                                !std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR"))) {
        queue_properties2_native = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(gipa(instance, name));
        return queue_properties2_native ? reinterpret_cast<PFN_vkVoidFunction>(compute_queues2) : nullptr;
    }
    if (drop("api10") && std::strcmp(name,"vkEnumerateInstanceVersion")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(version10);
    if (drop("api10") && std::strcmp(name,"vkGetPhysicalDeviceProperties")==0) {
        physical_properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance,name));
        return reinterpret_cast<PFN_vkVoidFunction>(properties10);
    }
    if (drop("trust-other-identity") && std::strcmp(name,"vkGetPhysicalDeviceProperties")==0) {
        physical_properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance,name));
        return reinterpret_cast<PFN_vkVoidFunction>(properties_other);
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, name));
        return device_proc ? reinterpret_cast<PFN_vkVoidFunction>(get_device) : nullptr;
    }
    return gipa(instance, name);
}

#include "core/layer_manager.h"
#include "modules/vulkan_1_4/push_descriptor.h"
#include <array>
#include <cstring>
#include <map>
#include <set>

namespace {
bool active = false;
bool fail_layout = false;
using Key = std::pair<uint64_t, uint64_t>;
Key key(VkDevice d, uint64_t h) {
    return {(uint64_t)(uintptr_t)d, h};
}
using Elements = std::map<std::pair<uint32_t, uint32_t>, uint64_t>;
struct Set {
    VkDescriptorPool pool;
    Elements values;
};
std::map<Key, Set> sets;
std::set<Key> live_layouts;
std::map<Key, std::vector<VkDescriptorUpdateTemplateEntry>> templates;
std::map<std::pair<VkCommandBuffer, VkPipelineBindPoint>, VkDescriptorSet> bound;
uint64_t next_handle = 0x100000000ULL;
unsigned copies = 0, invalid_copies = 0, pool_destroys = 0;
bool allocation_layout_alive = true, template_layout_valid = true;

VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *,
                                             const VkAllocationCallbacks *,
                                             VkDescriptorSetLayout *out) {
    if (fail_layout)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = (VkDescriptorSetLayout)next_handle++;
    live_layouts.insert(key(d, (uint64_t)*out));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice d, VkDescriptorSetLayout layout,
                                          const VkAllocationCallbacks *) {
    live_layouts.erase(key(d, (uint64_t)layout));
}
VKAPI_ATTR VkResult VKAPI_CALL create_pool(VkDevice, const VkDescriptorPoolCreateInfo *,
                                           const VkAllocationCallbacks *, VkDescriptorPool *out) {
    *out = (VkDescriptorPool)next_handle++;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice d, VkDescriptorPool pool,
                                        const VkAllocationCallbacks *) {
    ++pool_destroys;
    for (auto it = sets.begin(); it != sets.end();)
        if (it->first.first == (uint64_t)(uintptr_t)d && it->second.pool == pool)
            it = sets.erase(it);
        else
            ++it;
}
VKAPI_ATTR VkResult VKAPI_CALL reset_pool(VkDevice d, VkDescriptorPool pool,
                                          VkDescriptorPoolResetFlags) {
    for (auto it = sets.begin(); it != sets.end();)
        if (it->first.first == (uint64_t)(uintptr_t)d && it->second.pool == pool)
            it = sets.erase(it);
        else
            ++it;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice d, const VkDescriptorSetAllocateInfo *info,
                                        VkDescriptorSet *out) {
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
        allocation_layout_alive &= live_layouts.count(key(d, (uint64_t)info->pSetLayouts[i])) != 0;
        out[i] = (VkDescriptorSet)next_handle++;
        sets[key(d, (uint64_t)out[i])] = {info->descriptorPool, {}};
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL update(VkDevice d, uint32_t wc, const VkWriteDescriptorSet *writes,
                                  uint32_t cc, const VkCopyDescriptorSet *cp) {
    // Follow Vulkan's specified writes-before-copies ordering. A combined call
    // that copies old state after new writes must fail the overwrite checks.
    for (uint32_t i = 0; i < wc; ++i) {
        const auto &w = writes[i];
        for (uint32_t n = 0; n < w.descriptorCount; ++n) {
            auto value = w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                             ? (uint64_t)w.pBufferInfo[n].buffer
                             : (uint64_t)w.pImageInfo[n].imageView;
            sets[key(d, (uint64_t)w.dstSet)].values[{w.dstBinding, w.dstArrayElement + n}] = value;
        }
    }
    for (uint32_t i = 0; i < cc; ++i) {
        ++copies;
        if (cp[i].srcBinding == 11)
            ++invalid_copies; // immutable sampler binding
        auto source = sets[key(d, (uint64_t)cp[i].srcSet)].values;
        auto &dest = sets[key(d, (uint64_t)cp[i].dstSet)].values;
        for (uint32_t n = 0; n < cp[i].descriptorCount; ++n) {
            auto from = source.find({cp[i].srcBinding, cp[i].srcArrayElement + n});
            auto to = std::make_pair(cp[i].dstBinding, cp[i].dstArrayElement + n);
            if (from == source.end())
                dest.erase(to);
            else
                dest[to] = from->second;
        }
    }
}
VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout,
                                uint32_t, uint32_t, const VkDescriptorSet *descriptorSets, uint32_t,
                                const uint32_t *) {
    bound[{cmd, point}] = descriptorSets[0];
}
VKAPI_ATTR VkResult VKAPI_CALL create_template(VkDevice d,
                                               const VkDescriptorUpdateTemplateCreateInfo *info,
                                               const VkAllocationCallbacks *,
                                               VkDescriptorUpdateTemplate *out) {
    template_layout_valid &=
        info->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET &&
        live_layouts.count(key(d, (uint64_t)info->descriptorSetLayout));
    *out = (VkDescriptorUpdateTemplate)next_handle++;
    templates[key(d, (uint64_t)*out)].assign(info->pDescriptorUpdateEntries,
                                             info->pDescriptorUpdateEntries +
                                                 info->descriptorUpdateEntryCount);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_template(VkDevice d, VkDescriptorUpdateTemplate t,
                                            const VkAllocationCallbacks *) {
    templates.erase(key(d, (uint64_t)t));
}
VKAPI_ATTR void VKAPI_CALL update_template(VkDevice d, VkDescriptorSet dst,
                                           VkDescriptorUpdateTemplate t, const void *data) {
    for (const auto &e : templates.at(key(d, (uint64_t)t))) {
        for (uint32_t n = 0; n < e.descriptorCount; ++n) {
            VkDescriptorBufferInfo buffer{};
            std::memcpy(&buffer, (const char *)data + e.offset + n * e.stride, sizeof(buffer));
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = dst;
            w.dstBinding = e.dstBinding;
            w.dstArrayElement = e.dstArrayElement + n;
            w.descriptorCount = 1;
            w.descriptorType = e.descriptorType;
            w.pBufferInfo = &buffer;
            update(d, 1, &w, 0, nullptr);
        }
    }
}
class EmulatedPush : public PushDescriptorModule {
    bool is_device_native(VkDevice) override {
        return false;
    }
};
} // namespace

void *push_descriptor_mock_proc(const char *name) {
    if (!active)
        return nullptr;
#define PROC(n, f)                                                                                 \
    if (!strcmp(name, "vk" #n))                                                                    \
    return (void *)f
    PROC(CreateDescriptorSetLayout, create_layout);
    PROC(DestroyDescriptorSetLayout, destroy_layout);
    PROC(CreateDescriptorPool, create_pool);
    PROC(DestroyDescriptorPool, destroy_pool);
    PROC(ResetDescriptorPool, reset_pool);
    PROC(AllocateDescriptorSets, allocate);
    PROC(UpdateDescriptorSets, update);
    PROC(CmdBindDescriptorSets, bind);
    PROC(CreateDescriptorUpdateTemplate, create_template);
    PROC(DestroyDescriptorUpdateTemplate, destroy_template);
    PROC(UpdateDescriptorSetWithTemplate, update_template);
#undef PROC
    return nullptr;
}

void run_push_descriptor_regressions(void (*check)(bool, const char *)) {
    active = true;
    auto &manager = LayerManager::get();
    auto owned = std::make_unique<EmulatedPush>();
    auto &module = *owned;
    manager.register_module(std::move(owned));
    const auto device = (VkDevice)(uintptr_t)0xd000;
    const auto other_device = (VkDevice)(uintptr_t)0xe000;
    manager.init_device_dispatch_table(device);
    manager.init_device_dispatch_table(other_device);
    auto cmd = (VkCommandBuffer)(uintptr_t)0xd123;
    const auto pool = (VkCommandPool)0xd456;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.commandBufferCount = 1;
    module.on_post_allocate_command_buffers(device, &ai, VK_SUCCESS, &cmd);
    auto ordinary = (VkDescriptorSetLayout)0x100001234ULL;
    auto push = (VkDescriptorSetLayout)0x200001234ULL;
    VkDescriptorSetLayoutBinding ordinary_binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                  VK_SHADER_STAGE_ALL, nullptr};
    VkSampler immutable = (VkSampler)0x8888;
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{
        {{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2, VK_SHADER_STAGE_ALL, nullptr},
         {7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
         {11, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &immutable}}};
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sci.bindingCount = 1;
    sci.pBindings = &ordinary_binding;
    module.on_post_create_descriptor_set_layout(device, &sci, VK_SUCCESS, ordinary);
    sci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sci.bindingCount = uint32_t(bindings.size());
    sci.pBindings = bindings.data();
    module.on_post_create_descriptor_set_layout(device, &sci, VK_SUCCESS, push);
    auto layout = (VkPipelineLayout)0x200000001ULL;
    std::array<VkDescriptorSetLayout, 3> layouts{ordinary, push, ordinary};
    VkPushConstantRange range{VK_SHADER_STAGE_ALL, 0, 16};
    VkPipelineLayoutCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pci.setLayoutCount = uint32_t(layouts.size());
    pci.pSetLayouts = layouts.data();
    pci.pushConstantRangeCount = 1;
    pci.pPushConstantRanges = &range;
    module.on_post_create_pipeline_layout(device, &pci, VK_SUCCESS, layout);
    auto compatible_layout = (VkPipelineLayout)0x300000001ULL;
    auto equivalent_push = (VkDescriptorSetLayout)0x300001234ULL;
    module.on_post_create_descriptor_set_layout(device, &sci, VK_SUCCESS, equivalent_push);
    layouts[1] = equivalent_push;
    module.on_post_create_pipeline_layout(device, &pci, VK_SUCCESS, compatible_layout);
    auto incompatible_layout = (VkPipelineLayout)0x400000001ULL;
    range.size = 32;
    module.on_post_create_pipeline_layout(device, &pci, VK_SUCCESS, incompatible_layout);
    range.size = 16;

    VkDescriptorImageInfo images[2]{};
    images[0].imageView = (VkImageView)0x100007777ULL;
    images[1].imageView = (VkImageView)0x200007777ULL;
    VkDescriptorBufferInfo buffer{(VkBuffer)0x10000000aULL, 0, 16};
    VkWriteDescriptorSet writes[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                   {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    writes[0].dstBinding = 3;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].descriptorCount = 2;
    writes[0].pImageInfo = images;
    writes[1].dstBinding = 7;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &buffer;
    auto push_writes = [&](VkPipelineBindPoint point, VkPipelineLayout l, uint32_t count,
                           const VkWriteDescriptorSet *w) {
        return module.on_cmd_push_descriptor_set(cmd, point, l, 1, count, w);
    };
    auto current = [&](VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_GRAPHICS) {
        return bound.at({cmd, point});
    };
    auto value = [&](VkDescriptorSet set, uint32_t binding, uint32_t element = 0) {
        auto &v = sets.at(key(device, (uint64_t)set)).values;
        auto it = v.find({binding, element});
        return it == v.end() ? uint64_t(0) : it->second;
    };
    check(push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, writes),
          "push snapshot fixture uses actual emulation path");
    const auto first = current();
    buffer.buffer = (VkBuffer)0x20000000aULL;
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, writes + 1);
    const auto second = current();
    check(second != first && value(second, 3) == (uint64_t)images[0].imageView &&
              value(second, 3, 1) == (uint64_t)images[1].imageView,
          "partial uniform update retains both shadow image array elements");
    check(value(second, 7) == (uint64_t)buffer.buffer && value(first, 7) == 0x10000000aULL,
          "later pushes overwrite new snapshots without changing earlier draw bindings");
    images[1].imageView = (VkImageView)0x300007777ULL;
    auto single = writes[0];
    single.dstArrayElement = 1;
    single.descriptorCount = 1;
    single.pImageInfo = images + 1;
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, &single);
    check(value(current(), 3) == (uint64_t)images[0].imageView &&
              value(current(), 3, 1) == (uint64_t)images[1].imageView &&
              value(current(), 7) == (uint64_t)buffer.buffer,
          "partial image-array update preserves untouched elements and uniform binding");
    check(invalid_copies == 0 && copies > 0,
          "immutable samplers are never used as descriptor-copy destinations");
    push_writes(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 1, writes + 1);
    check(value(current(VK_PIPELINE_BIND_POINT_COMPUTE), 3) == 0,
          "compute push state starts independently of graphics");
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, compatible_layout, 1, writes + 1);
    check(value(current(), 3) == (uint64_t)images[0].imageView,
          "identically defined distinct layouts preserve incremental push state");
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, incompatible_layout, 1, writes + 1);
    check(value(current(), 3) == 0,
          "incompatible push-constant ranges discard prior push descriptors");
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, writes);
    manager.track_descriptor_binding(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compatible_layout, 2, 1);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, compatible_layout, 1, writes + 1);
    check(value(current(), 3) == (uint64_t)images[0].imageView,
          "binding a compatible higher set preserves lower push descriptors");
    manager.track_descriptor_binding(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, incompatible_layout, 2, 1);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, incompatible_layout, 1, writes + 1);
    check(value(current(), 3) == 0,
          "binding an incompatible higher set disturbs lower push descriptors");
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, writes);
    manager.track_descriptor_binding(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, writes + 1);
    check(value(current(), 3) == (uint64_t)images[0].imageView,
          "binding a compatible lower set preserves shadow descriptors");
    manager.track_descriptor_binding(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, incompatible_layout, 0,
                                     1);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, incompatible_layout, 1, writes + 1);
    check(value(current(), 3) == 0, "binding an incompatible lower set disturbs higher push state");
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, writes);
    manager.track_descriptor_binding(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, writes + 1);
    check(value(current(), 3) == 0,
          "replacing the pushed set with a bound set discards incremental state");
    check(manager.should_intercept_device_proc(device, "vkCmdBindDescriptorSets") &&
              manager.should_intercept_device_proc(device, "vkCmdBindDescriptorSets2KHR") &&
              manager.should_intercept_device_proc(device, "vkDestroyCommandPool"),
          "native proc shortcuts retain descriptor disturbance and pool lifetime hooks");

    VkDescriptorUpdateTemplateEntry entry{
        7, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, sizeof(buffer)};
    VkDescriptorUpdateTemplateCreateInfo ti{
        VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
    ti.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    ti.pipelineLayout = layout;
    ti.set = 1;
    ti.descriptorUpdateEntryCount = 1;
    ti.pDescriptorUpdateEntries = &entry;
    VkDescriptorUpdateTemplate descriptor_template{};
    check(manager.dispatch_create_descriptor_update_template(device, &ti, nullptr,
                                                             &descriptor_template) == VK_SUCCESS &&
              template_layout_valid,
          "push template conversion supplies a live allocation layout");
    push_writes(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 2, writes);
    auto before_template = current(VK_PIPELINE_BIND_POINT_COMPUTE);
    buffer.buffer = (VkBuffer)0x30000000aULL;
    module.on_cmd_push_descriptor_set_with_template(cmd, descriptor_template, layout, 1, &buffer);
    auto after_template = current(VK_PIPELINE_BIND_POINT_COMPUTE);
    check(after_template != before_template &&
              value(after_template, 3) == (uint64_t)images[0].imageView &&
              value(after_template, 7) == (uint64_t)buffer.buffer,
          "compute template updates retain shadow images and use the template bind point");
    check(value(before_template, 7) == 0x20000000aULL,
          "template updates preserve already recorded descriptor snapshots");
    manager.dispatch_destroy_descriptor_update_template(device, descriptor_template, nullptr);
    check(templates.empty(), "template destruction releases native metadata");
    module.on_destroy_descriptor_set_layout(device, push);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, writes);
    check(allocation_layout_alive && value(current(), 3) == (uint64_t)images[0].imageView,
          "pipeline-owned allocation layout survives application set-layout destruction");
    module.on_destroy_pipeline_layout(device, layout);
    range.size = 48;
    module.on_post_create_pipeline_layout(device, &pci, VK_SUCCESS, layout);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, writes + 1);
    check(value(current(), 3) == 0,
          "reused pipeline handles cannot retrieve stale cached layout definitions");
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, compatible_layout, 2, writes);
    module.on_begin_command_buffer(cmd, nullptr);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, compatible_layout, 1, writes + 1);
    check(value(current(), 3) == 0, "begin recording clears all inherited descriptors");
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, compatible_layout, 2, writes);
    manager.track_command_pool_reset(device, pool, false);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, compatible_layout, 1, writes + 1);
    check(value(current(), 3) == 0,
          "command-pool reset clears push snapshots and cached allocations");
    auto pool_count = pool_destroys;
    manager.track_command_pool_reset(device, pool, true);
    check(pool_destroys > pool_count && sets.empty(),
          "command-pool destruction retires every internal descriptor pool");
    module.on_post_allocate_command_buffers(device, &ai, VK_SUCCESS, &cmd);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, compatible_layout, 1, writes + 1);
    check(value(current(), 3) == 0, "reused command handles cannot access freed TLS state");
    // Reuse all application handles on a second logical device with a different definition.
    sci.pBindings = bindings.data();
    module.on_post_create_descriptor_set_layout(other_device, &sci, VK_SUCCESS, equivalent_push);
    module.on_post_create_pipeline_layout(other_device, &pci, VK_SUCCESS, compatible_layout);
    module.on_destroy_device(other_device);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, compatible_layout, 2, writes);
    check(value(current(), 3) == (uint64_t)images[0].imageView,
          "destroying another device preserves this device's pipeline metadata");
    fail_layout = true;
    auto failed_push = (VkDescriptorSetLayout)0x400001234ULL;
    module.on_post_create_descriptor_set_layout(device, &sci, VK_SUCCESS, failed_push);
    fail_layout = false;
    layouts[1] = failed_push;
    auto failed_layout = (VkPipelineLayout)0x500000001ULL;
    module.on_post_create_pipeline_layout(device, &pci, VK_SUCCESS, failed_layout);
    push_writes(VK_PIPELINE_BIND_POINT_GRAPHICS, failed_layout, 2, writes);
    check(manager.command_recording_error(cmd) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
              manager.should_intercept_device_proc(device, "vkEndCommandBuffer"),
          "internal layout allocation failure is exposed at command-buffer completion");
    module.on_reset_command_buffer(cmd, 0);
    check(manager.command_recording_error(cmd) == VK_SUCCESS,
          "command reset clears an internal allocation failure for rerecording");
    module.on_destroy_device(device);
    check(sets.empty() && live_layouts.empty(),
          "device destruction releases descriptor pools and all private layout references");
    manager.remove_device_dispatch_table(device);
    manager.remove_device_dispatch_table(other_device);
    active = false;
}

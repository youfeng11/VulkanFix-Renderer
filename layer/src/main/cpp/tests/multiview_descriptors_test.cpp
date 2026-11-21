#include "core/multiview_descriptors.h"
#include "core/multiview_commands.h"
#include <array>
#include "core/multiview_pipeline_layouts.h"
#include "core/driver_loader.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <algorithm>

static std::atomic<bool> fail_new{false};
void* operator new(size_t size) {
    if (fail_new.exchange(false)) throw std::bad_alloc();
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
using namespace vkfix;
namespace {
template<class T> T handle(uint64_t n) { return (T)(uintptr_t)n; }
using Key = std::pair<VkDevice, uint64_t>;
template<class T> Key key(VkDevice device, T handle) { return {device, DeviceObjectKey::make(device, handle).handle}; }
unsigned failures = 0, serial = 0x10000, allocations = 0, pool_destroys = 0;
bool fail_allocation = false;
VkDescriptorSet forced_set{};
void check(bool okay, const char* label) { printf("%s: %s\n", okay ? "PASS" : "FAIL", label); failures += !okay; }
struct Binding { VkDescriptorSetLayoutBinding info{}; std::vector<VkSampler> immutable; };
struct Element { bool defined = false; VkDescriptorImageInfo image{}; VkDescriptorBufferInfo buffer{}; };
struct Layout { std::vector<Binding> bindings; };
struct Pool { std::map<VkDescriptorType, uint32_t> counts; };
struct Set { VkDescriptorPool pool; std::map<uint32_t, std::vector<Element>> elements; };
std::map<Key, Layout> layouts;
std::map<Key, Pool> pools;
std::map<Key, Set> sets;
std::map<Key, VkImageViewCreateInfo> views;
std::map<Key, VkPushConstantRange> pipeline_layouts;
bool fail_pipeline_layout = false;
unsigned invalid_layout_destroys = 0;
VkPipelineLayout callback_source{};
unsigned layout_callbacks = 0;
std::set<Key> render_passes;
std::map<Key, VkAttachmentDescription> pass_attachment;
std::map<Key, std::vector<VkImageView>> framebuffers;
VkDevice command_device{};
bool in_pass = false;
bool queries_outside = false;
unsigned physical_begins = 0, physical_ends = 0, descriptor_binds = 0, pushes = 0;
std::vector<uint32_t> replayed_views, begin_layers, clear_values;
std::vector<VkAttachmentLoadOp> load_ops;
std::vector<VkAttachmentStoreOp> store_ops;
std::vector<std::pair<unsigned, uint32_t>> phase_events;
uint32_t replay_state = 0;
VkDependencyFlags expected_barrier_flags = 0;
std::vector<VkImageMemoryBarrier> unused_barriers;
std::vector<std::vector<VkImageMemoryBarrier>> layout_batches;
VkImageLayout expected_volume_layout = VK_IMAGE_LAYOUT_UNDEFINED;
unsigned alias_memory_dependencies = 0;
std::vector<std::pair<VkQueryPool,uint32_t>> active_queries;
std::vector<uint32_t> query_begins, query_ends, timestamps;
std::map<std::pair<VkQueryPool,uint32_t>,uint64_t> query_values;
unsigned query_local_barriers = 0;
bool check_group_begin = false, expect_native_group = false;
uint32_t expected_group_area_count = 0;
const VkRect2D group_area{{3,4},{5,6}};
VKAPI_ATTR void VKAPI_CALL begin_query(VkCommandBuffer, VkQueryPool pool, uint32_t query, VkQueryControlFlags) {
    check(in_pass != queries_outside, "native query begins in the selected physical scope");
    active_queries.emplace_back(pool,query); query_begins.push_back(query); query_values[{pool,query}] = 0;
}
VKAPI_ATTR void VKAPI_CALL end_query(VkCommandBuffer, VkQueryPool pool, uint32_t query) {
    const auto found = std::find(active_queries.begin(), active_queries.end(), std::make_pair(pool,query));
    check(in_pass != queries_outside && found != active_queries.end(), "native query ends in the same selected scope");
    if (found != active_queries.end()) active_queries.erase(found);
    query_ends.push_back(query);
}
VKAPI_ATTR void VKAPI_CALL timestamp(VkCommandBuffer, VkPipelineStageFlagBits stage, VkQueryPool, uint32_t query) {
    check(in_pass && stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, "timestamp stage and physical pass are preserved");
    timestamps.push_back(query);
}
VKAPI_ATTR void VKAPI_CALL query_barrier(VkCommandBuffer, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t mc, const VkMemoryBarrier* memory, uint32_t bc, const VkBufferMemoryBarrier*, uint32_t ic, const VkImageMemoryBarrier*) {
    if (in_pass) {
        check(!active_queries.empty() && src == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT && dst == src &&
            flags == VK_DEPENDENCY_BY_REGION_BIT && mc == 1 && !bc && !ic &&
            memory[0].srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT && memory[0].dstAccessMask == VK_ACCESS_SHADER_READ_BIT,
            "a query-spanning barrier uses owned original scopes inside the physical pass");
        ++query_local_barriers;
    } else check(src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == src && !flags && mc == 1,
        "barriers outside query scopes retain global segment synchronization");
}
VKAPI_ATTR void VKAPI_CALL unused_pipeline_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t mc, const VkMemoryBarrier* memory, uint32_t bc, const VkBufferMemoryBarrier*, uint32_t ic, const VkImageMemoryBarrier* images) {
    if (mc) {
        check(!in_pass && MultiviewCommands::replaying() && src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
            dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && !flags && mc == 1 && !bc && !ic &&
            memory[0].srcAccessMask == VK_ACCESS_MEMORY_WRITE_BIT &&
            memory[0].dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
            "alias subpass boundaries synchronize all bound memory, including distinct native images");
        ++alias_memory_dependencies; return;
    }
    check(!in_pass && MultiviewCommands::replaying() && src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
        dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && !flags && !mc && !bc && ic,
        "unused-attachment transitions execute outside physical passes under the replay guard");
    check(!MultiviewCommands::get().unsupported(cmd), "generated transition cannot be recaptured");
    unused_barriers.assign(images, images + ic);
    layout_batches.emplace_back(images, images + ic);
}
VKAPI_ATTR VkResult VKAPI_CALL create_pipeline_layout(VkDevice device, const VkPipelineLayoutCreateInfo* ci,
    const VkAllocationCallbacks* allocator, VkPipelineLayout* out) {
    if (allocator || ci->pNext || ci->flags) return VK_ERROR_UNKNOWN;
    if (fail_pipeline_layout) { *out = handle<VkPipelineLayout>(0xdead); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    for (uint32_t i = 0; i < ci->setLayoutCount; ++i) if (!layouts.count(key(device, ci->pSetLayouts[i]))) return VK_ERROR_UNKNOWN;
    *out = handle<VkPipelineLayout>(++serial);
    pipeline_layouts[key(device, *out)] = ci->pushConstantRangeCount ? ci->pPushConstantRanges[0] : VkPushConstantRange{};
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_pipeline_layout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks*) {
    invalid_layout_destroys += pipeline_layouts.erase(key(device, layout)) != 1;
}
VKAPI_ATTR void VKAPI_CALL reentrant_layout_destroy(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks* allocator) {
    auto retained = MultiviewPipelineLayouts::get().acquire(device,callback_source);
    check(!retained || !retained->native(), "layout retirement callback can reenter registry and resource access without held locks");
    ++layout_callbacks; destroy_pipeline_layout(device,layout,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL create_render_pass(VkDevice device, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks*, VkRenderPass* out) {
    *out = handle<VkRenderPass>(++serial); render_passes.insert(key(device, *out));
    if (ci->attachmentCount) pass_attachment[key(device, *out)] = ci->pAttachments[0];
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_render_pass(VkDevice device, VkRenderPass pass, const VkAllocationCallbacks*) {
    render_passes.erase(key(device, pass)); pass_attachment.erase(key(device, pass));
}
VKAPI_ATTR VkResult VKAPI_CALL create_framebuffer(VkDevice device, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks*, VkFramebuffer* out) {
    if (!render_passes.count(key(device, ci->renderPass)) || ci->layers != 1) return VK_ERROR_UNKNOWN;
    *out = handle<VkFramebuffer>(++serial); auto& attached = framebuffers[key(device, *out)];
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) attached.push_back(ci->pAttachments[i]);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_framebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks*) { framebuffers.erase(key(device, fb)); }
VKAPI_ATTR void VKAPI_CALL begin_pass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    check(!in_pass && contents == VK_SUBPASS_CONTENTS_INLINE, "native replay opens one inline pass per view");
    if (check_group_begin) {
        const auto* group = static_cast<const VkDeviceGroupRenderPassBeginInfo*>(info->pNext);
        if (expect_native_group) {
            check(group && group->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO && group->deviceMask == 1 &&
                group->deviceRenderAreaCount == expected_group_area_count &&
                (!expected_group_area_count || std::memcmp(group->pDeviceRenderAreas,&group_area,sizeof(group_area)) == 0),
                "native device-group replay preserves the owned mask and per-device render area");
        } else {
            const VkRect2D ordinary{{0,0},{16,16}};
            const auto& expected = expected_group_area_count ? group_area : ordinary;
            check(!group && std::memcmp(&info->renderArea,&expected,sizeof(expected)) == 0,
                "singleton replay removes the native group node and honors the selected render area");
        }
    }
    check(!MultiviewCommands::get().capture(cmd, true, []() -> MultiviewCommands::Replay { throw std::runtime_error("recursive replay"); }),
        "replay guard prevents command recapture");
    in_pass = true; ++physical_begins;
    const auto& attached = framebuffers.at(key(command_device, info->framebuffer));
    begin_layers.push_back(views.at(key(command_device, attached.at(0))).subresourceRange.baseArrayLayer);
    clear_values.push_back(info->pClearValues[0].color.uint32[0]);
    load_ops.push_back(pass_attachment.at(key(command_device, info->renderPass)).loadOp);
    store_ops.push_back(pass_attachment.at(key(command_device, info->renderPass)).storeOp);
    if (expected_volume_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        const auto& desc = pass_attachment.at(key(command_device, info->renderPass));
        check(desc.initialLayout == expected_volume_layout && desc.finalLayout == expected_volume_layout,
            "3D slice passes leave whole-mip layouts unchanged for every view");
    }
    replay_state = 0xffffffffu;
}
VKAPI_ATTR void VKAPI_CALL end_pass(VkCommandBuffer) {
    check(in_pass && (active_queries.empty() || queries_outside), "native replay closes a pass without any query begun inside it"); in_pass = false; ++physical_ends;
}
VKAPI_ATTR void VKAPI_CALL bind_sets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout layout, uint32_t first,
    uint32_t count, const VkDescriptorSet* bound, uint32_t dynamic_count, const uint32_t* offsets) {
    check(in_pass && pipeline_layouts.count(key(command_device, layout)) && first == 0 && count == 1 &&
        dynamic_count == 1 && offsets[0] == 64 && sets.count(key(command_device, bound[0])), "descriptor replay owns layout/set array/dynamic offsets");
    ++descriptor_binds;
}
VKAPI_ATTR void VKAPI_CALL push_constants(VkCommandBuffer, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data) {
    uint32_t value{}; if (size == 4) std::memcpy(&value, data, 4);
    check(in_pass && pipeline_layouts.count(key(command_device, layout)) && stages == VK_SHADER_STAGE_VERTEX_BIT &&
        !offset && size == 4 && value == 17, "push-constant replay owns bytes and equivalent private layout"); ++pushes;
}
std::vector<VkImageResolve> repaired_regions;
std::vector<std::array<VkImageMemoryBarrier, 2>> repair_barriers;
VkImage repair_source{}, repair_target{};
VKAPI_ATTR void VKAPI_CALL repair_barrier(VkCommandBuffer, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t mc, const VkMemoryBarrier*, uint32_t bc, const VkBufferMemoryBarrier*,
    uint32_t ic, const VkImageMemoryBarrier* images) {
    check(!in_pass && src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == src && !flags && !mc && !bc && ic == 2,
        "resolve repair synchronizes images outside physical passes with complete producer/consumer scopes");
    if (ic == 2) repair_barriers.push_back({images[0],images[1]});
}
VKAPI_ATTR void VKAPI_CALL repair_resolve(VkCommandBuffer, VkImage source, VkImageLayout source_layout,
    VkImage target, VkImageLayout target_layout, uint32_t count, const VkImageResolve* regions) {
    check(!in_pass && source == repair_source && target == repair_target && count == 1 &&
        source_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && target_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        "resolve repair uses the original images and legal transfer layouts after the physical pass");
    if (count == 1) repaired_regions.push_back(regions[0]);
}
VKAPI_ATTR void VKAPI_CALL pipeline_barrier(VkCommandBuffer, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t count, const VkMemoryBarrier* memory, uint32_t bc, const VkBufferMemoryBarrier*, uint32_t ic, const VkImageMemoryBarrier*) {
    check(!in_pass && src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
        flags == expected_barrier_flags && count == 1 && !bc && !ic &&
        memory[0].srcAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) &&
        memory[0].dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
        "barrier is emitted once outside physical passes and covers generated LOAD/STORE accesses");
    phase_events.emplace_back(9, 0);
}
VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* ci,
    const VkAllocationCallbacks* allocator, VkDescriptorSetLayout* out) {
    if (allocator || ci->flags || ci->pNext) return VK_ERROR_UNKNOWN;
    *out = handle<VkDescriptorSetLayout>(++serial); Layout layout;
    for (uint32_t i = 0; i < ci->bindingCount; ++i) {
        Binding b; b.info = ci->pBindings[i]; b.info.pImmutableSamplers = nullptr;
        if (ci->pBindings[i].pImmutableSamplers)
            b.immutable.assign(ci->pBindings[i].pImmutableSamplers, ci->pBindings[i].pImmutableSamplers + b.info.descriptorCount);
        layout.bindings.push_back(std::move(b));
    }
    layouts[key(device, *out)] = std::move(layout); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks*) {
    layouts.erase(key(device, layout));
}
VKAPI_ATTR VkResult VKAPI_CALL create_pool(VkDevice device, const VkDescriptorPoolCreateInfo* ci,
    const VkAllocationCallbacks* allocator, VkDescriptorPool* out) {
    if (allocator || ci->flags) return VK_ERROR_UNKNOWN;
    *out = handle<VkDescriptorPool>(++serial); Pool pool;
    for (uint32_t i = 0; i < ci->poolSizeCount; ++i) pool.counts[ci->pPoolSizes[i].type] += ci->pPoolSizes[i].descriptorCount;
    pools[key(device, *out)] = std::move(pool); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks*) {
    ++pool_destroys; pools.erase(key(device, pool));
    for (auto it = sets.begin(); it != sets.end();)
        if (it->first.first == device && it->second.pool == pool) it = sets.erase(it); else ++it;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate_sets(VkDevice device, const VkDescriptorSetAllocateInfo* ci, VkDescriptorSet* out) {
    ++allocations;
    if (fail_allocation) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    // Model a YCbCr implementation requiring three pool descriptors for each
    // immutable combined sampler. The application pool is sized accordingly.
    const auto& counts = pools.at(key(device, ci->descriptorPool)).counts;
    auto capacity = counts.find(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    if (capacity != counts.end() && capacity->second < 6) return VK_ERROR_OUT_OF_POOL_MEMORY;
    for (uint32_t i = 0; i < ci->descriptorSetCount; ++i) {
        auto layout = layouts.find(key(device, ci->pSetLayouts[i]));
        if (layout == layouts.end()) return VK_ERROR_UNKNOWN;
        out[i] = forced_set ? forced_set : handle<VkDescriptorSet>(++serial);
        Set set; set.pool = ci->descriptorPool;
        for (const auto& b : layout->second.bindings) {
            auto& values = set.elements[b.info.binding]; values.resize(b.info.descriptorCount);
            if (b.info.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && !b.immutable.empty())
                for (uint32_t n = 0; n < values.size(); ++n) { values[n].defined = true; values[n].image.sampler = b.immutable[n]; }
        }
        sets[key(device, out[i])] = std::move(set);
    }
    return VK_SUCCESS;
}
template<class F> void visit(Set& set, uint32_t binding, uint32_t start, uint32_t count, F f) {
    auto it = set.elements.find(binding);
    for (uint32_t i = 0; i < count; ++i) {
        while (it != set.elements.end() && start >= it->second.size()) { start -= uint32_t(it->second.size()); ++it; }
        if (it == set.elements.end()) { check(false, "fake descriptor range stays in bounds"); return; }
        f(it->second[start++], i);
    }
}
VKAPI_ATTR void VKAPI_CALL update(VkDevice device, uint32_t wc, const VkWriteDescriptorSet* writes, uint32_t cc, const VkCopyDescriptorSet* copies) {
    for (uint32_t i = 0; i < wc; ++i) {
        const auto& w = writes[i]; auto& set = sets.at(key(device, w.dstSet));
        visit(set, w.dstBinding, w.dstArrayElement, w.descriptorCount, [&](Element& out, uint32_t n) {
            out.defined = true;
            if (w.pImageInfo) out.image = w.pImageInfo[n];
            if (w.pBufferInfo) out.buffer = w.pBufferInfo[n];
        });
    }
    for (uint32_t i = 0; i < cc; ++i) {
        const auto& c = copies[i]; auto& source = sets.at(key(device, c.srcSet)); auto& dest = sets.at(key(device, c.dstSet));
        std::vector<Element> values;
        visit(source, c.srcBinding, c.srcArrayElement, c.descriptorCount, [&](const Element& value, uint32_t) { values.push_back(value); });
        visit(dest, c.dstBinding, c.dstArrayElement, c.descriptorCount, [&](Element& value, uint32_t n) { value = values[n]; });
    }
}
VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice device, const VkImageViewCreateInfo* ci,
    const VkAllocationCallbacks*, VkImageView* out) {
    *out = handle<VkImageView>(++serial); views[key(device, *out)] = *ci; views.at(key(device, *out)).pNext = nullptr; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice device, VkImageView view, const VkAllocationCallbacks*) { views.erase(key(device, view)); }
VkDescriptorSet allocate_public(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ci.descriptorPool = pool; ci.descriptorSetCount = 1; ci.pSetLayouts = &layout;
    VkDescriptorSet set{};
    check(MultiviewDescriptors::get().allocate(device, &ci, &set, allocate_sets) == VK_SUCCESS, "allocate public set without FREE_DESCRIPTOR_SET pool flag");
    return set;
}
void write(VkDevice device, uint32_t wc, const VkWriteDescriptorSet* writes, uint32_t cc = 0, const VkCopyDescriptorSet* copies = nullptr) {
    MultiviewDescriptors::get().update(device, wc, writes, cc, copies); update(device, wc, writes, cc, copies);
}
}
extern "C" VkInstance get_last_instance() { return handle<VkInstance>(1); }
extern "C" void* get_real_proc(VkInstance, VkDevice, const char* name) {
#define PROC(n, f) if (!std::strcmp(name, n)) return reinterpret_cast<void*>(f)
    PROC("vkCreateDescriptorSetLayout", create_layout); PROC("vkDestroyDescriptorSetLayout", destroy_layout);
    PROC("vkCreateDescriptorPool", create_pool); PROC("vkDestroyDescriptorPool", destroy_pool);
    PROC("vkAllocateDescriptorSets", allocate_sets); PROC("vkUpdateDescriptorSets", update);
    PROC("vkCreateImageView", create_view); PROC("vkDestroyImageView", destroy_view);
    PROC("vkCreatePipelineLayout", create_pipeline_layout); PROC("vkDestroyPipelineLayout", destroy_pipeline_layout);
    PROC("vkCreateRenderPass", create_render_pass); PROC("vkDestroyRenderPass", destroy_render_pass);
    PROC("vkCreateFramebuffer", create_framebuffer); PROC("vkDestroyFramebuffer", destroy_framebuffer);
#undef PROC
    return nullptr;
}
int main() {
    auto& registry = MultiviewDescriptors::get(); auto& attachments = MultiviewAttachments::get();
    const auto device = handle<VkDevice>(0x100), other = handle<VkDevice>(0x200);
    VkSampler samplers[] = {handle<VkSampler>(0x101), handle<VkSampler>(0x102)};
    VkDescriptorSetLayoutBinding bindings[] = {
        {7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 0, 0, nullptr},
        {9, VK_DESCRIPTOR_TYPE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT, samplers},
        {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT, samplers}};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; lci.bindingCount = 6; lci.pBindings = bindings;
    VkDescriptorSetLayout layout{}; create_layout(device, &lci, nullptr, &layout); registry.remember_layout(device, layout, lci);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 30}, {VK_DESCRIPTOR_TYPE_SAMPLER, 20},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 60}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 10}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pci.maxSets = 10; pci.poolSizeCount = 4; pci.pPoolSizes = sizes;
    VkDescriptorPool pool{}; create_pool(device, &pci, nullptr, &pool); registry.remember_pool(device, pool, pci);
    auto a = allocate_public(device, pool, layout), b = allocate_public(device, pool, layout), undefined = allocate_public(device, pool, layout);
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &layout;
    VkDescriptorSet failed{}; const auto before = allocations; fail_new = true;
    check(registry.allocate(device, &allocation, &failed, allocate_sets) == VK_ERROR_OUT_OF_HOST_MEMORY && !failed && allocations == before,
        "host metadata allocation fails before any native descriptor allocation");
    registry.forget_layout(device, layout); destroy_layout(device, layout, nullptr);
    samplers[0] = handle<VkSampler>(0x999); bindings[0].stageFlags = 0;
    const auto image = handle<VkImage>(0x100000001ull);
    const auto first_view = handle<VkImageView>(0x100000002ull), second_view = handle<VkImageView>(0x200000002ull);
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ici.imageType = VK_IMAGE_TYPE_2D; ici.arrayLayers = 10; ici.mipLevels = 1; ici.extent = {16, 16, 1};
    attachments.remember_image(device, image, ici);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vci.image = image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 9}; attachments.remember_view(device, first_view, vci);
    vci.subresourceRange.baseArrayLayer = 2; vci.subresourceRange.layerCount = 8; attachments.remember_view(device, second_view, vci);
    VkDescriptorImageInfo infos[] = {{}, {}, {}};
    for (auto& i : infos) i = {VK_NULL_HANDLE, first_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = a; w.dstBinding = 1; w.descriptorCount = 3; w.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; w.pImageInfo = infos;
    VkCopyDescriptorSet c{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET}; c.srcSet = a; c.srcBinding = c.dstBinding = 1; c.dstSet = b; c.descriptorCount = 3;
    write(device, 1, &w, 1, &c);
    VkDescriptorBufferInfo buffer{handle<VkBuffer>(0x800), 32, 128};
    VkWriteDescriptorSet buffer_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; buffer_write.dstSet = b; buffer_write.dstBinding = 7;
    buffer_write.descriptorCount = 1; buffer_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; buffer_write.pBufferInfo = &buffer;
    write(device, 1, &buffer_write);
    MultiviewDescriptorArena arena; VkDescriptorSet shadow{};
    check(arena.select(device, b, 4, {{5, 0}, {1, 1}, {1, 0}, {1, 0}}, shadow) == VK_SUCCESS,
        "shadow survives original layout destruction and canonicalizes selected inputs");
    auto& native = sets.at(key(device, shadow));
    check(views.at(key(device, native.elements.at(5)[0].image.imageView)).subresourceRange.baseArrayLayer == 5 &&
        views.at(key(device, native.elements.at(1)[1].image.imageView)).subresourceRange.baseArrayLayer == 5,
        "writes precede copies and spill over sparse/zero-count input bindings");
    check(native.elements.at(7)[0].buffer.buffer == buffer.buffer && native.elements.at(7)[0].buffer.offset == 32 &&
        native.elements.at(9)[0].image.sampler == handle<VkSampler>(0x101), "native copies preserve dynamic buffer descriptors and owned immutable samplers");
    check(allocations > before + 1 && pool_destroys >= 2, "YCbCr-style combined descriptor capacity grows without leaking failed pools");
    auto old_shadow = shadow;
    check(arena.select(device, b, 4, {{1, 0}, {5, 0}, {1, 1}}, shadow) == VK_SUCCESS && shadow == old_shadow, "cache by source identity, revision, view and selected elements");
    VkDescriptorSet whole_shadow{}, whole_again{};
    check(arena.select_all(device,b,0,whole_shadow,true) == VK_SUCCESS &&
        arena.select_all(device,b,0,whole_again,true) == VK_SUCCESS && whole_again == whole_shadow &&
        views.at(key(device,sets.at(key(device,whole_shadow)).elements.at(5)[0].image.imageView)).subresourceRange.baseArrayLayer == 1 &&
        views.at(key(device,sets.at(key(device,whole_shadow)).elements.at(5)[0].image.imageView)).subresourceRange.layerCount == 9 &&
        views.at(key(device,sets.at(key(device,whole_shadow)).elements.at(5)[0].image.imageView)).viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        "ordinary layered input selection preserves the complete native view instead of slicing layer zero");
    VkDescriptorSet sliced_zero{};
    check(arena.select_all(device,b,0,sliced_zero) == VK_SUCCESS && sliced_zero != whole_shadow &&
        views.at(key(device,sets.at(key(device,sliced_zero)).elements.at(5)[0].image.imageView)).subresourceRange.layerCount == 1,
        "ordinary and multiview descriptor snapshots cannot reuse each other's cache entry");
    {
        const auto volume = handle<VkImage>(0x100000091ull);
        const auto volume_view = handle<VkImageView>(0x100000092ull);
        auto volume_info = ici; volume_info.imageType = VK_IMAGE_TYPE_3D; volume_info.arrayLayers = 1; volume_info.extent.depth = 8;
        attachments.remember_image(device,volume,volume_info,true);
        auto volume_view_info = vci; volume_view_info.image = volume; volume_view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
        volume_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        attachments.remember_view(device,volume_view,volume_view_info);
        const auto equivalent_view = handle<VkImageView>(0x100000093ull);
        attachments.remember_view(device,equivalent_view,volume_view_info);
        auto original = attachments.acquire_view(device,volume_view), equivalent = attachments.acquire_view(device,equivalent_view);
        auto backing_image = std::make_shared<MultiviewImage>(); backing_image->handle = handle<VkImage>(0x100000094ull);
        auto backing_info = volume_view_info; backing_info.image = backing_image->handle; backing_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        auto backing = std::make_shared<MultiviewViewResources>(device,backing_info,backing_image,nullptr);
        AttachmentReplayScope::Bindings scope_bindings{{original,backing}};
        {
            AttachmentReplayScope scope(&scope_bindings); VkImageView mapped{};
            check(AttachmentReplayScope::lookup(equivalent.get()) == backing.get() && equivalent->whole(0,mapped) == VK_SUCCESS &&
                views.at(key(device,mapped)).image == backing_image->handle && views.at(key(device,mapped)).viewType == VK_IMAGE_VIEW_TYPE_2D,
                "equivalent input view handles map to the current recording's native backing image before descriptor updates");
        }
        check(!AttachmentReplayScope::lookup(equivalent.get()),"attachment replay mapping does not escape its physical pass scope");
        VkDescriptorImageInfo sampled[] = {{handle<VkSampler>(0x101),volume_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {handle<VkSampler>(0x102),first_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet sample_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; sample_write.dstSet = b;
        sample_write.dstBinding = 10; sample_write.descriptorCount = 2; sample_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sample_write.pImageInfo = sampled;
        fail_new = true;
        const auto translated = registry.update_native_layouts(device,1,&sample_write,0,nullptr,update);
        const auto no_allocation = fail_new.exchange(false);
        check(translated && no_allocation && sampled[0].imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            sets.at(key(device,b)).elements.at(10)[0].image.imageLayout == VK_IMAGE_LAYOUT_GENERAL &&
            sets.at(key(device,b)).elements.at(10)[1].image.imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            "volume descriptor translation uses preallocated scratch, leaves application memory intact and preserves native-image layouts");
        VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET}; copy.srcSet = b; copy.dstSet = a;
        copy.srcBinding = copy.dstBinding = 10; copy.descriptorCount = 2;
        check(registry.update_native_layouts(device,1,&sample_write,1,&copy,update) &&
            sets.at(key(device,a)).elements.at(10)[0].image.imageLayout == VK_IMAGE_LAYOUT_GENERAL,
            "all translated writes complete before native descriptor copies consume their layout values");
        attachments.forget_view(device,equivalent_view); attachments.forget_view(device,volume_view); attachments.forget_image(device,volume);
    }
    buffer.buffer = handle<VkBuffer>(0x801); write(device, 1, &buffer_write);
    check(arena.select(device, b, 4, {{1, 0}, {5, 0}, {1, 1}}, shadow) == VK_SUCCESS && shadow != old_shadow &&
        sets.at(key(device, old_shadow)).elements.at(7)[0].buffer.buffer == handle<VkBuffer>(0x800), "non-input update creates a new snapshot without rewriting an existing recording");

    VkDescriptorUpdateTemplateEntry entry{1, 1, 2, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, sizeof(VkDescriptorImageInfo) + 3};
    VkDescriptorUpdateTemplateCreateInfo tci{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO}; tci.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET; tci.descriptorUpdateEntryCount = 1; tci.pDescriptorUpdateEntries = &entry;
    const auto tmpl = handle<VkDescriptorUpdateTemplate>(0x900); registry.remember_template(device, tmpl, tci);
    std::vector<char> packed(1 + entry.stride + sizeof(VkDescriptorImageInfo));
    VkDescriptorImageInfo replacement{VK_NULL_HANDLE, second_view, VK_IMAGE_LAYOUT_GENERAL};
    std::memcpy(packed.data() + 1, &replacement, sizeof(replacement)); std::memcpy(packed.data() + 1 + entry.stride, &replacement, sizeof(replacement));
    entry.dstBinding = 99; registry.update_template(device, b, tmpl, packed.data());
    // Model the matching native template update with its gathered infos.
    VkDescriptorImageInfo replacements[] = {replacement, replacement}; w.dstSet = b; w.dstArrayElement = 1; w.descriptorCount = 2; w.pImageInfo = replacements;
    update(device, 1, &w, 0, nullptr);
    check(arena.select(device, b, 4, {{1, 1}, {5, 0}}, shadow) == VK_SUCCESS &&
        views.at(key(device, sets.at(key(device, shadow)).elements.at(5)[0].image.imageView)).subresourceRange.baseArrayLayer == 6 &&
        sets.at(key(device, shadow)).elements.at(5)[0].image.imageLayout == VK_IMAGE_LAYOUT_GENERAL,
        "owned template entries gather unaligned, padded image infos across bindings");
    fail_new = true; registry.update_template(device, b, tmpl, packed.data());
    const bool no_allocations = fail_new.exchange(false); check(no_allocations, "preallocated input metadata updates need no host allocations");
    registry.forget_template(device, tmpl);
    c.srcSet = undefined; c.dstSet = b; write(device, 0, nullptr, 1, &c);
    check(arena.select(device, b, 4, {{1, 0}}, shadow) == VK_SUCCESS && !sets.at(key(device, shadow)).elements.at(1)[0].defined,
        "copying undefined descriptors clears old input metadata");
    auto last_shadow = shadow; registry.forget_sets(device, 1, &b); forced_set = b;
    check(!sets.count(key(device, last_shadow)), "freeing public set retires its private pools before immutable sampler lifetime ends");
    // Recreate equivalent public layout; its handle and CI storage may differ.
    create_layout(device, &lci, nullptr, &layout); registry.remember_layout(device, layout, lci);
    b = allocate_public(device, pool, layout); forced_set = VK_NULL_HANDLE;
    check(arena.select(device, b, 4, {{1, 0}}, shadow) == VK_SUCCESS && shadow != last_shadow,
        "reused descriptor-set handle does not inherit a previous shadow identity");
    fail_allocation = true;
    check(arena.select(device, b, 5, {{1, 0}}, shadow) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !shadow, "private allocation failure propagates and retires its pool");
    fail_allocation = false;
    check(arena.select(device, b, 5, {{1, 0}}, shadow) == VK_SUCCESS, "retry private allocation after failure");

    // Identical handle bits are legal on two devices. Keep both records live
    // while destroying just one device, including its arena and native pools.
    layouts[key(other, layout)] = layouts.at(key(device, layout));
    pools[key(other, pool)] = pools.at(key(device, pool));
    registry.remember_layout(other, layout, lci); registry.remember_pool(other, pool, pci);
    forced_set = a; const auto other_set = allocate_public(other, pool, layout); forced_set = VK_NULL_HANDLE;
    attachments.remember_image(other, image, ici); vci.subresourceRange.baseArrayLayer = 1; vci.subresourceRange.layerCount = 9;
    attachments.remember_view(other, first_view, vci);
    w.dstSet = other_set; w.dstArrayElement = 0; w.descriptorCount = 1; w.pImageInfo = infos;
    write(other, 1, &w);
    MultiviewDescriptorArena other_arena; VkDescriptorSet other_shadow{};
    check(other_set == a && other_arena.select(other, other_set, 4, {{1, 0}}, other_shadow) == VK_SUCCESS &&
        views.at(key(other, sets.at(key(other, other_shadow)).elements.at(1)[0].image.imageView)).subresourceRange.baseArrayLayer == 5,
        "descriptor layout/pool/set/view identities are qualified by device and full-width handles");

    // Couple real command capture with view-specific framebuffer and descriptor
    // resources. Only the driver boundary is mocked; no replay actions borrow CI.
    command_device = device;
    w.dstSet = b; w.dstArrayElement = 0; w.descriptorCount = 3; w.pImageInfo = infos; write(device, 1, &w);
    VkAttachmentDescription attachment{}; attachment.format = vci.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpasses[2]{};
    for (auto& subpass : subpasses) { subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color; }
    uint32_t masks[] = {(1u << 2) | (1u << 4), (1u << 0) | (1u << 3)};
    VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO}; mv.subpassCount = 2; mv.pViewMasks = masks;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rci.pNext = &mv; rci.attachmentCount = 1; rci.pAttachments = &attachment; rci.subpassCount = 2; rci.pSubpasses = subpasses;
    VkRenderPass pass{}; VkResult status{};
    check(MultiviewRenderPasses::get().create_fallback(device, &rci, nullptr, &pass, true, status) && status == VK_SUCCESS, "create command replay pass plan");
    const auto framebuffer = handle<VkFramebuffer>(0x3300);
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fci.renderPass = pass; fci.attachmentCount = 1; fci.pAttachments = &first_view; fci.width = fci.height = 16; fci.layers = 1;
    attachments.remember_framebuffer(device, framebuffer, fci);
    const auto public_layout = handle<VkPipelineLayout>(0x3400);
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci.setLayoutCount = 1; plci.pSetLayouts = &layout; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &range;
    fail_pipeline_layout = true;
    check(MultiviewPipelineLayouts::get().remember(device, public_layout, plci) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !invalid_layout_destroys,
        "failed private layout creation never destroys an undefined output handle");
    fail_pipeline_layout = false;
    check(MultiviewPipelineLayouts::get().remember(device, public_layout, plci) == VK_SUCCESS, "prepare independent layout while source set layouts are alive");
    auto& commands = MultiviewCommands::get();
    const auto primary = handle<VkCommandBuffer>(0x3500), secondary = handle<VkCommandBuffer>(0x3600);
    const auto command_pool = handle<VkCommandPool>(0x3700);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(commands.begin(device, primary, command_pool, cbi, true, {begin_pass, end_pass}) == VK_SUCCESS, "begin primary command recording");
    bool factory_called = false;
    check(!commands.capture(primary, false, [&] { factory_called = true; return MultiviewCommands::Replay{}; }) && !factory_called,
        "work outside a multiview pass does not evaluate replay factories");
    std::array<uint32_t, 3> restored{};
    std::array<uint32_t, 2> partial{11, 22};
    const bool first_state = commands.capture(primary, true, [&] {
        factory_called = true;
        return [values = partial, &restored](VkCommandBuffer, uint32_t) {
            restored[0] = values[0]; restored[1] = values[1]; return VK_SUCCESS;
        };
    });
    partial = {33, 44};
    const bool overlapping_state = commands.capture(primary, true, [&] {
        return [values = partial, &restored](VkCommandBuffer, uint32_t) {
            restored[1] = values[0]; restored[2] = values[1]; return VK_SUCCESS;
        };
    });
    partial = {0, 0};
    check(!first_state && !overlapping_state && factory_called && restored == std::array<uint32_t, 3>{},
        "state before a pass owns overlapping inputs while allowing the ordinary native command through");
    auto verify_restored = [&] {
        check(restored == std::array<uint32_t, 3>{11, 33, 44},
            "each view restores ordered partial state set before the logical pass");
        restored = {}; // A preceding view must not supply the next view's state.
    };
    VkClearValue clear{}; clear.color.uint32[0] = 7;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass = pass; rp.framebuffer = framebuffer; rp.renderArea.extent = {16, 16}; rp.clearValueCount = 1; rp.pClearValues = &clear;
    check(commands.begin_pass(primary, &rp, VK_SUBPASS_CONTENTS_INLINE), "capture logical render pass before any native begin");
    VkDescriptorSet set_array[] = {b}; uint32_t dynamic[] = {64}, push_value = 17;
    check(commands.bind_descriptors(primary, VK_PIPELINE_BIND_POINT_GRAPHICS, public_layout, 0, 1, set_array, 1, dynamic, bind_sets) &&
        commands.push_constants(primary, public_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &push_value, push_constants), "capture descriptor and push-constant state");
    set_array[0] = VK_NULL_HANDLE; dynamic[0] = 0; push_value = 0; clear.color.uint32[0] = 999;
    MultiviewPipelineLayouts::get().forget(device, public_layout);
    commands.capture(primary, false, [&] { return [verify_restored](VkCommandBuffer, uint32_t view) { verify_restored(); replayed_views.push_back(view); return VK_SUCCESS; }; });
    check(!physical_begins && commands.next_subpass(primary, VK_SUBPASS_CONTENTS_INLINE), "defer native work until logical subpass completion");
    commands.capture(primary, false, [&] { return [verify_restored](VkCommandBuffer, uint32_t view) { verify_restored(); replayed_views.push_back(100 + view); return VK_SUCCESS; }; });
    commands.end_pass(primary);
    check(commands.error(primary) == VK_SUCCESS && replayed_views == std::vector<uint32_t>{2, 4, 100, 103} &&
        begin_layers == std::vector<uint32_t>{3, 5, 1, 4} && clear_values == std::vector<uint32_t>{7, 7, 7, 7} && descriptor_binds == 4 && pushes == 4,
        "subpasses replay active views with original layer offset and owned clear/state values");
    check(!pipeline_layouts.empty(), "state retains its private binding layout until command-buffer reset");
    commands.begin_pass(primary, &rp, VK_SUBPASS_CONTENTS_INLINE);
    commands.capture(primary, false, [&] { return [verify_restored](VkCommandBuffer, uint32_t) { verify_restored(); return VK_SUCCESS; }; });
    commands.next_subpass(primary, VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
    check(commands.error(primary) == VK_SUCCESS && descriptor_binds == 8 && pushes == 8,
        "a later logical pass inherits descriptor and push-constant state without rebinding");

    VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}; inheritance.renderPass = pass;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT; cbi.pInheritanceInfo = &inheritance;
    check(commands.begin(device, secondary, command_pool, cbi, true, {begin_pass, end_pass}) == VK_SUCCESS, "capture inherited multiview secondary stream");
    commands.capture(secondary, false, [] { return [](VkCommandBuffer, uint32_t view) { replayed_views.push_back(200 + view); return VK_SUCCESS; }; });
    cbi.flags = 0; cbi.pInheritanceInfo = nullptr;
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass, end_pass});
    check(pipeline_layouts.empty(), "command-buffer reset releases retained state layouts");
    commands.begin_pass(primary, &rp, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    check(commands.execute(primary, 1, &secondary), "capture secondary record identity");
    commands.next_subpass(primary, VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
    check(commands.error(primary) == VK_SUCCESS && replayed_views[4] == 202 && replayed_views[5] == 204,
        "secondary actions replay inline in each primary view");
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass, end_pass});
    commands.begin_pass(primary, &rp, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS); commands.execute(primary, 1, &secondary);
    commands.forget(secondary); commands.end_pass(primary);
    check(commands.error(primary) == VK_ERROR_FEATURE_NOT_PRESENT && !in_pass && physical_begins == physical_ends,
        "retired secondary cannot be replaced by handle reuse and replay failure closes the physical pass");
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass, end_pass}); commands.begin_pass(primary, &rp, VK_SUBPASS_CONTENTS_INLINE);
    commands.unsupported(primary); const auto began_before_error = physical_begins; commands.end_pass(primary);
    check(commands.error(primary) == VK_ERROR_FEATURE_NOT_PRESENT && physical_begins == began_before_error,
        "unsupported lowering is returned as recording error without native work");

    // Core multiview self dependencies with multiple views must be VIEW_LOCAL.
    // The implementation may conservatively strengthen these to global sync.
    VkSubpassDependency dependency{0, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT | VK_DEPENDENCY_DEVICE_GROUP_BIT};
    VkRenderPassCreateInfo brci = rci; brci.dependencyCount = 1; brci.pDependencies = &dependency;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderPass barrier_pass{};
    check(MultiviewRenderPasses::get().create_fallback(device, &brci, nullptr, &barrier_pass, true, status) && status == VK_SUCCESS,
        "capture valid view-local self dependency for barrier splitting");
    const auto barrier_fb = handle<VkFramebuffer>(0x3301);
    fci.renderPass = barrier_pass; attachments.remember_framebuffer(device, barrier_fb, fci);
    auto brp = rp; brp.renderPass = barrier_pass; brp.framebuffer = barrier_fb;
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    auto capture_state = [&](VkCommandBuffer cmd, uint32_t base) {
        commands.capture(cmd, true, [base] { return [base](VkCommandBuffer, uint32_t view) { replay_state = base + view; return VK_SUCCESS; }; });
    };
    auto capture_work = [&](VkCommandBuffer cmd, unsigned phase, uint32_t base) {
        commands.capture(cmd, false, [phase, base] { return [phase, base](VkCommandBuffer, uint32_t view) {
            check(replay_state == base + view, "each continuation restores logical state for its own view");
            phase_events.emplace_back(phase, view); return VK_SUCCESS;
        }; });
    };
    auto capture_barrier = [&](VkCommandBuffer cmd, bool native_group = false) {
        return commands.pipeline_barrier(cmd, dependency.srcStageMask, dependency.dstStageMask, dependency.dependencyFlags | VK_DEPENDENCY_DEVICE_GROUP_BIT,
            1, &memory, 0, nullptr, 0, nullptr, native_group, pipeline_barrier);
    };
    load_ops.clear(); store_ops.clear(); phase_events.clear();
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass, end_pass}); commands.begin_pass(primary, &brp, VK_SUBPASS_CONTENTS_INLINE);
    capture_state(primary, 10); capture_work(primary, 1, 10); capture_barrier(primary);
    capture_state(primary, 100); capture_work(primary, 2, 100); capture_barrier(primary); capture_work(primary, 3, 100);
    commands.next_subpass(primary, VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
    check(commands.error(primary) == VK_SUCCESS && phase_events == std::vector<std::pair<unsigned, uint32_t>>{{1,2},{1,4},{9,0},{2,2},{2,4},{9,0},{3,2},{3,4}},
        "all views finish each segment before any view executes after the barrier");
    check(load_ops == std::vector<VkAttachmentLoadOp>{VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_LOAD_OP_LOAD,
        VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_LOAD_OP_CLEAR} &&
        store_ops == std::vector<VkAttachmentStoreOp>{VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE}, "continuations clear only at first use, preserve intermediate data and apply final DONT_CARE once");

    inheritance.renderPass = barrier_pass; cbi.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT; cbi.pInheritanceInfo = &inheritance;
    commands.begin(device, secondary, command_pool, cbi, true, {begin_pass, end_pass});
    capture_state(secondary, 20); capture_work(secondary, 1, 20); capture_barrier(secondary); capture_work(secondary, 2, 20);
    cbi.flags = 0; cbi.pInheritanceInfo = nullptr; phase_events.clear();
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass, end_pass}); commands.begin_pass(primary, &brp, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    commands.execute(primary, 1, &secondary); commands.next_subpass(primary, VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
    check(commands.error(primary) == VK_SUCCESS && phase_events == std::vector<std::pair<unsigned, uint32_t>>{{1,2},{1,4},{9,0},{2,2},{2,4}},
        "secondary barriers split the primary view schedule rather than synchronizing each secondary independently");

    load_ops.clear(); phase_events.clear(); expected_barrier_flags = VK_DEPENDENCY_DEVICE_GROUP_BIT;
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass, end_pass}); commands.begin_pass(primary, &brp, VK_SUBPASS_CONTENTS_INLINE);
    capture_barrier(primary, true); capture_barrier(primary, true);
    commands.next_subpass(primary, VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
    check(commands.error(primary) == VK_SUCCESS && load_ops.size() == 8 && phase_events.size() == 2,
        "leading/consecutive barriers retain empty segments and preserve the enabled device-group flag");
    expected_barrier_flags = 0;

    const auto query_pool = handle<VkQueryPool>(0x6100), statistics_pool = handle<VkQueryPool>(0x6200), time_pool = handle<VkQueryPool>(0x6300);
    auto clear_query_trace = [&] { query_begins.clear(); query_ends.clear(); timestamps.clear(); query_values.clear(); query_local_barriers = 0; };
    auto query_work = [&](VkCommandBuffer cmd, uint32_t amount) {
        commands.capture(cmd, false, [amount] { return [amount](VkCommandBuffer, uint32_t view) {
            for (const auto& active : active_queries) query_values[active] += amount + view;
            return VK_SUCCESS;
        }; });
    };
    auto query_memory_barrier = [&](VkCommandBuffer cmd) {
        VkMemoryBarrier owned{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; owned.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; owned.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        commands.pipeline_barrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT, 1, &owned, 0, nullptr, 0, nullptr, false, query_barrier);
        owned.srcAccessMask = 0; // Captured barriers must not borrow this storage.
    };
    clear_query_trace();
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass,end_pass}); commands.begin_pass(primary, &brp, VK_SUBPASS_CONTENTS_INLINE);
    commands.begin_query(primary, query_pool, 5, VK_QUERY_CONTROL_PRECISE_BIT, begin_query, end_query);
    query_work(primary,1); commands.timestamp(primary,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,time_pool,20,timestamp);
    query_memory_barrier(primary); query_work(primary,10); commands.timestamp(primary,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,time_pool,22,timestamp);
    commands.end_query(primary,query_pool,5,end_query); query_memory_barrier(primary);
    commands.next_subpass(primary,VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
    check(commands.error(primary) == VK_SUCCESS && query_begins == std::vector<uint32_t>{5,6} && query_ends == query_begins &&
        query_values[{query_pool,5}] == 15 && query_values[{query_pool,6}] == 19 && query_local_barriers == 2 &&
        timestamps == std::vector<uint32_t>{20,22,21,23},
        "sparse views map to consecutive query slots; query results and timestamps survive an internal barrier without restarting queries");

    {
        auto ordinary_ci = brci; ordinary_ci.pNext = nullptr; ordinary_ci.dependencyCount = 0;
        VkRenderPass ordinary_pass{};
        check(MultiviewRenderPasses::get().create_fallback(device,&ordinary_ci,nullptr,&ordinary_pass,true,status,false,true) && status == VK_SUCCESS,
            "capture ordinary query replay pass");
        const auto ordinary_fb = handle<VkFramebuffer>(0x3310);
        auto ordinary_fb_ci = fci; ordinary_fb_ci.renderPass = ordinary_pass;
        attachments.remember_framebuffer(device,ordinary_fb,ordinary_fb_ci);
        auto ordinary_rp = brp; ordinary_rp.renderPass = ordinary_pass; ordinary_rp.framebuffer = ordinary_fb;
        clear_query_trace(); queries_outside = true;
        const auto began = physical_begins;
        commands.begin(device,primary,command_pool,cbi,true,{begin_pass,end_pass});
        commands.begin_pass(primary,&ordinary_rp,VK_SUBPASS_CONTENTS_INLINE);
        commands.begin_query(primary,query_pool,5,0,begin_query,end_query);
        query_work(primary,1); query_memory_barrier(primary); query_work(primary,10);
        commands.end_query(primary,query_pool,5,end_query);
        commands.next_subpass(primary,VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
        check(commands.error(primary) == VK_SUCCESS && physical_begins > began + 2 && query_begins == std::vector<uint32_t>{5} &&
            query_ends == query_begins && query_values[{query_pool,5}] == 11 && !query_local_barriers && active_queries.empty(),
            "one ordinary query spans physical pass splits and outside barriers without restarting or changing its slot");
        clear_query_trace();
        commands.begin(device,primary,command_pool,cbi,true,{begin_pass,end_pass});
        commands.begin_pass(primary,&ordinary_rp,VK_SUBPASS_CONTENTS_INLINE);
        commands.begin_query(primary,query_pool,5,0,begin_query,end_query);
        commands.capture(primary,false,[] { return [](VkCommandBuffer,uint32_t) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; }; });
        commands.end_query(primary,query_pool,5,end_query); commands.end_pass(primary);
        check(commands.error(primary) == VK_ERROR_OUT_OF_DEVICE_MEMORY && query_begins == query_ends &&
            active_queries.empty() && !in_pass && physical_begins == physical_ends,
            "replay failure ends its outside query after closing the native pass, allowing safe command-buffer cleanup");
        queries_outside = false;
        commands.forget(primary); attachments.forget_framebuffer(device,ordinary_fb);
        MultiviewRenderPasses::get().forget(device,ordinary_pass); destroy_render_pass(device,ordinary_pass,nullptr);
    }

    // A secondary recorded without a local query can inherit a query from the
    // primary. Barrier selection must inspect the expanded primary stream.
    inheritance.renderPass = barrier_pass; inheritance.occlusionQueryEnable = VK_TRUE;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT; cbi.pInheritanceInfo = &inheritance;
    commands.begin(device,secondary,command_pool,cbi,true,{begin_pass,end_pass}); query_memory_barrier(secondary); query_work(secondary,7);
    cbi.flags = 0; cbi.pInheritanceInfo = nullptr; clear_query_trace();
    commands.begin(device,primary,command_pool,cbi,true,{begin_pass,end_pass}); commands.begin_pass(primary,&brp,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    commands.begin_query(primary,query_pool,5,0,begin_query,end_query); commands.execute(primary,1,&secondary); commands.end_query(primary,query_pool,5,end_query);
    commands.next_subpass(primary,VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
    check(commands.error(primary) == VK_SUCCESS && query_local_barriers == 2 && query_begins == std::vector<uint32_t>{5,6} &&
        query_ends == query_begins && query_values[{query_pool,5}] == 9 && query_values[{query_pool,6}] == 11,
        "secondary barriers stay inside an inherited primary query after stream expansion");

    clear_query_trace();
    commands.begin(device,primary,command_pool,cbi,true,{begin_pass,end_pass}); commands.begin_pass(primary,&brp,VK_SUBPASS_CONTENTS_INLINE);
    commands.begin_query(primary,query_pool,5,0,begin_query,end_query); commands.begin_query(primary,statistics_pool,9,0,begin_query,end_query);
    commands.capture(primary,false,[] { return [](VkCommandBuffer,uint32_t) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; }; });
    commands.end_query(primary,query_pool,5,end_query); commands.end_query(primary,statistics_pool,9,end_query);
    commands.next_subpass(primary,VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
    check(commands.error(primary) == VK_ERROR_OUT_OF_DEVICE_MEMORY && active_queries.empty() &&
        query_begins == std::vector<uint32_t>{5,9} && query_ends == std::vector<uint32_t>{9,5},
        "replay failures close every started query before ending the physical pass, including overlapping query types");

    clear_query_trace();
    commands.begin(device,primary,command_pool,cbi,true,{begin_pass,end_pass}); commands.begin_pass(primary,&brp,VK_SUBPASS_CONTENTS_INLINE);
    commands.begin_query(primary,query_pool,5,0,begin_query,end_query); commands.next_subpass(primary,VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
    check(commands.error(primary) == VK_ERROR_FEATURE_NOT_PRESENT && query_begins.empty() && active_queries.empty(),
        "queries crossing logical subpasses fail before opening an unmatched native query");

    VkAttachmentDescription msaa[2] = {attachment, attachment}; msaa[0].samples = VK_SAMPLE_COUNT_4_BIT;
    VkAttachmentReference resolve{1, VK_IMAGE_LAYOUT_GENERAL}; auto ms_subpass = subpasses[0]; ms_subpass.pResolveAttachments = &resolve;
    auto msmv = mv; msmv.subpassCount = 1;
    auto msci = brci; msci.pNext = &msmv; msci.attachmentCount = 2; msci.pAttachments = msaa; msci.subpassCount = 1; msci.pSubpasses = &ms_subpass;
    auto msplan = MultiviewPassPlan::capture(&msci);
    auto first_chunk = msplan->chunk(0, 2, true, false), last_chunk = msplan->chunk(0, 2, false, true);
    check(first_chunk.resolves.empty() && first_chunk.attachments.size() == 1 && first_chunk.attachments[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
        last_chunk.resolves.size() == 1 && last_chunk.attachments.size() == 2 && last_chunk.attachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD,
        "multisampled attachments are preserved across segments and resolved only by the final chunk");
    // Attachment 1 is used only in subpass 0; its views 0/3 and the wholly
    // unused attachment 2 still need initial-to-final transitions. The union of
    // both subpass masks must be used rather than just the final subpass mask.
    VkAttachmentDescription unused_desc[3] = {attachment, attachment, attachment};
    for (auto& desc : unused_desc) { desc.initialLayout = VK_IMAGE_LAYOUT_GENERAL; desc.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }
    VkAttachmentReference two_colors[] = {color, {1, VK_IMAGE_LAYOUT_GENERAL}};
    VkSubpassDescription unused_subpasses[2] = {subpasses[0], subpasses[1]};
    unused_subpasses[0].colorAttachmentCount = 2; unused_subpasses[0].pColorAttachments = two_colors;
    auto unused_ci = rci; unused_ci.attachmentCount = 3; unused_ci.pAttachments = unused_desc; unused_ci.pSubpasses = unused_subpasses;
    VkRenderPass unused_pass{};
    check(MultiviewRenderPasses::get().create_fallback(device, &unused_ci, nullptr, &unused_pass, true, status) && status == VK_SUCCESS,
        "capture partially and wholly unused attachments with distinct subpass view masks");
    const auto unused_image1 = handle<VkImage>(0x4401), unused_image2 = handle<VkImage>(0x4402);
    const auto unused_view1 = handle<VkImageView>(0x4501), unused_view2 = handle<VkImageView>(0x4502);
    auto unused_vci = vci; unused_vci.pNext = nullptr; unused_vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 9};
    attachments.remember_image(device, unused_image1, ici); attachments.remember_image(device, unused_image2, ici);
    unused_vci.image = unused_image1; attachments.remember_view(device, unused_view1, unused_vci);
    unused_vci.image = unused_image2; attachments.remember_view(device, unused_view2, unused_vci);
    const VkImageView unused_views[] = {first_view, unused_view1, unused_view2};
    auto unused_fci = fci; unused_fci.renderPass = unused_pass; unused_fci.attachmentCount = 3; unused_fci.pAttachments = unused_views;
    const auto unused_fb = handle<VkFramebuffer>(0x3302); attachments.remember_framebuffer(device, unused_fb, unused_fci);
    auto unused_rp = rp; unused_rp.renderPass = unused_pass; unused_rp.framebuffer = unused_fb;
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass, end_pass, unused_pipeline_barrier});
    commands.begin_pass(primary, &unused_rp, VK_SUBPASS_CONTENTS_INLINE);
    commands.next_subpass(primary, VK_SUBPASS_CONTENTS_INLINE);
    check(unused_barriers.empty(), "unused transitions wait until the entire logical pass ends");
    commands.end_pass(primary);
    check(commands.error(primary) == VK_SUCCESS && unused_barriers.size() == 4 &&
        unused_barriers[0].subresourceRange.baseArrayLayer == 1 && unused_barriers[0].subresourceRange.layerCount == 1 &&
        unused_barriers[1].subresourceRange.baseArrayLayer == 4 &&
        unused_barriers[2].subresourceRange.baseArrayLayer == 1 &&
        unused_barriers[3].subresourceRange.baseArrayLayer == 3 && unused_barriers[3].subresourceRange.layerCount == 3 &&
        unused_barriers[0].oldLayout == VK_IMAGE_LAYOUT_GENERAL && unused_barriers[0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        "unused transitions include each inactive attachment/view pair across the complete pass view union");
    const auto volume_image = handle<VkImage>(0x4601); const auto volume_view = handle<VkImageView>(0x4701);
    auto volume_ici = ici; volume_ici.imageType = VK_IMAGE_TYPE_3D; volume_ici.extent.depth = 8; volume_ici.arrayLayers = 1;
    volume_ici.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    attachments.remember_image(device, volume_image, volume_ici);
    auto volume_vci = unused_vci; volume_vci.image = volume_image; volume_vci.subresourceRange.layerCount = 7;
    attachments.remember_view(device, volume_view, volume_vci);
    VkAttachmentReference volume_color0{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, volume_color1{0, VK_IMAGE_LAYOUT_GENERAL};
    auto volume_desc = unused_desc[0]; volume_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkSubpassDescription volume_subpasses[] = {subpasses[0],subpasses[1]};
    volume_subpasses[0].pColorAttachments = &volume_color0; volume_subpasses[1].pColorAttachments = &volume_color1;
    auto volume_ci = rci; volume_ci.pAttachments = &volume_desc; volume_ci.pSubpasses = volume_subpasses;
    VkRenderPass volume_pass{};
    check(MultiviewRenderPasses::get().create_fallback(device, &volume_ci, nullptr, &volume_pass, true, status) && status == VK_SUCCESS,
        "capture 3D attachment with changing subpass layouts and view masks");
    auto volume_fci = fci; volume_fci.renderPass = volume_pass; volume_fci.pAttachments = &volume_view;
    const auto volume_fb = handle<VkFramebuffer>(0x4801); attachments.remember_framebuffer(device, volume_fb, volume_fci);
    auto volume_rp = rp; volume_rp.renderPass = volume_pass; volume_rp.framebuffer = volume_fb;
    layout_batches.clear();
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass,end_pass,unused_pipeline_barrier});
    commands.begin_pass(primary, &volume_rp, VK_SUBPASS_CONTENTS_INLINE);
    expected_volume_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; commands.next_subpass(primary, VK_SUBPASS_CONTENTS_INLINE);
    expected_volume_layout = VK_IMAGE_LAYOUT_GENERAL; commands.end_pass(primary); expected_volume_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(commands.error(primary) == VK_SUCCESS && layout_batches.size() == 3 &&
        layout_batches[0].size() == 1 && layout_batches[1].size() == 1 && layout_batches[2].size() == 1 &&
        layout_batches[0][0].oldLayout == VK_IMAGE_LAYOUT_GENERAL && layout_batches[0][0].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        layout_batches[1][0].oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && layout_batches[1][0].newLayout == VK_IMAGE_LAYOUT_GENERAL &&
        layout_batches[2][0].oldLayout == VK_IMAGE_LAYOUT_GENERAL && layout_batches[2][0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        layout_batches[0][0].image == volume_image && layout_batches[0][0].subresourceRange.baseArrayLayer == 0 &&
        layout_batches[0][0].subresourceRange.layerCount == 1,
        "3D whole-mip transitions occur once per subpass boundary and once at logical pass end");
    VkAttachmentDescription alias_desc[] = {volume_desc,volume_desc};
    alias_desc[0].flags = alias_desc[1].flags = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;
    alias_desc[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL; alias_desc[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAttachmentReference alias_colors[] = {{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkSubpassDescription alias_subpasses[] = {subpasses[0],subpasses[1]};
    alias_subpasses[0].pColorAttachments = &alias_colors[0]; alias_subpasses[1].pColorAttachments = &alias_colors[1];
    auto alias_ci = rci; alias_ci.attachmentCount = 2; alias_ci.pAttachments = alias_desc; alias_ci.pSubpasses = alias_subpasses;
    VkSubpassDependency alias_dep{0,1,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,0};
    alias_ci.dependencyCount = 1; alias_ci.pDependencies = &alias_dep;
    VkRenderPass alias_pass{};
    check(MultiviewRenderPasses::get().create_fallback(device, &alias_ci, nullptr, &alias_pass, true, status) && status == VK_SUCCESS,
        "capture two aliases of the same 3D view used in successive subpasses");
    const VkImageView alias_views[] = {volume_view,volume_view};
    auto alias_fci = fci; alias_fci.renderPass = alias_pass; alias_fci.attachmentCount = 2; alias_fci.pAttachments = alias_views;
    const auto alias_fb = handle<VkFramebuffer>(0x4802); attachments.remember_framebuffer(device, alias_fb, alias_fci);
    auto alias_rp = rp; alias_rp.renderPass = alias_pass; alias_rp.framebuffer = alias_fb;
    const VkClearValue alias_clears[] = {*rp.pClearValues,*rp.pClearValues}; alias_rp.clearValueCount = 2; alias_rp.pClearValues = alias_clears;
    layout_batches.clear(); alias_memory_dependencies = 0;
    commands.begin(device, primary, command_pool, cbi, true, {begin_pass,end_pass,unused_pipeline_barrier});
    commands.begin_pass(primary, &alias_rp, VK_SUBPASS_CONTENTS_INLINE);
    expected_volume_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; commands.next_subpass(primary, VK_SUBPASS_CONTENTS_INLINE);
    check(layout_batches.size() == 2 && layout_batches[1][0].oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        layout_batches[1][0].newLayout == VK_IMAGE_LAYOUT_GENERAL && alias_memory_dependencies == 1,
        "the first alias reaches its final layout before the second alias starts");
    commands.end_pass(primary); expected_volume_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(commands.error(primary) == VK_SUCCESS && layout_batches.size() == 4 && alias_memory_dependencies == 2 &&
        layout_batches[2][0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && layout_batches[2][0].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        layout_batches[3][0].oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && layout_batches[3][0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        "later 3D aliases discard once and end with their own layout without stale transitions from earlier aliases");
    check_group_begin = true;
    for (bool native_group : {false,true}) for (uint32_t area_count : {0u,1u}) {
        VkRenderPass group_pass{};
        check(MultiviewRenderPasses::get().create_fallback(device,&rci,nullptr,&group_pass,true,status,native_group) && status == VK_SUCCESS,
            "create a pass with an immutable native device-group route");
        auto group_fci = fci; group_fci.renderPass = group_pass;
        const auto group_fb = handle<VkFramebuffer>(0x4900); attachments.remember_framebuffer(device,group_fb,group_fci);
        VkRect2D area = group_area;
        VkDeviceGroupRenderPassBeginInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO};
        group.deviceMask = 1; group.deviceRenderAreaCount = area_count; group.pDeviceRenderAreas = &area;
        auto group_rp = rp; group_rp.renderPass = group_pass; group_rp.framebuffer = group_fb; group_rp.pNext = &group;
        expect_native_group = native_group; expected_group_area_count = area_count;
        commands.begin(device,primary,command_pool,cbi,true,{begin_pass,end_pass});
        commands.begin_pass(primary,&group_rp,VK_SUBPASS_CONTENTS_INLINE);
        group.deviceMask = 0; area = {{9,9},{1,1}};
        commands.next_subpass(primary,VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
        check(commands.error(primary) == VK_SUCCESS,"device-group render areas remain valid across every replayed view/subpass");
        attachments.forget_framebuffer(device,group_fb); MultiviewRenderPasses::get().forget(device,group_pass);
        destroy_render_pass(device,group_pass,nullptr);
    }
    check_group_begin = false;
    const auto run_resolve_case = [&](VkImageUsageFlags source_usage, VkImageUsageFlags target_usage,
        VkSampleCountFlagBits samples, VkFormat format, VkAttachmentStoreOp store, bool measured, unsigned expected) {
        VkImageCreateInfo source_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        source_info.imageType = VK_IMAGE_TYPE_2D; source_info.format = format; source_info.extent = {64,64,1};
        source_info.mipLevels = 1; source_info.arrayLayers = 10; source_info.samples = samples; source_info.usage = source_usage;
        auto target_info = source_info; target_info.samples = VK_SAMPLE_COUNT_1_BIT;
        target_info.arrayLayers = 12; target_info.mipLevels = 2; target_info.usage = target_usage;
        repair_source = handle<VkImage>(++serial); repair_target = handle<VkImage>(++serial);
        attachments.remember_image(device,repair_source,source_info); attachments.remember_image(device,repair_target,target_info);
        VkImageViewCreateInfo source_view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        source_view_info.image = repair_source; source_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        source_view_info.format = format; source_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,2,4};
        auto target_view_info = source_view_info; target_view_info.image = repair_target;
        target_view_info.subresourceRange.baseMipLevel = 1; target_view_info.subresourceRange.baseArrayLayer = 4;
        const auto source_view = handle<VkImageView>(++serial), target_view = handle<VkImageView>(++serial);
        attachments.remember_view(device,source_view,source_view_info); attachments.remember_view(device,target_view,target_view_info);
        VkAttachmentDescription descriptions[2]{};
        for (auto& a : descriptions) {
            a.format = format; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        descriptions[0].samples = samples; descriptions[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; descriptions[0].storeOp = store;
        descriptions[0].initialLayout = descriptions[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, resolve{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color; subpass.pResolveAttachments = &resolve;
        const uint32_t mask = 5;
        VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO}; mv.subpassCount = 1; mv.pViewMasks = &mask;
        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ci.pNext = &mv;
        ci.attachmentCount = 2; ci.pAttachments = descriptions; ci.subpassCount = 1; ci.pSubpasses = &subpass;
        VkRenderPass resolve_pass{}; VkResult status{};
        check(MultiviewRenderPasses::get().create_fallback(device,&ci,nullptr,&resolve_pass,true,status) && status == VK_SUCCESS,
            "create independently owned multiview resolve pass");
        const VkImageView bound[]{source_view,target_view}; auto fb_info = fci;
        fb_info.renderPass = resolve_pass; fb_info.attachmentCount = 2; fb_info.pAttachments = bound;
        const auto resolve_fb = handle<VkFramebuffer>(++serial); attachments.remember_framebuffer(device,resolve_fb,fb_info);
        auto begin = rp; begin.renderPass = resolve_pass; begin.framebuffer = resolve_fb; begin.renderArea = {{3,5},{7,9}};
        repaired_regions.clear(); repair_barriers.clear();
        commands.begin(device,primary,command_pool,cbi,true,{begin_pass,end_pass,repair_barrier,measured ? repair_resolve : nullptr});
        commands.begin_pass(primary,&begin,VK_SUBPASS_CONTENTS_INLINE); commands.end_pass(primary);
        check(commands.error(primary) == VK_SUCCESS && repaired_regions.size() == expected && repair_barriers.size() == expected * 2,
            "resolve repair respects measured support, image transfer usages, sample format and source preservation");
        for (uint32_t i = 0; i < repaired_regions.size(); ++i) {
            const auto& region = repaired_regions[i]; const auto& before = repair_barriers[i*2]; const auto& after = repair_barriers[i*2+1];
            check(region.srcSubresource.mipLevel == 0 && region.dstSubresource.mipLevel == 1 &&
                region.srcSubresource.baseArrayLayer == 2 + 2*i && region.dstSubresource.baseArrayLayer == 4 + 2*i &&
                region.srcSubresource.layerCount == 1 && region.dstSubresource.layerCount == 1 &&
                region.srcOffset.x == 3 && region.srcOffset.y == 5 && region.dstOffset.x == 3 && region.dstOffset.y == 5 &&
                region.extent.width == 7 && region.extent.height == 9 && region.extent.depth == 1 &&
                before[0].oldLayout == VK_IMAGE_LAYOUT_GENERAL && before[1].oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                after[0].newLayout == before[0].oldLayout && after[1].newLayout == before[1].oldLayout &&
                before[0].subresourceRange.baseArrayLayer == region.srcSubresource.baseArrayLayer &&
                before[1].subresourceRange.baseMipLevel == 1 && before[1].subresourceRange.baseArrayLayer == region.dstSubresource.baseArrayLayer,
                "resolve repair preserves nonzero render areas, distinct source/target mip-layer offsets and final layouts");
        }
        commands.forget(primary); attachments.forget_framebuffer(device,resolve_fb);
        attachments.forget_view(device,source_view); attachments.forget_view(device,target_view);
        attachments.forget_image(device,repair_source); attachments.forget_image(device,repair_target);
        MultiviewRenderPasses::get().forget(device,resolve_pass); destroy_render_pass(device,resolve_pass,nullptr);
    };
    const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    const auto source_usage = color_usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, target_usage = color_usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    run_resolve_case(source_usage,target_usage,VK_SAMPLE_COUNT_4_BIT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_ATTACHMENT_STORE_OP_STORE,true,2);
    run_resolve_case(color_usage,target_usage,VK_SAMPLE_COUNT_4_BIT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_ATTACHMENT_STORE_OP_STORE,true,0);
    run_resolve_case(source_usage,color_usage,VK_SAMPLE_COUNT_4_BIT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_ATTACHMENT_STORE_OP_STORE,true,0);
    run_resolve_case(source_usage,target_usage,VK_SAMPLE_COUNT_4_BIT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_ATTACHMENT_STORE_OP_DONT_CARE,true,0);
    run_resolve_case(source_usage,target_usage,VK_SAMPLE_COUNT_2_BIT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_ATTACHMENT_STORE_OP_STORE,true,0);
    run_resolve_case(source_usage,target_usage,VK_SAMPLE_COUNT_4_BIT,VK_FORMAT_R8G8B8A8_UNORM,VK_ATTACHMENT_STORE_OP_STORE,true,0);
    run_resolve_case(source_usage,target_usage,VK_SAMPLE_COUNT_4_BIT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_ATTACHMENT_STORE_OP_STORE,false,0);
    commands.forget_pool(device, command_pool);
    check(commands.error(primary) == VK_SUCCESS, "command pool retirement removes captured streams and their errors");
    commands.forget_device(device);
    callback_source = handle<VkPipelineLayout>(0x9901);
    check(MultiviewPipelineLayouts::get().remember(device,callback_source,plci,create_pipeline_layout,reentrant_layout_destroy) == VK_SUCCESS,
        "create callback-aware layout for final-reference retirement");
    MultiviewPipelineLayouts::get().forget(device,callback_source);
    check(layout_callbacks == 1, "final map-reference destruction invokes callback outside the registry mutex");
    check(MultiviewPipelineLayouts::get().remember(device,callback_source,plci,create_pipeline_layout,reentrant_layout_destroy) == VK_SUCCESS,
        "create callback-aware layout for device retirement");
    MultiviewPipelineLayouts::get().forget_device(device);
    check(layout_callbacks == 2, "device retirement detaches handles before callbacks and destroys each once");
    registry.forget_pool(device, pool);
    check(arena.select(device, b, 5, {{1, 0}}, shadow) == VK_ERROR_FEATURE_NOT_PRESENT, "pool reset retires captured source set records");
    registry.forget_device(device); attachments.forget_device(device);
    MultiviewRenderPasses::get().forget_device(device); destroy_render_pass(device, pass, nullptr); destroy_render_pass(device, barrier_pass, nullptr);
    destroy_render_pass(device, unused_pass, nullptr);
    destroy_render_pass(device, volume_pass, nullptr);
    destroy_render_pass(device, alias_pass, nullptr);
    check(arena.select(device, a, 0, {}, shadow) == VK_ERROR_FEATURE_NOT_PRESENT, "device teardown retires live arenas before native device destruction");
    check(other_arena.select(other, other_set, 4, {{1, 0}}, shadow) == VK_SUCCESS && shadow == other_shadow,
        "destroying one device preserves the other device's live arena");
    registry.forget_device(other); attachments.forget_device(other);
    destroy_layout(other, layout, nullptr); destroy_pool(other, pool, nullptr);
    registry.forget_layout(device, layout); destroy_layout(device, layout, nullptr); destroy_pool(device, pool, nullptr);
    check(views.empty() && pools.empty() && framebuffers.empty() && render_passes.empty() && pipeline_layouts.empty() && !invalid_layout_destroys,
        "all private pools, slices, framebuffers, passes and layouts are reclaimed");
    printf("Multiview descriptor failures: %u\n", failures); return failures ? 1 : 0;
}

#include "vertex_store_emulator.h"
#include "driver_loader.h"
#include "layer_manager.h"
#include "shader/vertex_store_lowering.h"
#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace {
thread_local bool internal_call = false;
struct Internal {
    bool previous = internal_call;
    Internal() {
        internal_call = true;
    }
    ~Internal() {
        internal_call = previous;
    }
};
template <class T> T fn(VkDevice d, const char *name) {
    return reinterpret_cast<T>(get_real_proc(get_last_instance(), d, name));
}
#define REAL(name) fn<PFN_vk##name>(device, "vk" #name)
uint64_t align4(uint64_t n) {
    return (n + 3) & ~uint64_t(3);
}
struct SetInfo {
    VkDevice device{};
    std::shared_ptr<VertexStoreEmulator::MemoryStats> stats;
    VkDescriptorSetLayout clone{};
    uint32_t dynamic_count = 0;
    bool supported = true;
    ~SetInfo() {
        if (clone) {
            fn<PFN_vkDestroyDescriptorSetLayout>(device, "vkDestroyDescriptorSetLayout")(
                device, clone, nullptr);
            --stats->cloned_set_layouts;
        }
    }
};
struct Layout {
    VkDevice device{};
    std::shared_ptr<VertexStoreEmulator::MemoryStats> stats;
    std::vector<std::shared_ptr<SetInfo>> sets;
    std::vector<VkPushConstantRange> pushes;
    VkPipelineLayout clone{};
    ~Layout() {
        if (clone) {
            fn<PFN_vkDestroyPipelineLayout>(device, "vkDestroyPipelineLayout")(device, clone,
                                                                               nullptr);
            --stats->cloned_pipeline_layouts;
        }
    }
};
struct Pipeline {
    VkPipeline compute{};
    VkPipelineLayout layout{};
    std::shared_ptr<Layout> original;
    uint32_t output_words = 0, push_size = 0;
    std::vector<vkfix::VertexStoreAttribute> attributes;
    std::map<uint32_t, uint32_t> strides;
    std::vector<uint32_t> resource_sets;
};
struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    VkDeviceSize size = 0;
    VkDeviceSize allocation_size = 0;
    std::shared_ptr<VertexStoreEmulator::MemoryStats> stats;
};
struct DrawResources {
    Buffer input, output;
    VkDescriptorPool pool{};
};
struct BoundSet {
    VkDescriptorSet set{};
    std::shared_ptr<Layout> layout;
    std::vector<uint32_t> offsets;
    uint32_t index = 0;
};
struct Push {
    std::shared_ptr<Layout> layout;
    uint32_t offset;
    std::vector<uint8_t> bytes;
};
struct Rendering {
    VkRenderingInfo info{};
    std::vector<VkRenderingAttachmentInfo> colors;
    VkRenderingAttachmentInfo depth{}, stencil{};
    bool has_depth = false, has_stencil = false, supported = true;
    void pointers() {
        info.pColorAttachments = colors.empty() ? nullptr : colors.data();
        info.pDepthAttachment = has_depth ? &depth : nullptr;
        info.pStencilAttachment = has_stencil ? &stencil : nullptr;
    }
};
struct Command {
    VkDevice device{};
    VkCommandPool pool{};
    bool primary = true, compute_queue = true;
    VkPipeline graphics{}, compute{};
    std::array<VkBuffer, vkfix::kVertexStoreBindings> vertices{};
    std::array<VkDeviceSize, vkfix::kVertexStoreBindings> offsets{};
    std::map<uint32_t, uint32_t> dynamic_strides;
    VkBuffer index{};
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    std::map<uint32_t, BoundSet> graphics_sets, compute_sets;
    std::vector<Push> compute_push;
    std::vector<uint8_t> vertex_push;
    std::vector<DrawResources> resources;
    Rendering rendering;
    bool active = false, legacy = false;
    uint32_t queries = 0;
    VkResult error = VK_SUCCESS;
};
struct Device {
    std::shared_ptr<VertexStoreEmulator::MemoryStats> stats =
        std::make_shared<VertexStoreEmulator::MemoryStats>();
    uint64_t presents = 0;
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout private_set{};
    std::map<VkShaderModule, std::vector<uint32_t>> shaders;
    std::map<VkBuffer, VkDeviceSize> buffers;
    std::map<VkDescriptorSetLayout, std::shared_ptr<SetInfo>> sets;
    std::map<VkPipelineLayout, std::shared_ptr<Layout>> layouts;
    std::map<VkPipeline, Pipeline> pipelines;
    std::map<VkImage, bool> transient_images;
    std::map<VkImageView, bool> transient_views;
    std::map<VkCommandPool, bool> pools;
};
void release(VkDevice device, DrawResources &r) {
    if (r.pool)
        REAL(DestroyDescriptorPool)(device, r.pool, nullptr);
    for (auto b : {r.input, r.output}) {
        if (b.handle)
            REAL(DestroyBuffer)(device, b.handle, nullptr);
        if (b.memory) {
            REAL(FreeMemory)(device, b.memory, nullptr);
            b.stats->scratch_bytes -= b.allocation_size;
        }
    }
    r = {};
}
bool allocate(VkDevice device, const Device &d, VkDeviceSize size, Buffer &b) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (REAL(CreateBuffer)(device, &ci, nullptr, &b.handle) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    REAL(GetBufferMemoryRequirements)(device, b.handle, &req);
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < d.memory.memoryTypeCount; ++i)
        if (req.memoryTypeBits & (1u << i)) {
            if (index == UINT32_MAX)
                index = i;
            if (d.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                index = i;
                break;
            }
        }
    if (index == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = index;
    if (REAL(AllocateMemory)(device, &ai, nullptr, &b.memory) != VK_SUCCESS)
        return false;
    b.allocation_size = req.size;
    b.stats = d.stats;
    b.stats->scratch_bytes += req.size;
    b.stats->peak_scratch_bytes = std::max(b.stats->peak_scratch_bytes, b.stats->scratch_bytes);
    b.size = size;
    return REAL(BindBufferMemory)(device, b.handle, b.memory, 0) == VK_SUCCESS;
}
void barrier(VkDevice device, VkCommandBuffer cmd, VkPipelineStageFlags src,
             VkPipelineStageFlags dst, VkAccessFlags sa, VkAccessFlags da) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    REAL(CmdPipelineBarrier)(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}
} // namespace
struct VertexStoreEmulator::Impl {
    std::atomic<uint32_t> active{0};
    std::recursive_mutex mutex;
    std::map<VkDevice, Device> devices;
    std::map<VkCommandBuffer, Command> commands;
    Command *command(VkCommandBuffer cmd) {
        if (internal_call)
            return nullptr;
        auto it = commands.find(cmd);
        return it == commands.end() ? nullptr : &it->second;
    }
    void fail(Command &c, const char *message) {
        if (c.error == VK_SUCCESS)
            LOGE("vertex store emulation: %s", message);
        c.error = VK_ERROR_FEATURE_NOT_PRESENT;
    }
};
VertexStoreEmulator::VertexStoreEmulator() : impl(std::make_unique<Impl>()) {}
VertexStoreEmulator::~VertexStoreEmulator() = default;
VertexStoreEmulator &VertexStoreEmulator::get() {
    static VertexStoreEmulator instance;
    return instance;
}
VertexStoreEmulator::MemoryStats VertexStoreEmulator::memory_stats(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto d = impl->devices.find(device);
    if (d == impl->devices.end())
        return {};
    auto result = *d->second.stats;
    for (auto &c : impl->commands)
        if (c.second.device == device) {
            result.live_draws += c.second.resources.size();
            ++result.command_buffers;
        }
    return result;
}
void VertexStoreEmulator::on_present(VkDevice device) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto d = impl->devices.find(device);
    if (d == impl->devices.end() || ++d->second.presents % 300)
        return;
    auto stats = memory_stats(device);
    LOGI("vertex stores: frames=%llu draws=%llu liveDraws=%llu scratch=%llu peak=%llu "
         "copiedInput=%llu clonedSetLayouts=%llu clonedPipelineLayouts=%llu",
         (unsigned long long)d->second.presents, (unsigned long long)stats.total_draws,
         (unsigned long long)stats.live_draws, (unsigned long long)stats.scratch_bytes,
         (unsigned long long)stats.peak_scratch_bytes, (unsigned long long)stats.copied_input_bytes,
         (unsigned long long)stats.cloned_set_layouts,
         (unsigned long long)stats.cloned_pipeline_layouts);
}
bool VertexStoreEmulator::enabled(VkDevice device) {
    if (internal_call || !impl->active.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    return impl->devices.count(device) != 0;
}
void VertexStoreEmulator::enable(VkDevice device, VkPhysicalDevice physical) {
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto &d = impl->devices[device];
    d.physical = physical;
    fn<PFN_vkGetPhysicalDeviceProperties>(VK_NULL_HANDLE,
                                          "vkGetPhysicalDeviceProperties")(physical, &d.properties);
    fn<PFN_vkGetPhysicalDeviceMemoryProperties>(
        VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties")(physical, &d.memory);
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 2;
    ci.pBindings = bindings;
    if (REAL(CreateDescriptorSetLayout)(device, &ci, nullptr, &d.private_set) != VK_SUCCESS)
        d.private_set = VK_NULL_HANDLE;
    impl->active.store(uint32_t(impl->devices.size()), std::memory_order_release);
    LOGI("vertex stores: compute/replay enabled; direct draws with dynamic rendering only");
}
void VertexStoreEmulator::destroy_device(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto it = impl->devices.find(device);
    if (it == impl->devices.end())
        return;
    for (auto c = impl->commands.begin(); c != impl->commands.end();) {
        if (c->second.device != device) {
            ++c;
            continue;
        }
        for (auto &r : c->second.resources)
            release(device, r);
        c = impl->commands.erase(c);
    }
    auto &d = it->second;
    for (auto &p : d.pipelines) {
        REAL(DestroyPipeline)(device, p.second.compute, nullptr);
        REAL(DestroyPipelineLayout)(device, p.second.layout, nullptr);
    }
    if (d.private_set)
        REAL(DestroyDescriptorSetLayout)(device, d.private_set, nullptr);
    impl->devices.erase(it);
    impl->active.store(uint32_t(impl->devices.size()), std::memory_order_release);
}
void VertexStoreEmulator::shader(VkDevice device, VkShaderModule module,
                                 const VkShaderModuleCreateInfo *ci) {
    if (!impl->active.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (!enabled(device) || !ci || !ci->pCode || ci->codeSize % 4)
        return;
    impl->devices.at(device).shaders[module] = {ci->pCode, ci->pCode + ci->codeSize / 4};
}
void VertexStoreEmulator::erase_shader(VkDevice device, VkShaderModule module) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (enabled(device))
        impl->devices.at(device).shaders.erase(module);
}
VkBufferCreateInfo VertexStoreEmulator::buffer_info(VkDevice device, const VkBufferCreateInfo &ci) {
    if (!impl->active.load(std::memory_order_acquire))
        return ci;

    auto out = ci;
    if (enabled(device) && !ci.flags && !ci.pNext &&
        (ci.usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT))) {
        out.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    return out;
}
void VertexStoreEmulator::buffer(VkDevice device, VkBuffer b, const VkBufferCreateInfo *ci) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (enabled(device) && ci && !ci->flags && !ci->pNext)
        impl->devices.at(device).buffers[b] = ci->size;
}
void VertexStoreEmulator::erase_buffer(VkDevice device, VkBuffer b) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (enabled(device))
        impl->devices.at(device).buffers.erase(b);
}

void VertexStoreEmulator::set_layout(VkDevice device, VkDescriptorSetLayout set,
                                     const VkDescriptorSetLayoutCreateInfo *ci) {
    if (!impl->active.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (!enabled(device) || !ci)
        return;
    auto s = std::make_shared<SetInfo>();
    auto &d = impl->devices.at(device);
    s->device = device;
    s->stats = d.stats;
    s->supported = !ci->pNext && !ci->flags;
    for (uint32_t i = 0; i < ci->bindingCount; ++i)
        if (ci->pBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            ci->pBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
            s->dynamic_count += ci->pBindings[i].descriptorCount;
    if (s->supported &&
        REAL(CreateDescriptorSetLayout)(device, ci, nullptr, &s->clone) != VK_SUCCESS)
        s->supported = false;
    if (s->clone)
        ++d.stats->cloned_set_layouts;
    d.sets[set] = s;
}
void VertexStoreEmulator::erase_set_layout(VkDevice device, VkDescriptorSetLayout set) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (enabled(device))
        impl->devices.at(device).sets.erase(set);
}
void VertexStoreEmulator::layout(VkDevice device, VkPipelineLayout handle,
                                 const VkPipelineLayoutCreateInfo *ci) {
    if (!impl->active.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (!enabled(device) || !ci)
        return;
    auto &d = impl->devices.at(device);
    auto l = std::make_shared<Layout>();
    l->device = device;
    l->stats = d.stats;
    std::vector<VkDescriptorSetLayout> sets;
    bool valid = !ci->pNext && !ci->flags;
    for (uint32_t i = 0; i < ci->setLayoutCount; ++i) {
        auto it = d.sets.find(ci->pSetLayouts[i]);
        if (it == d.sets.end()) {
            valid = false;
            break;
        }
        l->sets.push_back(it->second);
        sets.push_back(it->second->clone);
        valid &= it->second->supported;
    }
    if (ci->pushConstantRangeCount)
        l->pushes.assign(ci->pPushConstantRanges,
                         ci->pPushConstantRanges + ci->pushConstantRangeCount);
    auto copy = *ci;
    copy.pSetLayouts = sets.data();
    if (valid)
        REAL(CreatePipelineLayout)(device, &copy, nullptr, &l->clone);
    if (l->clone)
        ++d.stats->cloned_pipeline_layouts;
    d.layouts[handle] = l;
}
void VertexStoreEmulator::erase_layout(VkDevice device, VkPipelineLayout handle) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (enabled(device))
        impl->devices.at(device).layouts.erase(handle);
}
void VertexStoreEmulator::image(VkDevice device, VkImage handle, const VkImageCreateInfo *ci) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (enabled(device) && ci)
        impl->devices.at(device).transient_images[handle] =
            (ci->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0;
}
void VertexStoreEmulator::view(VkDevice device, VkImageView handle,
                               const VkImageViewCreateInfo *ci) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (enabled(device) && ci)
        impl->devices.at(device).transient_views[handle] =
            impl->devices.at(device).transient_images[ci->image];
}
void VertexStoreEmulator::erase_image(VkDevice device, VkImage handle) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (enabled(device))
        impl->devices.at(device).transient_images.erase(handle);
}
void VertexStoreEmulator::erase_view(VkDevice device, VkImageView handle) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (enabled(device))
        impl->devices.at(device).transient_views.erase(handle);
}

bool VertexStoreEmulator::create_pipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                           const VkGraphicsPipelineCreateInfo *infos,
                                           const VkAllocationCallbacks *allocator, VkPipeline *out,
                                           VkResult &result) {
    if (!impl->active.load(std::memory_order_acquire))
        return false;

    if (!enabled(device) || !infos || !count)
        return false;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto &d = impl->devices.at(device);
    std::vector<VkGraphicsPipelineCreateInfo> copies(infos, infos + count);
    std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages(count);
    std::vector<Pipeline> prepared(count);
    std::vector<VkShaderModule> temporary;
    VkPipelineVertexInputStateCreateInfo no_inputs{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    bool any = false;
    auto fail = [&](const char *why) {
        LOGE("vertex store pipeline rejected: %s", why);
        result = VK_ERROR_FEATURE_NOT_PRESENT;
    };
    result = VK_SUCCESS;
    for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i) {
        const auto &ci = infos[i];
        const VkPipelineShaderStageCreateInfo *vertex = nullptr;
        bool effect = false;
        for (uint32_t s = 0; s < ci.stageCount; ++s) {
            const auto &stage = ci.pStages[s];
            if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT)
                vertex = &stage;
            if (!(stage.stage &
                  (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT)))
                continue;
            auto code = d.shaders.find(stage.module);
            if (code == d.shaders.end()) {
                fail("shader code unavailable (inline modules/libraries are unsupported)");
                break;
            }
            if (vkfix::vertex_has_storage_effects(code->second))
                effect = true;
        }
        if (result != VK_SUCCESS || !effect)
            continue;
        any = true;
        if (!vertex || vertex->pNext || ci.renderPass || !ci.pInputAssemblyState ||
            ci.pInputAssemblyState->primitiveRestartEnable || ci.pTessellationState ||
            ci.flags & (VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)) {
            fail("requires dynamic rendering, no tessellation/geometry, no restart or derivatives");
            break;
        }
        for (uint32_t s = 0; s < ci.stageCount; ++s)
            if (ci.pStages[s].stage != VK_SHADER_STAGE_VERTEX_BIT &&
                ci.pStages[s].stage != VK_SHADER_STAGE_FRAGMENT_BIT)
                fail("unsupported graphics stage");
        if (result != VK_SUCCESS)
            break;
        if (ci.pDynamicState)
            for (uint32_t s = 0; s < ci.pDynamicState->dynamicStateCount; ++s) {
                const auto state = ci.pDynamicState->pDynamicStates[s];
                if (state == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT ||
                    state == VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE ||
                    state == VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY)
                    fail("unsupported dynamic vertex input/topology");
            }
        auto layout_it = d.layouts.find(ci.layout);
        if (layout_it == d.layouts.end() || !layout_it->second->clone || !d.private_set ||
            layout_it->second->sets.size() + 1 > d.properties.limits.maxBoundDescriptorSets ||
            d.properties.limits.maxPerStageDescriptorStorageBuffers < 2) {
            fail("pipeline layout or vertex storage limits cannot support replay");
            break;
        }
        auto &p = prepared[i];
        p.original = layout_it->second;
        if (ci.pVertexInputState) {
            if (ci.pVertexInputState->pNext) {
                fail("vertex input extensions are unsupported");
                break;
            }
            const auto &vi = *ci.pVertexInputState;
            for (uint32_t b = 0; b < vi.vertexBindingDescriptionCount; ++b) {
                const auto &binding = vi.pVertexBindingDescriptions[b];
                p.strides[binding.binding] = binding.stride;
            }
            for (uint32_t a = 0; a < vi.vertexAttributeDescriptionCount; ++a) {
                const auto &attr = vi.pVertexAttributeDescriptions[a];
                auto binding =
                    std::find_if(vi.pVertexBindingDescriptions,
                                 vi.pVertexBindingDescriptions + vi.vertexBindingDescriptionCount,
                                 [&](const auto &b) { return b.binding == attr.binding; });
                if (binding == vi.pVertexBindingDescriptions + vi.vertexBindingDescriptionCount ||
                    attr.binding >= vkfix::kVertexStoreBindings) {
                    fail("missing vertex binding");
                    break;
                }
                p.attributes.push_back(
                    {attr.location, attr.binding, attr.offset, attr.format, binding->inputRate});
            }
        }
        if (result != VK_SUCCESS)
            break;
        vkfix::VertexStoreProgram program;
        if (!vkfix::lower_vertex_stores(d.shaders.at(vertex->module), vertex->pName,
                                        uint32_t(p.original->sets.size()), p.attributes, program)) {
            fail(program.error.c_str());
            break;
        }
        p.output_words = program.output_words;
        p.resource_sets = program.resource_sets;
        std::vector<VkDescriptorSetLayout> sets;
        for (auto &set : p.original->sets)
            sets.push_back(set->clone);
        sets.push_back(d.private_set);
        std::vector<VkPushConstantRange> pushes = p.original->pushes;
        for (auto &range : pushes) {
            p.push_size = std::max(p.push_size, range.offset + range.size);
            range.stageFlags &= ~VK_SHADER_STAGE_COMPUTE_BIT;
        }
        pushes.erase(std::remove_if(pushes.begin(), pushes.end(),
                                    [](const auto &r) { return !r.stageFlags; }),
                     pushes.end());
        if (p.push_size)
            pushes.push_back({VK_SHADER_STAGE_COMPUTE_BIT, 0, p.push_size});
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = uint32_t(sets.size());
        li.pSetLayouts = sets.data();
        li.pushConstantRangeCount = uint32_t(pushes.size());
        li.pPushConstantRanges = pushes.data();
        if (REAL(CreatePipelineLayout)(device, &li, nullptr, &p.layout) != VK_SUCCESS) {
            fail("internal pipeline layout creation failed");
            break;
        }
        VkShaderModule modules[2]{};
        for (uint32_t s = 0; s < 2; ++s) {
            const auto &code = s ? program.replay : program.compute;
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = code.size() * 4;
            mi.pCode = code.data();
            if (REAL(CreateShaderModule)(device, &mi, nullptr, &modules[s]) != VK_SUCCESS) {
                fail("internal shader module creation failed");
                break;
            }
            temporary.push_back(modules[s]);
        }
        if (result != VK_SUCCESS)
            break;
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.layout = p.layout;
        cp.stage = *vertex;
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = modules[0];
        cp.stage.pName = "main";
        cp.stage.pNext = nullptr;
        if (REAL(CreateComputePipelines)(device, VK_NULL_HANDLE, 1, &cp, nullptr, &p.compute) !=
            VK_SUCCESS) {
            fail("compute pipeline compilation failed");
            break;
        }
        stages[i].assign(ci.pStages, ci.pStages + ci.stageCount);
        for (auto &s : stages[i])
            if (s.stage == VK_SHADER_STAGE_VERTEX_BIT) {
                s.module = modules[1];
                s.pName = "main";
                s.pSpecializationInfo = nullptr;
            }
        copies[i].pStages = stages[i].data();
        copies[i].pVertexInputState = &no_inputs;
        copies[i].layout = p.layout;
    }
    if (!any && result == VK_SUCCESS)
        return false;
    if (result == VK_SUCCESS) {
        Internal guard;
        result = LayerManager::get().dispatch_create_graphics_pipelines(
            device, cache, count, copies.data(), allocator, out);
    } else
        for (uint32_t i = 0; i < count; ++i)
            out[i] = VK_NULL_HANDLE;
    for (auto module : temporary)
        REAL(DestroyShaderModule)(device, module, nullptr);
    for (uint32_t i = 0; i < count; ++i) {
        auto &p = prepared[i];
        if (p.compute && out[i])
            d.pipelines[out[i]] = std::move(p);
        else {
            if (p.compute)
                REAL(DestroyPipeline)(device, p.compute, nullptr);
            if (p.layout)
                REAL(DestroyPipelineLayout)(device, p.layout, nullptr);
        }
    }
    return true;
}
void VertexStoreEmulator::erase_pipeline(VkDevice device, VkPipeline handle) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (!enabled(device))
        return;
    auto &map = impl->devices.at(device).pipelines;
    auto it = map.find(handle);
    if (it == map.end())
        return;
    REAL(DestroyPipeline)(device, it->second.compute, nullptr);
    REAL(DestroyPipelineLayout)(device, it->second.layout, nullptr);
    map.erase(it);
}

void VertexStoreEmulator::pool(VkDevice device, VkCommandPool pool, uint32_t family) {
    if (!impl->active.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (!enabled(device))
        return;
    auto query = fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        VK_NULL_HANDLE, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t count = 0;
    query(impl->devices.at(device).physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    query(impl->devices.at(device).physical, &count, props.data());
    impl->devices.at(device).pools[pool] =
        family < count && (props[family].queueFlags & VK_QUEUE_COMPUTE_BIT);
}
void VertexStoreEmulator::allocate_commands(VkDevice device, const VkCommandBufferAllocateInfo *ci,
                                            const VkCommandBuffer *buffers) {
    if (!impl->active.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (!enabled(device) || !ci || !buffers)
        return;
    for (uint32_t i = 0; i < ci->commandBufferCount; ++i) {
        auto &c = impl->commands[buffers[i]];
        c.device = device;
        c.pool = ci->commandPool;
        c.primary = ci->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        c.compute_queue = impl->devices.at(device).pools[ci->commandPool];
    }
}
void VertexStoreEmulator::clear_commands(VkCommandBuffer cmd) {
    if (!impl->active.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (!c)
        return;
    for (auto &r : c->resources)
        release(c->device, r);
    auto device = c->device;
    auto pool = c->pool;
    auto primary = c->primary;
    auto compute = c->compute_queue;
    *c = {};
    c->device = device;
    c->pool = pool;
    c->primary = primary;
    c->compute_queue = compute;
}
void VertexStoreEmulator::free_commands(VkDevice device, uint32_t count,
                                        const VkCommandBuffer *cmds) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    for (uint32_t i = 0; cmds && i < count; ++i) {
        clear_commands(cmds[i]);
        impl->commands.erase(cmds[i]);
    }
}
void VertexStoreEmulator::clear_pool(VkDevice device, VkCommandPool pool) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    for (auto &c : impl->commands)
        if (c.second.device == device && c.second.pool == pool)
            clear_commands(c.first);
}
void VertexStoreEmulator::destroy_pool(VkDevice device, VkCommandPool pool) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    for (auto c = impl->commands.begin(); c != impl->commands.end();) {
        if (c->second.device == device && c->second.pool == pool) {
            clear_commands(c->first);
            c = impl->commands.erase(c);
        } else
            ++c;
    }
    auto d = impl->devices.find(device);
    if (d != impl->devices.end())
        d->second.pools.erase(pool);
}
void VertexStoreEmulator::bind_pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point,
                                        VkPipeline pipeline) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (c)
        (point == VK_PIPELINE_BIND_POINT_GRAPHICS ? c->graphics : c->compute) = pipeline;
}
void VertexStoreEmulator::bind_vertices(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                        const VkBuffer *buffers, const VkDeviceSize *offsets,
                                        const VkDeviceSize *strides) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (!c)
        return;
    for (uint32_t i = 0; buffers && offsets && i < count && first + i < vkfix::kVertexStoreBindings;
         ++i) {
        c->vertices[first + i] = buffers[i];
        c->offsets[first + i] = offsets[i];
        if (strides)
            c->dynamic_strides[first + i] = uint32_t(strides[i]);
    }
}
void VertexStoreEmulator::bind_index(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                     VkIndexType type) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (c) {
        c->index = buffer;
        c->index_offset = offset;
        c->index_type = type;
    }
}
void VertexStoreEmulator::bind_sets(VkCommandBuffer cmd, VkPipelineBindPoint point,
                                    VkPipelineLayout layout, uint32_t first, uint32_t count,
                                    const VkDescriptorSet *sets, uint32_t dynamic_count,
                                    const uint32_t *offsets) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (!c)
        return;
    auto &d = impl->devices.at(c->device);
    auto l = d.layouts.find(layout);
    if (l == d.layouts.end())
        return;
    auto &bound = point == VK_PIPELINE_BIND_POINT_GRAPHICS ? c->graphics_sets : c->compute_sets;
    uint32_t next = 0;
    for (uint32_t i = 0; sets && i < count && first + i < l->second->sets.size(); ++i) {
        const auto n = l->second->sets[first + i]->dynamic_count;
        if (next + n > dynamic_count) {
            impl->fail(*c, "invalid dynamic descriptor offsets");
            return;
        }
        BoundSet b{sets[i], l->second, {}, first + i};
        if (n)
            b.offsets.assign(offsets + next, offsets + next + n);
        next += n;
        bound[first + i] = std::move(b);
    }
}
void VertexStoreEmulator::push(VkCommandBuffer cmd, VkPipelineLayout layout,
                               VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                               const void *data) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (!c || !data ||
        offset + uint64_t(size) >
            impl->devices.at(c->device).properties.limits.maxPushConstantsSize)
        return;
    auto &d = impl->devices.at(c->device);
    auto l = d.layouts.find(layout);
    if (l == d.layouts.end())
        return;
    const auto bytes = static_cast<const uint8_t *>(data);
    if (stages & VK_SHADER_STAGE_VERTEX_BIT) {
        c->vertex_push.resize(d.properties.limits.maxPushConstantsSize);
        memcpy(c->vertex_push.data() + offset, data, size);
    }
    if (stages & VK_SHADER_STAGE_COMPUTE_BIT)
        c->compute_push.push_back({l->second, offset, {bytes, bytes + size}});
}
const VkRenderingInfo *VertexStoreEmulator::begin_rendering(VkCommandBuffer cmd,
                                                            const VkRenderingInfo *info) {
    if (!impl->active.load(std::memory_order_acquire))
        return info;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (!c || !info)
        return info;
    c->active = true;
    auto &r = c->rendering;
    r = {};
    r.info = *info;
    r.supported = !info->pNext && !info->flags && !info->viewMask;
    auto &d = impl->devices.at(c->device);
    auto attach = [&](VkRenderingAttachmentInfo &a) {
        r.supported &=
            !a.pNext && a.resolveMode == VK_RESOLVE_MODE_NONE && !d.transient_views[a.imageView];
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    };
    if (info->colorAttachmentCount) {
        r.colors.assign(info->pColorAttachments,
                        info->pColorAttachments + info->colorAttachmentCount);
        for (auto &a : r.colors)
            attach(a);
    }
    if (info->pDepthAttachment) {
        r.has_depth = true;
        r.depth = *info->pDepthAttachment;
        attach(r.depth);
    }
    if (info->pStencilAttachment) {
        r.has_stencil = true;
        r.stencil = *info->pStencilAttachment;
        attach(r.stencil);
    }
    r.pointers();
    return r.supported ? &r.info : info;
}
void VertexStoreEmulator::end_rendering(VkCommandBuffer cmd) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (c)
        c->active = false;
}
void VertexStoreEmulator::legacy_rendering(VkCommandBuffer cmd, bool active) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (c)
        c->legacy = active;
}
void VertexStoreEmulator::query(VkCommandBuffer cmd, bool active) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (c) {
        if (active)
            ++c->queries;
        else if (c->queries)
            --c->queries;
    }
}
VkResult VertexStoreEmulator::recording_error(VkCommandBuffer cmd) {
    if (!impl->active.load(std::memory_order_acquire))
        return VK_SUCCESS;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    return c ? c->error : VK_SUCCESS;
}
void VertexStoreEmulator::indirect(VkCommandBuffer cmd) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (c && impl->devices.at(c->device).pipelines.count(c->graphics))
        impl->fail(*c, "indirect emulated vertex draws are unsupported");
}

bool VertexStoreEmulator::draw(VkCommandBuffer cmd, uint32_t count, uint32_t instances,
                               uint32_t first, int32_t vertex_offset, uint32_t first_instance,
                               bool indexed) {
    if (!impl->active.load(std::memory_order_acquire))
        return false;

    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    auto c = impl->command(cmd);
    if (!c)
        return false;
    const VkDevice device = c->device;
    auto &d = impl->devices.at(device);
    auto pipeline = d.pipelines.find(c->graphics);
    if (pipeline == d.pipelines.end())
        return false;
    if (!count || !instances)
        return true;
    auto &p = pipeline->second;
    auto fail = [&](const char *why) {
        impl->fail(*c, why);
        return true;
    };
    if (c->error != VK_SUCCESS)
        return true;
    if (!c->primary || !c->compute_queue || c->legacy || !c->active || !c->rendering.supported ||
        c->queries)
        return fail("requires primary compute-capable command buffer, splittable dynamic rendering "
                    "and no queries");
    if (indexed && c->index_type != VK_INDEX_TYPE_UINT16 && c->index_type != VK_INDEX_TYPE_UINT32)
        return fail("unsupported index type");
    const uint64_t bytes_per_vertex = uint64_t(instances) * p.output_words * 4;
    if (!bytes_per_vertex || count > d.properties.limits.maxStorageBufferRange / bytes_per_vertex ||
        count > uint64_t(d.properties.limits.maxComputeWorkGroupCount[0]) * 64 ||
        instances > d.properties.limits.maxComputeWorkGroupCount[1])
        return fail("draw exceeds compute/replay limits");
    const uint64_t output_size = uint64_t(count) * bytes_per_vertex;
    for (auto i : p.resource_sets) {
        if (i >= p.original->sets.size())
            return fail("shader descriptor set exceeds layout");
        auto b = c->graphics_sets.find(i);
        if (b == c->graphics_sets.end() || b->second.index >= b->second.layout->sets.size() ||
            b->second.layout->sets[b->second.index] != p.original->sets[i])
            return fail("missing or incompatible graphics descriptors");
    }
    std::array<uint32_t, vkfix::kVertexStoreHeaderWords> header{};
    header[0] = count;
    header[1] = instances;
    header[2] = indexed ? uint32_t(vertex_offset) : first;
    header[3] = first_instance;
    header[4] = indexed ? (c->index_type == VK_INDEX_TYPE_UINT16 ? 16 : 32) : 0;
    header[5] = indexed ? first : 0;
    struct Copy {
        VkBuffer source;
        VkDeviceSize size, destination;
    };
    std::vector<Copy> copies;
    uint64_t input_size = sizeof(header);
    auto append = [&](VkBuffer buffer, VkDeviceSize offset, uint32_t &destination) {
        auto b = d.buffers.find(buffer);
        if (b == d.buffers.end() || offset >= b->second || offset > UINT32_MAX ||
            input_size > UINT32_MAX || b->second > UINT32_MAX - input_size)
            return false;
        destination = uint32_t(input_size + offset);
        copies.push_back({buffer, b->second, input_size});
        input_size += align4(b->second);
        return true;
    };
    std::set<uint32_t> bindings;
    for (auto &attr : p.attributes)
        bindings.insert(attr.binding);
    for (auto binding : bindings) {
        if (!append(c->vertices[binding], c->offsets[binding], header[8 + 2 * binding]))
            return fail("vertex buffer is unavailable or too large");
        header[9 + 2 * binding] = c->dynamic_strides.count(binding) ? c->dynamic_strides.at(binding)
                                                                    : p.strides.at(binding);
    }
    if (indexed && !append(c->index, c->index_offset, header[6]))
        return fail("index buffer is unavailable or too large");
    if (input_size > d.properties.limits.maxStorageBufferRange)
        return fail("input buffers exceed storage buffer range");
    DrawResources resources;
    if (!allocate(device, d, input_size, resources.input) ||
        !allocate(device, d, output_size, resources.output)) {
        release(device, resources);
        return fail("scratch buffer allocation failed");
    }
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &pool_size;
    if (REAL(CreateDescriptorPool)(device, &pi, nullptr, &resources.pool) != VK_SUCCESS) {
        release(device, resources);
        return fail("descriptor pool allocation failed");
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = resources.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &d.private_set;
    VkDescriptorSet set{};
    if (REAL(AllocateDescriptorSets)(device, &ai, &set) != VK_SUCCESS) {
        release(device, resources);
        return fail("descriptor allocation failed");
    }
    VkDescriptorBufferInfo buffers[2]{{resources.output.handle, 0, output_size},
                                      {resources.input.handle, 0, input_size}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffers[i];
    }
    REAL(UpdateDescriptorSets)(device, 2, writes, 0, nullptr);
    Internal guard;
    LayerManager::get().dispatch_cmd_end_rendering(cmd);
    barrier(device, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    REAL(CmdUpdateBuffer)(cmd, resources.input.handle, 0, sizeof(header), header.data());
    for (auto copy : copies) {
        VkBufferCopy region{0, copy.destination, copy.size};
        REAL(CmdCopyBuffer)(cmd, copy.source, resources.input.handle, 1, &region);
    }
    barrier(device, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    REAL(CmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.compute);
    for (auto index : p.resource_sets) {
        const auto &b = c->graphics_sets.at(index);
        REAL(CmdBindDescriptorSets)
        (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, index, 1, &b.set,
         uint32_t(b.offsets.size()), b.offsets.data());
    }
    const auto private_index = uint32_t(p.original->sets.size());
    REAL(CmdBindDescriptorSets)
    (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, private_index, 1, &set, 0, nullptr);
    if (p.push_size) {
        auto bytes = c->vertex_push;
        bytes.resize(p.push_size);
        REAL(CmdPushConstants)
        (cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, p.push_size, bytes.data());
    }
    REAL(CmdDispatch)(cmd, (count + 63) / 64, instances, 1);
    barrier(device, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (c->compute)
        REAL(CmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->compute);
    for (auto &item : c->compute_sets) {
        auto &b = item.second;
        if (b.layout->clone)
            REAL(CmdBindDescriptorSets)
        (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.layout->clone, b.index, 1, &b.set,
         uint32_t(b.offsets.size()), b.offsets.data());
    }
    for (auto &push : c->compute_push)
        if (push.layout->clone)
            REAL(CmdPushConstants)
    (cmd, push.layout->clone, VK_SHADER_STAGE_COMPUTE_BIT, push.offset, uint32_t(push.bytes.size()),
     push.bytes.data());
    auto resume = c->rendering;
    for (auto &a : resume.colors)
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    if (resume.has_depth)
        resume.depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    if (resume.has_stencil)
        resume.stencil.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    resume.pointers();
    LayerManager::get().dispatch_cmd_begin_rendering(cmd, &resume.info);
    REAL(CmdBindDescriptorSets)
    (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, private_index, 1, &set, 0, nullptr);
    REAL(CmdDraw)(cmd, count, instances, 0, 0);
    for (auto &item : c->graphics_sets) {
        auto &b = item.second;
        if (b.layout->clone)
            REAL(CmdBindDescriptorSets)
        (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b.layout->clone, b.index, 1, &b.set,
         uint32_t(b.offsets.size()), b.offsets.data());
    }
    c->resources.push_back(resources);
    ++d.stats->total_draws;
    d.stats->copied_input_bytes += input_size - sizeof(header);
    return true;
}

void VertexStoreEmulator::unsupported(VkCommandBuffer cmd, const char *reason) {
    if (!impl->active.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lock(impl->mutex);
    if (auto c = impl->command(cmd))
        impl->fail(*c, reason);
}
extern "C" VK_LAYER_EXPORT VkBool32
vulkanfixGetVertexStoreMemoryStats(VkDevice device, VertexStoreEmulator::MemoryStats *out) {
    if (!out)
        return VK_FALSE;
    auto &emulator = VertexStoreEmulator::get();
    *out = emulator.memory_stats(device);
    return emulator.enabled(device) ? VK_TRUE : VK_FALSE;
}

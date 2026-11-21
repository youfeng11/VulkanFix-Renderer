#include "multiview_pipelines.h"
#include "specialized_shader_stages.h"
#include "shader/spirv_extensions.h"
#include <algorithm>
#include <new>
#include <stdexcept>

namespace vkfix {
MultiviewPipelineVariants::MultiviewPipelineVariants(VkDevice device,
    std::shared_ptr<MultiviewPassResources> pass, uint32_t subpass,
    PFN_vkDestroyPipeline destroy, const VkAllocationCallbacks* allocator)
    : device_(device), pass_(std::move(pass)), subpass_(subpass),
      view_mask_(pass_->plan()->replay_mask(subpass)), destroy_(destroy) {
    if (allocator) allocator_ = *allocator;
}
MultiviewPipelineVariants::~MultiviewPipelineVariants() { release(); }
VkPipeline MultiviewPipelineVariants::for_view(uint32_t view) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ && view < pipelines_.size() ? pipelines_[view] : VK_NULL_HANDLE;
}
void MultiviewPipelineVariants::release() {
    std::array<VkPipeline, 32> retired;
    VkDevice device;
    bool published;
    uint32_t representative_view;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!device_) return;
        device = device_; device_ = VK_NULL_HANDLE;
        retired = pipelines_; pipelines_.fill(VK_NULL_HANDLE);
        published = published_; representative_view = representative_view_;
    }
    // Layer callbacks can enter the registry again for a private pipeline.
    // Neither the record nor registry mutex may be held while calling them.
    for (uint32_t view = 0; view < retired.size(); ++view)
        if (retired[view] && (!published || view != representative_view))
            destroy_(device, retired[view], allocator_ ? &*allocator_ : nullptr);
}

MultiviewPipelines& MultiviewPipelines::get() { static MultiviewPipelines registry; return registry; }
void MultiviewPipelines::publish(VkDevice device, const std::shared_ptr<MultiviewPipelineVariants>& variants) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& live = live_[device];
    live.erase(std::remove_if(live.begin(), live.end(), [](const auto& ref) { return ref.expired(); }), live.end());
    live.push_back(variants);
    const auto inserted = pipelines_.emplace(DeviceObjectKey::make(device, variants->representative_), variants);
    if (!inserted.second) throw std::runtime_error("duplicate live multiview pipeline handle");
    variants->published_ = true;
    count_.fetch_add(1, std::memory_order_release);
}

VkResult MultiviewPipelines::create_one(VkDevice device, VkPipelineCache cache,
    const VkGraphicsPipelineCreateInfo& info, const VkAllocationCallbacks* allocator,
    VkPipeline& pipeline, PFN_vkCreateGraphicsPipelines create, PFN_vkDestroyPipeline destroy,
    PFN_vkCreateShaderModule create_shader, PFN_vkDestroyShaderModule destroy_shader, uint32_t routes) {
    auto pass = MultiviewRenderPasses::get().acquire(device, info.renderPass);
    if (!pass || !pass->emulated()) return create(device, cache, 1, &info, allocator, &pipeline);
    if (!pass->plan()->core_only) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto variants = std::make_shared<MultiviewPipelineVariants>(device, pass, info.subpass, destroy, allocator);
    for (uint32_t view = 0; view < 32; ++view) {
        if (!(variants->view_mask_ & (uint32_t(1) << view))) continue;
        VkRenderPass native_pass{};
        auto status = pass->native_pass(info.subpass, view, true, true,
            (routes & NativeInputAttachmentAspects) != 0, native_pass);
        if (status != VK_SUCCESS) return status;
        auto native = info;
        native.renderPass = native_pass; native.subpass = 0;
        SpecializedShaderStages stages(device, create_shader, destroy_shader, view,
            (routes & NativeDeviceGroup) != 0);
        status = stages.prepare(1, &native);
        if (status != VK_SUCCESS) return status;
        status = create(device, cache, 1, stages.graphics(&native), allocator, &variants->pipelines_[view]);
        if (status != VK_SUCCESS) return status;
        if (!variants->pipelines_[view]) return VK_ERROR_INITIALIZATION_FAILED;
        if (!variants->representative_) {
            variants->representative_ = variants->pipelines_[view]; variants->representative_view_ = view;
        }
    }
    if (!variants->representative_) return VK_ERROR_INITIALIZATION_FAILED;
    publish(device, variants);
    pipeline = variants->representative_;
    return VK_SUCCESS;
}

bool MultiviewPipelines::create(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines, VkResult& result, PFN_vkCreateGraphicsPipelines create,
    PFN_vkDestroyPipeline destroy, PFN_vkCreateShaderModule create_shader, PFN_vkDestroyShaderModule destroy_shader, uint32_t routes) {
    if (!infos || !pipelines || !count) return false;
    bool handles = false;
    for (uint32_t i = 0; i < count; ++i)
        if (auto pass = MultiviewRenderPasses::get().acquire(device, infos[i].renderPass))
            if (pass->emulated()) { handles = true; break; }
    if (!handles) return false;
    std::fill_n(pipelines, count, VK_NULL_HANDLE);
    result = VK_SUCCESS;
    for (uint32_t i = 0; i < count; ++i) {
        VkResult status;
        try {
            auto info = infos[i];
            if ((info.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) && info.basePipelineIndex >= 0) {
                const auto base = uint32_t(info.basePipelineIndex);
                if (base >= i || !pipelines[base]) {
                    status = result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
                } else {
                    info.basePipelineHandle = pipelines[base]; info.basePipelineIndex = -1;
                    status = create_one(device, cache, info, allocator, pipelines[i], create, destroy, create_shader, destroy_shader, routes);
                }
            } else status = create_one(device, cache, info, allocator, pipelines[i], create, destroy, create_shader, destroy_shader, routes);
        } catch (const std::bad_alloc&) {
            if (pipelines[i]) { destroy(device, pipelines[i], allocator); pipelines[i] = VK_NULL_HANDLE; }
            status = VK_ERROR_OUT_OF_HOST_MEMORY;
        } catch (const std::out_of_range&) {
            if (pipelines[i]) { destroy(device, pipelines[i], allocator); pipelines[i] = VK_NULL_HANDLE; }
            status = VK_ERROR_UNKNOWN;
        } catch (const std::runtime_error&) {
            if (pipelines[i]) { destroy(device, pipelines[i], allocator); pipelines[i] = VK_NULL_HANDLE; }
            status = VK_ERROR_UNKNOWN;
        }
        if (status != VK_SUCCESS) {
            if (result == VK_SUCCESS || (result > VK_SUCCESS && status < VK_SUCCESS)) result = status;
            if ((infos[i].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT) || status == VK_ERROR_OUT_OF_HOST_MEMORY) break;
        }
    }
    return true;
}

std::shared_ptr<MultiviewPipelineVariants> MultiviewPipelines::acquire(VkDevice device, VkPipeline pipeline) {
    if (!count_.load(std::memory_order_acquire)) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pipelines_.find(DeviceObjectKey::make(device, pipeline));
    return found == pipelines_.end() ? nullptr : found->second;
}
void MultiviewPipelines::forget(VkDevice device, VkPipeline pipeline) {
    if (!count_.load(std::memory_order_acquire)) return;
    std::shared_ptr<MultiviewPipelineVariants> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = pipelines_.find(DeviceObjectKey::make(device, pipeline));
        if (found == pipelines_.end()) return;
        retired = std::move(found->second); pipelines_.erase(found);
        count_.fetch_sub(1, std::memory_order_release);
    }
    // Allocation callback user data may cease to exist after public destruction.
    // Retained command metadata must not delay freeing the private pipelines.
    retired->release();
}
void MultiviewPipelines::forget_device(VkDevice device) {
    for (;;) {
        std::shared_ptr<MultiviewPipelineVariants> retired;
        bool more = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = live_.find(device);
            if (found != live_.end()) {
                if (!found->second.empty()) {
                    retired = found->second.back().lock(); found->second.pop_back(); more = true;
                }
                if (found->second.empty()) live_.erase(found);
            }
        }
        if (retired) retired->release();
        if (!more) break;
    }
    // Destructors run outside the registry lock, just like normal destruction.
    for (;;) {
        std::shared_ptr<MultiviewPipelineVariants> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = pipelines_.begin();
            while (found != pipelines_.end() && found->first.device != device) ++found;
            if (found == pipelines_.end()) break;
            retired = std::move(found->second); pipelines_.erase(found);
            count_.fetch_sub(1, std::memory_order_release);
        }
        retired->release();
    }
}
}

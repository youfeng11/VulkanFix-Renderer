#include "multiview_pipeline_layouts.h"
#include "driver_loader.h"
#include <algorithm>
namespace vkfix {
MultiviewPipelineLayout::MultiviewPipelineLayout(VkDevice device, PFN_vkDestroyPipelineLayout destroy)
    : device_(device), destroy_(destroy) {}
MultiviewPipelineLayout::~MultiviewPipelineLayout() { release(); }
VkPipelineLayout MultiviewPipelineLayout::native() { std::lock_guard<std::mutex> lock(mutex_); return native_; }
void MultiviewPipelineLayout::release() {
    VkDevice device; VkPipelineLayout native;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        device = device_; native = native_;
        device_ = VK_NULL_HANDLE; native_ = VK_NULL_HANDLE;
    }
    // Layer callbacks retire metadata in other modules and may enter this
    // registry again. No resource/registry mutex may span the callback.
    if (device && native && destroy_) destroy_(device, native, nullptr);
}
MultiviewPipelineLayouts& MultiviewPipelineLayouts::get() { static MultiviewPipelineLayouts registry; return registry; }
VkResult MultiviewPipelineLayouts::remember(VkDevice device, VkPipelineLayout handle, const VkPipelineLayoutCreateInfo& info,
    PFN_vkCreatePipelineLayout create, PFN_vkDestroyPipelineLayout destroy) {
    try {
        if (!create) create = reinterpret_cast<PFN_vkCreatePipelineLayout>(get_real_proc(get_last_instance(), device, "vkCreatePipelineLayout"));
        if (!destroy) destroy = reinterpret_cast<PFN_vkDestroyPipelineLayout>(get_real_proc(get_last_instance(), device, "vkDestroyPipelineLayout"));
        auto resource = std::make_shared<MultiviewPipelineLayout>(device, destroy);
        // The set-layout handles are valid now. Creating the equivalent layout
        // here avoids recreating it from destroyed set layouts or samplers later.
        if (!info.flags && !info.pNext) {
            if (!create || !destroy) return VK_ERROR_INITIALIZATION_FAILED;
            VkPipelineLayout native{};
            const auto status = create(device, &info, nullptr, &native);
            if (status != VK_SUCCESS) return status;
            if (!native) return VK_ERROR_INITIALIZATION_FAILED;
            resource->native_ = native;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto& live = live_[device];
        live.erase(std::remove_if(live.begin(), live.end(), [](const auto& r) { return r.expired(); }), live.end());
        live.push_back(resource); layouts_[DeviceObjectKey::make(device, handle)] = std::move(resource);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
std::shared_ptr<MultiviewPipelineLayout> MultiviewPipelineLayouts::acquire(VkDevice device, VkPipelineLayout handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = layouts_.find(DeviceObjectKey::make(device, handle));
    return found == layouts_.end() ? nullptr : found->second;
}
void MultiviewPipelineLayouts::forget(VkDevice device, VkPipelineLayout handle) {
    std::shared_ptr<MultiviewPipelineLayout> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = layouts_.find(DeviceObjectKey::make(device, handle));
        if (found == layouts_.end()) return;
        retired = std::move(found->second); layouts_.erase(found);
    }
}
void MultiviewPipelineLayouts::forget_device(VkDevice device) {
    std::vector<std::weak_ptr<MultiviewPipelineLayout>> live;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = live_.find(device);
        if (found != live_.end()) { live = std::move(found->second); live_.erase(found); }
    }
    for (const auto& weak : live) if (auto r = weak.lock()) r->release();
    std::lock_guard<std::mutex> lock(mutex_);
    // All resources have already detached their native handles; destroying the
    // final map references here cannot invoke a layer callback under the lock.
    for (auto it = layouts_.begin(); it != layouts_.end();) if (it->first.device == device) it = layouts_.erase(it); else ++it;
}
}

#pragma once
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkfix {
class ImageSliceCopyRepair {
public:
    static ImageSliceCopyRepair &get();
    enum class ImageKind { Other, Array, Volume };
    static ImageKind image_kind(const VkImageCreateInfo &);
    void remember_image(VkDevice, VkImage, ImageKind, uint32_t layers,
                        VkInstance, VkPhysicalDevice, PFN_vkGetInstanceProcAddr);
    void forget_image(VkDevice, VkImage);
    void forget_device(VkDevice);
    // An empty result keeps the original command. Splitting retains a single
    // native copy command with disjoint regions, so no barriers are added.
    std::vector<VkImageCopy> split_regions(VkDevice, uint32_t queue_family, VkImage src, VkImage dst,
                                          uint32_t count, const VkImageCopy *) const;
private:
    struct DeviceImages {
        std::unordered_map<VkImage, uint32_t> arrays;
        std::unordered_set<VkImage> volumes;
        std::vector<VkQueueFamilyProperties> families;
    };
    mutable std::shared_mutex mutex_;
    std::unordered_map<VkDevice, DeviceImages> devices_;
};
} // namespace vkfix

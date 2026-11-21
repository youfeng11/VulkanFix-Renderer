#pragma once
#include "multiview_attachments.h"

namespace vkfix {
struct ImageTypeCopyPlan {
    VkDeviceSize bytes = 0;
    std::vector<VkBufferImageCopy> reads, writes;
};
VkResult plan_image_type_copy(const MultiviewImage&, const MultiviewImage&,
                             uint32_t, const VkImageCopy*, ImageTypeCopyPlan&);

// Private transfer storage is retained for the entire recorded command-buffer
// lifetime, including resubmission, and retired on reset/free/pool/device teardown.
class ImageTypeCopy {
public:
    static ImageTypeCopy& get();
    VkResult begin(VkDevice, VkCommandBuffer, VkCommandPool);
    bool copy(VkDevice, VkPhysicalDevice, VkCommandBuffer, VkImage, VkImageLayout,
              VkImage, VkImageLayout, uint32_t, const VkImageCopy*);
    // Internal images are not public registry entries. Their owner supplies
    // immutable metadata; staging still belongs to the recorded command.
    VkResult copy(VkDevice, VkPhysicalDevice, VkCommandBuffer,
                  const MultiviewImage&, VkImageLayout, const MultiviewImage&, VkImageLayout,
                  uint32_t, const VkImageCopy*);
    VkResult error(VkCommandBuffer);
    void forget(VkCommandBuffer);
    void forget_pool(VkDevice, VkCommandPool);
    void forget_device(VkDevice);
private:
    struct Recording;
    std::shared_ptr<Recording> acquire(VkCommandBuffer);
    void forget_matching(VkDevice, VkCommandPool, bool whole_device);
    std::mutex mutex_;
    std::unordered_map<VkCommandBuffer, std::shared_ptr<Recording>> commands_;
};
}

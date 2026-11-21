#pragma once
#include <vector>
#include <vulkan/vulkan.h>

namespace vkfix {
// Own only bindings changed for this immediate support query. The application's
// layout definition, binding flags and immutable samplers remain untouched.
class DescriptorLayoutQuery {
public:
    VkResult prepare(const VkDescriptorSetLayoutCreateInfo&, bool vertex_compute);
    VkDescriptorSetLayoutCreateInfo& info() { return info_; }
private:
    VkDescriptorSetLayoutCreateInfo info_{};
    std::vector<VkDescriptorSetLayoutBinding> bindings_;
};
}

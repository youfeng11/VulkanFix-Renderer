#pragma once
#include "vk_common.h"
#include <memory>

// Split aggregate outputs for pre-1.2 drivers. The native query omits
// unsupported structures, while module hooks still see their emulated outputs.
// Folding and exact application-chain restoration happen after all post-hooks.
class PromotedPropertyQuery {
public:
  PromotedPropertyQuery(VkPhysicalDevice physical,
                        VkPhysicalDeviceProperties2 *output);
  ~PromotedPropertyQuery();
  void query_native();
  uint32_t native_api_version() const;
  PromotedPropertyQuery(const PromotedPropertyQuery &) = delete;
  PromotedPropertyQuery &operator=(const PromotedPropertyQuery &) = delete;

private:
  struct State;
  std::unique_ptr<State> state;
};

PFN_vkGetPhysicalDeviceProperties2
native_properties2_proc(VkPhysicalDevice physical = VK_NULL_HANDLE);

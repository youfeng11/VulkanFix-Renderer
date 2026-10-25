#pragma once
#include "vk_common.h"
#include <memory>
#include <vector>

// Bridge the 1.2 aggregate feature structs to individual promoted structures
// on older drivers. Query results are folded back only after module post-hooks.
class PromotedFeatureQuery {
public:
  PromotedFeatureQuery(VkPhysicalDevice physical,
                       VkPhysicalDeviceFeatures2 *features);
  ~PromotedFeatureQuery();
  PromotedFeatureQuery(const PromotedFeatureQuery &) = delete;
  PromotedFeatureQuery &operator=(const PromotedFeatureQuery &) = delete;

private:
  struct State;
  std::unique_ptr<State> state;
};

// Owns a private input chain for vkCreateDevice, including its generated
// promotion nodes. Never mutates the application's structures, even in nested
// or concurrent device creations.
class PromotedDeviceFeatures {
public:
  PromotedDeviceFeatures(VkPhysicalDevice physical, VkDeviceCreateInfo &info,
                         std::vector<const char *> &extensions);
  ~PromotedDeviceFeatures();
  VkResult result() const;
  PromotedDeviceFeatures(const PromotedDeviceFeatures &) = delete;
  PromotedDeviceFeatures &operator=(const PromotedDeviceFeatures &) = delete;

private:
  struct State;
  std::unique_ptr<State> state;
};

PFN_vkGetPhysicalDeviceFeatures2
native_features2_proc(VkPhysicalDevice physical = VK_NULL_HANDLE);

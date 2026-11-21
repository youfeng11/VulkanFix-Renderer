#pragma once
#include "vk_common.h"

namespace vkfix {
// Select the instance's enabled core/KHR route, then publish only groups whose
// members can support the public Core device-group operations; legacy
// creation-only instances retain their native topology. No caller headers
// or payloads are changed unless the complete native snapshot succeeded.
VkResult enumerate_layer_device_groups(VkInstance,uint32_t*,VkPhysicalDeviceGroupProperties*);
// Called after native extension dependency closure, before vkCreateDevice.
// The head belongs to PromotedDeviceFeatures, never to the application.
VkResult prepare_device_group_creation(VkInstance,VkPhysicalDevice,const VkDeviceCreateInfo& application,
    VkDeviceCreateInfo& native,bool native_group_operations,VkDeviceGroupDeviceCreateInfo& storage);
}

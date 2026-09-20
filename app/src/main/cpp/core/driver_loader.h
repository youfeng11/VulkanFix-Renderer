#ifndef DRIVER_LOADER_H
#define DRIVER_LOADER_H

#include "vk_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void init_real_vulkan();
void register_vulkan_ptr();
void* get_real_proc(VkInstance instance, VkDevice device, const char* name);
PFN_vkGetInstanceProcAddr get_real_instance_proc_addr();
PFN_vkGetDeviceProcAddr get_real_device_proc_addr();
void set_last_instance(VkInstance instance);
VkInstance get_last_instance();

#ifdef __cplusplus
}
#endif

#endif // DRIVER_LOADER_H

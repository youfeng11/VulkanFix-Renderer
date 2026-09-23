#include "host_query_reset.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include "vk_pnext.h"

REGISTER_LAYER_MODULE(HostQueryResetModule);

HostQueryResetModule::HostQueryResetModule()
    : ExtensionModuleBase(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME, VK_EXT_HOST_QUERY_RESET_SPEC_VERSION, VK_API_VERSION_1_2) {
    LOGI("HostQueryResetModule initialized");
}

void HostQueryResetModule::on_pre_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void*& pUserData
) {
    pUserData = nullptr;
    if (is_phys_device_native(physicalDevice) || !pFeatures) return;

    pUserData = vku::unlink_pnext(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
}

void HostQueryResetModule::on_post_get_features2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures,
    void* pUserData
) {
    if (!pFeatures || !pUserData) return;

    auto* f = reinterpret_cast<VkPhysicalDeviceHostQueryResetFeatures*>(pUserData);
    f->hostQueryReset = VK_TRUE;
    vku::relink_pnext(pFeatures->pNext, pUserData);
}

void HostQueryResetModule::on_pre_create_device_custom(
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* pCreateInfo,
    VkPhysicalDeviceFeatures* pEnabledFeatures,
    std::vector<const char*>& enabledExtensions,
    void*& pUserData
) {
    if (pCreateInfo) {
        vku::unlink_pnext(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
    }
}

bool HostQueryResetModule::on_reset_query_pool(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount
) {
    if (is_device_native(device)) return false;

    const auto& dt = LayerManager::get().get_dispatch_table(device);
    PFN_vkResetQueryPool real_fn = dt.ResetQueryPool;
    if (!real_fn) {
        real_fn = (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPool");
    }
    if (!real_fn) {
        real_fn = (PFN_vkResetQueryPool) get_real_proc(get_last_instance(), device, "vkResetQueryPoolEXT");
    }
    if (real_fn) {
        real_fn(device, queryPool, firstQuery, queryCount);
        return true;
    }

    // Emulate via short-lived command buffer
    PFN_vkCreateCommandPool real_cp = (PFN_vkCreateCommandPool) get_real_proc(get_last_instance(), device, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool real_dp = (PFN_vkDestroyCommandPool) get_real_proc(get_last_instance(), device, "vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers real_ac = (PFN_vkAllocateCommandBuffers) get_real_proc(get_last_instance(), device, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer real_bc = (PFN_vkBeginCommandBuffer) get_real_proc(get_last_instance(), device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer real_ec = (PFN_vkEndCommandBuffer) get_real_proc(get_last_instance(), device, "vkEndCommandBuffer");
    PFN_vkCmdResetQueryPool real_cmd_reset = (PFN_vkCmdResetQueryPool) get_real_proc(get_last_instance(), device, "vkCmdResetQueryPool");
    PFN_vkQueueSubmit real_qs = (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueSubmit");
    PFN_vkQueueWaitIdle real_qwi = (PFN_vkQueueWaitIdle) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueWaitIdle");

    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    if (!LayerManager::get().get_device_queue_info(device, queue, queueFamilyIndex)) {
        PFN_vkGetDeviceQueue real_gdq = (PFN_vkGetDeviceQueue) get_real_proc(get_last_instance(), device, "vkGetDeviceQueue");
        if (real_gdq) real_gdq(device, 0, 0, &queue);
    }

    if (real_cp && real_dp && real_ac && real_bc && real_ec && real_cmd_reset && real_qs && queue) {
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo poolCI{};
        poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolCI.queueFamilyIndex = queueFamilyIndex;

        if (real_cp(device, &poolCI, nullptr, &cmdPool) == VK_SUCCESS) {
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = cmdPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;

            VkCommandBuffer cmd = VK_NULL_HANDLE;
            if (real_ac(device, &allocInfo, &cmd) == VK_SUCCESS) {
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

                real_bc(cmd, &beginInfo);
                real_cmd_reset(cmd, queryPool, firstQuery, queryCount);
                real_ec(cmd);

                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &cmd;

                real_qs(queue, 1, &submitInfo, VK_NULL_HANDLE);
                if (real_qwi) real_qwi(queue);
            }
            real_dp(device, cmdPool, nullptr);
        }
    }
    return true;
}

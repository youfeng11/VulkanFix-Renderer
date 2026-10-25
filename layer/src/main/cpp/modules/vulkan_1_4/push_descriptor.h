#ifndef PUSH_DESCRIPTOR_H
#define PUSH_DESCRIPTOR_H

#include "layer_module.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PushDescriptorModule : public IVulkanLayerModule {
  public:
    PushDescriptorModule();
    virtual ~PushDescriptorModule() = default;

    const char *get_name() const override {
        return "VK_KHR_push_descriptor";
    }

    // 1. Extensions
    void on_enumerate_device_extensions(VkPhysicalDevice physicalDevice,
                                        std::vector<VkExtensionProperties> &extensions) override;

    // 2. Properties
    void on_pre_get_properties2(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceProperties2 *pProperties,
                                void *&pUserData) override;

    void on_post_get_properties2(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceProperties2 *pProperties,
                                 void *pUserData) override;

    // 3. Logical Device
    void on_pre_create_device(VkPhysicalDevice physicalDevice, VkDeviceCreateInfo *pCreateInfo,
                              VkPhysicalDeviceFeatures *pEnabledFeatures,
                              std::vector<const char *> &enabledExtensions,
                              void *&pUserData) override;

    void on_post_create_device(VkPhysicalDevice physicalDevice, VkDevice device, VkResult result,
                               void *pUserData) override;

    void on_destroy_device(VkDevice device) override;

    // 4. Descriptor Set Layout
    void on_pre_create_descriptor_set_layout(VkDevice device,
                                             VkDescriptorSetLayoutCreateInfo &createInfo) override;

    void on_post_create_descriptor_set_layout(VkDevice device,
                                              const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                              VkResult result,
                                              VkDescriptorSetLayout setLayout) override;

    void on_destroy_descriptor_set_layout(VkDevice device,
                                          VkDescriptorSetLayout setLayout) override;

    // 5. Pipeline Layout
    void on_post_create_pipeline_layout(VkDevice device,
                                        const VkPipelineLayoutCreateInfo *pCreateInfo,
                                        VkResult result, VkPipelineLayout pipelineLayout) override;

    void on_destroy_pipeline_layout(VkDevice device, VkPipelineLayout pipelineLayout) override;

    // 6. Command Buffers
    void on_post_allocate_command_buffers(VkDevice device,
                                          const VkCommandBufferAllocateInfo *pAllocateInfo,
                                          VkResult result,
                                          VkCommandBuffer *pCommandBuffers) override;

    void on_free_command_buffers(VkDevice device, uint32_t count,
                                 const VkCommandBuffer *pCommandBuffers) override;

    void on_begin_command_buffer(VkCommandBuffer commandBuffer,
                                 const VkCommandBufferBeginInfo *pBeginInfo) override;

    void on_reset_command_buffer(VkCommandBuffer commandBuffer,
                                 VkCommandBufferResetFlags flags) override;

    // 7. Descriptor Update Template
    void on_pre_create_descriptor_update_template(
        VkDevice device, VkDescriptorUpdateTemplateCreateInfo &createInfo) override;
    void on_post_create_descriptor_update_template(
        VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *info, VkResult result,
        VkDescriptorUpdateTemplate descriptorTemplate) override;
    bool on_destroy_descriptor_update_template(VkDevice device,
                                               VkDescriptorUpdateTemplate descriptorTemplate,
                                               const VkAllocationCallbacks *) override;
    void on_cmd_bind_descriptor_sets(VkCommandBuffer cmd, VkPipelineBindPoint point,
                                     VkPipelineLayout layout, uint32_t first,
                                     uint32_t count) override;
    void on_reset_command_pool(VkDevice device, VkCommandPool pool) override;
    void on_destroy_command_pool(VkDevice device, VkCommandPool pool) override;
    VkResult command_recording_error(VkCommandBuffer cmd) override;

    // 8. Push Descriptors
    bool on_cmd_push_descriptor_set(VkCommandBuffer commandBuffer,
                                    VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
                                    uint32_t set, uint32_t descriptorWriteCount,
                                    const VkWriteDescriptorSet *pDescriptorWrites) override;

    bool on_cmd_push_descriptor_set_with_template(
        VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        VkPipelineLayout layout, uint32_t set, const void *pData) override;

    bool is_device_native(VkDevice device) override;

    bool on_queue_present(VkQueue queue, const VkPresentInfoKHR *pPresentInfo,
                          VkResult &outResult) override;

  private:
    bool is_phys_device_native(VkPhysicalDevice physDev);
    VkDevice get_device_for_cmd(VkCommandBuffer cmd);
    struct SetLayout;
    struct PipelineLayout;
    using ObjectKey = std::pair<uint64_t, uint64_t>;
    static ObjectKey object_key(VkDevice device, uint64_t handle);
    std::shared_ptr<PipelineLayout> get_layout(VkDevice device, VkPipelineLayout layout);
    static bool compatible(const std::shared_ptr<PipelineLayout> &a,
                           const std::shared_ptr<PipelineLayout> &b, uint32_t set);
    VkDescriptorSet allocate_push_set(VkDevice device, VkCommandBuffer cmd,
                                      const std::shared_ptr<SetLayout> &layout);
    VkDescriptorSet prepare_snapshot(VkDevice device, VkCommandBuffer cmd,
                                     VkPipelineBindPoint point,
                                     const std::shared_ptr<PipelineLayout> &layout, uint32_t set,
                                     uint32_t write_count, const VkWriteDescriptorSet *writes);
    VkDescriptorPool create_pool(VkDevice device, uint32_t maxSets);
    void reset_cmd_pools(VkCommandBuffer cmd);
    VkDescriptorSet fail_recording(VkCommandBuffer cmd, VkResult error);

    struct CmdPushState {
        VkDevice device = VK_NULL_HANDLE;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkResult error = VK_SUCCESS;
        struct BoundSet {
            std::shared_ptr<PipelineLayout> layout;
            VkDescriptorSet snapshot = VK_NULL_HANDLE;
        };
        std::map<VkPipelineBindPoint, std::map<uint32_t, BoundSet>> bindings;
        std::vector<VkDescriptorPool> pools;
        size_t current_pool_idx = 0;
        uint32_t current_pool_allocated = 0;

        static constexpr uint32_t BATCH_CAPACITY = 8;
        std::shared_ptr<SetLayout> cached_layout;
        VkDescriptorSet cached_sets[BATCH_CAPACITY]{};
        uint32_t cached_count = 0;
        uint32_t cached_index = 0;

        void reset_batch() {
            cached_layout.reset();
            cached_count = 0;
            cached_index = 0;
        }
    };
    static void bind_state(CmdPushState &state, VkPipelineBindPoint point,
                           const std::shared_ptr<PipelineLayout> &layout, uint32_t set,
                           VkDescriptorSet snapshot);
    struct SetLayout {
        VkDevice device = VK_NULL_HANDLE;
        VkDescriptorSetLayout allocation_layout = VK_NULL_HANDLE;
        VkResult allocation_error = VK_SUCCESS;
        std::vector<uint64_t> definition;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<std::vector<VkSampler>> samplers;
        ~SetLayout();
    };
    struct PipelineLayout {
        VkDevice device = VK_NULL_HANDLE;
        bool independent_sets = false;
        std::vector<std::shared_ptr<SetLayout>> sets;
        std::vector<uint64_t> push_ranges;
    };
    struct TemplateInfo {
        VkDevice device = VK_NULL_HANDLE;
        VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_GRAPHICS;
        std::vector<VkWriteDescriptorSet> coverage;
    };

    mutable std::shared_mutex m_rw_mutex;
    std::mutex m_cmd_mutex;
    std::atomic<VkDevice> m_primary_dev{VK_NULL_HANDLE};
    std::atomic<bool> m_primary_native{false};
    std::unordered_map<uint64_t, bool> m_phys_native_support;
    std::unordered_map<uint64_t, bool> m_device_native_support;
    std::map<ObjectKey, std::shared_ptr<SetLayout>> m_set_layouts;
    std::map<ObjectKey, std::shared_ptr<PipelineLayout>> m_pipeline_layouts;
    std::map<ObjectKey, TemplateInfo> m_templates;
    std::unordered_map<uint64_t, CmdPushState> m_cmd_states;

    // Per-present statistics: pushes between presents give the real per-frame
    // cost of the push-descriptor emulation (as opposed to the one-time load).
    std::atomic<uint64_t> m_pushes_since_present{0};
    std::atomic<uint64_t> m_present_frames{0};
    std::atomic<uint64_t> m_present_pushes_total{0};
    std::atomic<uint64_t> m_inherited_snapshots{0};
};

#endif // PUSH_DESCRIPTOR_H

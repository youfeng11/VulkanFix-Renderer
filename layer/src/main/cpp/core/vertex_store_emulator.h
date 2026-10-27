#pragma once
#include "vk_common.h"
#include <memory>
#include <vector>

// Compute/replay fallback for devices without native vertex storage support.
class VertexStoreEmulator {
  public:
    struct MemoryStats {
        uint64_t scratch_bytes = 0, peak_scratch_bytes = 0;
        uint64_t total_draws = 0, copied_input_bytes = 0, live_draws = 0, command_buffers = 0;
        uint64_t cloned_set_layouts = 0, cloned_pipeline_layouts = 0;
    };
    static VertexStoreEmulator &get();
    MemoryStats memory_stats(VkDevice device);
    void on_present(VkDevice device);
    bool enabled(VkDevice device);
    void enable(VkDevice device, VkPhysicalDevice physical);
    void destroy_device(VkDevice device);
    void shader(VkDevice, VkShaderModule, const VkShaderModuleCreateInfo *);
    void erase_shader(VkDevice, VkShaderModule);
    VkBufferCreateInfo buffer_info(VkDevice, const VkBufferCreateInfo &);
    void buffer(VkDevice, VkBuffer, const VkBufferCreateInfo *);
    void erase_buffer(VkDevice, VkBuffer);
    void set_layout(VkDevice, VkDescriptorSetLayout, const VkDescriptorSetLayoutCreateInfo *);
    void erase_set_layout(VkDevice, VkDescriptorSetLayout);
    void layout(VkDevice, VkPipelineLayout, const VkPipelineLayoutCreateInfo *);
    void erase_layout(VkDevice, VkPipelineLayout);
    void image(VkDevice, VkImage, const VkImageCreateInfo *);
    void view(VkDevice, VkImageView, const VkImageViewCreateInfo *);
    void erase_image(VkDevice, VkImage);
    void erase_view(VkDevice, VkImageView);
    bool create_pipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *,
                          const VkAllocationCallbacks *, VkPipeline *, VkResult &);
    void erase_pipeline(VkDevice, VkPipeline);
    void allocate_commands(VkDevice, const VkCommandBufferAllocateInfo *, const VkCommandBuffer *);
    void clear_commands(VkCommandBuffer);
    void free_commands(VkDevice, uint32_t, const VkCommandBuffer *);
    void clear_pool(VkDevice, VkCommandPool);
    void destroy_pool(VkDevice, VkCommandPool);
    void pool(VkDevice, VkCommandPool, uint32_t family);
    void bind_pipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
    void bind_vertices(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *,
                       const VkDeviceSize *strides = nullptr, const VkDeviceSize *sizes = nullptr);
    void bind_index(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
    void bind_sets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                   const VkDescriptorSet *, uint32_t, const uint32_t *);
    void push(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t,
              const void *);
    const VkRenderingInfo *begin_rendering(VkCommandBuffer, const VkRenderingInfo *);
    void end_rendering(VkCommandBuffer);
    void legacy_rendering(VkCommandBuffer, bool);
    void query(VkCommandBuffer, bool);
    bool draw(VkCommandBuffer, uint32_t count, uint32_t instances, uint32_t first,
              int32_t vertex_offset, uint32_t first_instance, bool indexed);
    void indirect(VkCommandBuffer);
    void unsupported(VkCommandBuffer, const char *reason);
    VkResult recording_error(VkCommandBuffer);

  private:
    VertexStoreEmulator();
    ~VertexStoreEmulator();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Read-only diagnostics for regression runners and native debugging tools.
extern "C" VK_LAYER_EXPORT VkBool32
vulkanfixGetVertexStoreMemoryStats(VkDevice device, VertexStoreEmulator::MemoryStats *out);

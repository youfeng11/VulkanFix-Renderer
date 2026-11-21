#ifndef DESCRIPTOR_UPDATE_TEMPLATE_H
#define DESCRIPTOR_UPDATE_TEMPLATE_H

#include "extension_module_base.h"
#include "core/host_object_allocator.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>
#include <atomic>

class DescriptorUpdateTemplateModule : public ExtensionModuleBase {
public:
    DescriptorUpdateTemplateModule();
    ~DescriptorUpdateTemplateModule() override = default;
    void set_native_route(VkDevice, bool);
    void set_update_callback(PFN_vkUpdateDescriptorSets update) { m_update.store(update,std::memory_order_release); }
    bool push_emulated_template(VkDevice, VkCommandBuffer, VkDescriptorUpdateTemplate,
        VkPipelineLayout, uint32_t, const void*, PFN_vkCmdPushDescriptorSetKHR);

    void on_destroy_device(VkDevice device) override;

    bool on_create_descriptor_update_template(
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate,
        VkResult& outResult) override;

    bool on_destroy_descriptor_update_template(
        VkDevice device,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const VkAllocationCallbacks* pAllocator) override;

    bool on_update_descriptor_set_with_template(
        VkDevice device,
        VkDescriptorSet descriptorSet,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const void* pData) override;

private:
    struct Entry {
        explicit Entry(const vkfix::HostObjectAllocator<Entry>& allocator) noexcept
            : images(allocator), buffers(allocator), views(allocator), acceleration(allocator), accelerationNV(allocator) {}
        VkDescriptorUpdateTemplateEntry info;
        std::vector<VkDescriptorImageInfo, vkfix::HostObjectAllocator<VkDescriptorImageInfo>> images;
        std::vector<VkDescriptorBufferInfo, vkfix::HostObjectAllocator<VkDescriptorBufferInfo>> buffers;
        std::vector<VkBufferView, vkfix::HostObjectAllocator<VkBufferView>> views;
        std::vector<VkAccelerationStructureKHR, vkfix::HostObjectAllocator<VkAccelerationStructureKHR>> acceleration;
        std::vector<VkAccelerationStructureNV, vkfix::HostObjectAllocator<VkAccelerationStructureNV>> accelerationNV;
        VkWriteDescriptorSetInlineUniformBlock inlineBlock{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK};
        VkWriteDescriptorSetAccelerationStructureKHR accelerationInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        VkWriteDescriptorSetAccelerationStructureNV accelerationNVInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_NV};
    };
    struct EmulatedTemplate {
        explicit EmulatedTemplate(const vkfix::HostObjectAllocator<EmulatedTemplate>& alloc) noexcept
            : allocator(alloc), entries(alloc) {}
        vkfix::HostObjectAllocator<EmulatedTemplate> allocator;
        VkDevice device;
        VkDescriptorUpdateTemplateType templateType;
        std::vector<Entry, vkfix::HostObjectAllocator<Entry>> entries;
        VkPipelineLayout pipelineLayout;
        VkPipelineBindPoint point;
        uint32_t set;
        std::mutex updateMutex;
    };
    template<class Emit> static void emit_writes(EmulatedTemplate&, VkDescriptorSet, const void*, Emit);
    std::mutex m_template_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<EmulatedTemplate>> m_templates;
    std::atomic<PFN_vkUpdateDescriptorSets> m_update{nullptr};
};

#endif // DESCRIPTOR_UPDATE_TEMPLATE_H

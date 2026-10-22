#ifndef DESCRIPTOR_UPDATE_TEMPLATE_H
#define DESCRIPTOR_UPDATE_TEMPLATE_H

#include "extension_module_base.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>

class DescriptorUpdateTemplateModule : public ExtensionModuleBase {
public:
    DescriptorUpdateTemplateModule();
    ~DescriptorUpdateTemplateModule() override = default;

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
    struct EmulatedTemplate {
        VkDescriptorUpdateTemplateType templateType;
        std::vector<VkDescriptorUpdateTemplateEntry> entries;
        VkPipelineLayout pipelineLayout;
        uint32_t set;
    };
    std::mutex m_template_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<EmulatedTemplate>> m_templates;
};

#endif // DESCRIPTOR_UPDATE_TEMPLATE_H

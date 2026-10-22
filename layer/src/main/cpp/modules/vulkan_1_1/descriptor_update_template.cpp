#include "descriptor_update_template.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include <atomic>

REGISTER_LAYER_MODULE(DescriptorUpdateTemplateModule);

DescriptorUpdateTemplateModule::DescriptorUpdateTemplateModule()
    : ExtensionModuleBase(VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_SPEC_VERSION, VK_API_VERSION_1_1) {
    LOGI("DescriptorUpdateTemplateModule initialized");
}

void DescriptorUpdateTemplateModule::on_destroy_device(VkDevice device) {
    ExtensionModuleBase::on_destroy_device(device);
    std::lock_guard<std::mutex> lock(m_template_mutex);
    m_templates.clear();
}

bool DescriptorUpdateTemplateModule::on_create_descriptor_update_template(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    PFN_vkCreateDescriptorUpdateTemplate real_fn =
        (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkCreateDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkCreateDescriptorUpdateTemplateKHR");
    }
    if (real_fn) {
        outResult = real_fn(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
        return true;
    }

    if (!pCreateInfo || !pDescriptorUpdateTemplate) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    auto tmpl = std::make_shared<EmulatedTemplate>();
    tmpl->templateType = pCreateInfo->templateType;
    tmpl->pipelineLayout = pCreateInfo->pipelineLayout;
    tmpl->set = pCreateInfo->set;
    if (pCreateInfo->pDescriptorUpdateEntries && pCreateInfo->descriptorUpdateEntryCount > 0) {
        tmpl->entries.assign(
            pCreateInfo->pDescriptorUpdateEntries,
            pCreateInfo->pDescriptorUpdateEntries + pCreateInfo->descriptorUpdateEntryCount
        );
    }

    static std::atomic<uint64_t> s_next_template_id{0x11000000ULL};
    uint64_t handleVal = ++s_next_template_id;
    VkDescriptorUpdateTemplate handle = (VkDescriptorUpdateTemplate)(uintptr_t)handleVal;

    std::lock_guard<std::mutex> lock(m_template_mutex);
    m_templates[handleVal] = tmpl;

    *pDescriptorUpdateTemplate = handle;
    outResult = VK_SUCCESS;
    return true;
}

bool DescriptorUpdateTemplateModule::on_destroy_descriptor_update_template(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks* pAllocator
) {
    if (is_device_native(device)) return false;

    PFN_vkDestroyDescriptorUpdateTemplate real_fn =
        (PFN_vkDestroyDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorUpdateTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkDestroyDescriptorUpdateTemplate) get_real_proc(get_last_instance(), device, "vkDestroyDescriptorUpdateTemplateKHR");
    }
    if (real_fn) {
        real_fn(device, descriptorUpdateTemplate, pAllocator);
        return true;
    }

    std::lock_guard<std::mutex> lock(m_template_mutex);
    m_templates.erase((uint64_t)(uintptr_t)descriptorUpdateTemplate);
    return true;
}

bool DescriptorUpdateTemplateModule::on_update_descriptor_set_with_template(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const void* pData
) {
    if (is_device_native(device)) return false;

    PFN_vkUpdateDescriptorSetWithTemplate real_fn =
        (PFN_vkUpdateDescriptorSetWithTemplate) get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSetWithTemplate");
    if (!real_fn) {
        real_fn = (PFN_vkUpdateDescriptorSetWithTemplate) get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSetWithTemplateKHR");
    }
    if (real_fn) {
        real_fn(device, descriptorSet, descriptorUpdateTemplate, pData);
        return true;
    }

    std::shared_ptr<EmulatedTemplate> tmpl;
    {
        std::lock_guard<std::mutex> lock(m_template_mutex);
        auto it = m_templates.find((uint64_t)(uintptr_t)descriptorUpdateTemplate);
        if (it != m_templates.end()) {
            tmpl = it->second;
        }
    }

    if (!tmpl || !pData) return true;

    PFN_vkUpdateDescriptorSets real_update =
        (PFN_vkUpdateDescriptorSets) get_real_proc(get_last_instance(), device, "vkUpdateDescriptorSets");
    if (!real_update) return true;

    for (const auto& entry : tmpl->entries) {
        for (uint32_t i = 0; i < entry.descriptorCount; ++i) {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.dstBinding = entry.dstBinding;
            write.dstArrayElement = entry.dstArrayElement + i;
            write.descriptorCount = 1;
            write.descriptorType = entry.descriptorType;

            const char* entryPtr = ((const char*) pData) + entry.offset + i * entry.stride;

            switch (entry.descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    write.pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(entryPtr);
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    write.pBufferInfo = reinterpret_cast<const VkDescriptorBufferInfo*>(entryPtr);
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    write.pTexelBufferView = reinterpret_cast<const VkBufferView*>(entryPtr);
                    break;
                default:
                    break;
            }
            real_update(device, 1, &write, 0, nullptr);
        }
    }
    return true;
}

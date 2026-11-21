#include "descriptor_update_template.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include <atomic>
#include <cstring>

namespace {
template <typename T, typename Allocator>
const T* descriptor_infos(const char* data, uint32_t count, size_t stride, std::vector<T, Allocator>& scratch) {
    if ((count == 1 || stride == sizeof(T)) && uintptr_t(data) % alignof(T) == 0)
        return reinterpret_cast<const T*>(data);
    // Storage was reserved when the template was created, where OOM can be
    // returned. Host updates and command recording must not allocate it.
    for (uint32_t i = 0; i < count; ++i)
        std::memcpy(&scratch[i], data + size_t(i) * stride, sizeof(T));
    return scratch.data();
}
}

REGISTER_LAYER_MODULE(DescriptorUpdateTemplateModule);

DescriptorUpdateTemplateModule::DescriptorUpdateTemplateModule()
    : ExtensionModuleBase(VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_SPEC_VERSION, VK_API_VERSION_1_1) {
    LOGI("DescriptorUpdateTemplateModule initialized");
}

void DescriptorUpdateTemplateModule::set_native_route(VkDevice device, bool native) {
    std::unique_lock<std::shared_mutex> lock(m_ext_mutex);
    m_device_native[(uint64_t)(uintptr_t)device] = native;
    m_native_cache.invalidate();
}

void DescriptorUpdateTemplateModule::on_destroy_device(VkDevice device) {
    ExtensionModuleBase::on_destroy_device(device);
    std::lock_guard<std::mutex> lock(m_template_mutex);
    for (auto it = m_templates.begin(); it != m_templates.end();) {
        if (it->second->device == device) it = m_templates.erase(it);
        else ++it;
    }
}

bool DescriptorUpdateTemplateModule::on_create_descriptor_update_template(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    if (!pCreateInfo || !pDescriptorUpdateTemplate) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    *pDescriptorUpdateTemplate = VK_NULL_HANDLE;
    try {
        auto allocator = vkfix::HostObjectAllocator<EmulatedTemplate>::for_object(pAllocator);
        auto tmpl = std::allocate_shared<EmulatedTemplate>(allocator, allocator);
        tmpl->device = device;
        tmpl->templateType = pCreateInfo->templateType;
        tmpl->pipelineLayout = VK_NULL_HANDLE;
        tmpl->point = VK_PIPELINE_BIND_POINT_GRAPHICS;
        tmpl->set = 0;
        if (tmpl->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR) {
            tmpl->pipelineLayout = pCreateInfo->pipelineLayout;
            tmpl->point = pCreateInfo->pipelineBindPoint;
            tmpl->set = pCreateInfo->set;
        }
        if (pCreateInfo->pDescriptorUpdateEntries && pCreateInfo->descriptorUpdateEntryCount > 0) {
            tmpl->entries.reserve(pCreateInfo->descriptorUpdateEntryCount);
            for (uint32_t i = 0; i < pCreateInfo->descriptorUpdateEntryCount; ++i) {
                auto& entry = tmpl->entries.emplace_back(tmpl->entries.get_allocator());
                entry.info = pCreateInfo->pDescriptorUpdateEntries[i];
                switch (entry.info.descriptorType) {
                    case VK_DESCRIPTOR_TYPE_SAMPLER:
                    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    case VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM:
                    case VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM:
                        entry.images.resize(entry.info.descriptorCount); break;
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                        entry.buffers.resize(entry.info.descriptorCount); break;
                    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                        entry.views.resize(entry.info.descriptorCount); break;
                    case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: break;
                    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                        entry.acceleration.resize(entry.info.descriptorCount); break;
                    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
                        entry.accelerationNV.resize(entry.info.descriptorCount); break;
                    default: outResult = VK_ERROR_FEATURE_NOT_PRESENT; return true;
                }
            }
        }

        static std::atomic<uint64_t> s_next_template_id{0x11000000ULL};
        std::lock_guard<std::mutex> lock(m_template_mutex);
        uint64_t handleVal;
        do { handleVal = ++s_next_template_id; } while (!handleVal || m_templates.count(handleVal));
        VkDescriptorUpdateTemplate handle = (VkDescriptorUpdateTemplate)handleVal;
        m_templates.emplace(handleVal, std::move(tmpl));

        *pDescriptorUpdateTemplate = handle;
        outResult = VK_SUCCESS;
    } catch (const std::bad_alloc&) { outResult = VK_ERROR_OUT_OF_HOST_MEMORY; }
    return true;
}

bool DescriptorUpdateTemplateModule::on_destroy_descriptor_update_template(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks* pAllocator
) {
    if (is_device_native(device)) return false;

    std::lock_guard<std::mutex> lock(m_template_mutex);
    auto it = m_templates.find((uint64_t)descriptorUpdateTemplate);
    if (it != m_templates.end() && it->second->device == device) {
        it->second->allocator.set_destroy_callbacks(pAllocator);
        m_templates.erase(it);
    }
    return true;
}

bool DescriptorUpdateTemplateModule::on_update_descriptor_set_with_template(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const void* pData
) {
    if (is_device_native(device)) return false;

    std::shared_ptr<EmulatedTemplate> tmpl;
    {
        std::lock_guard<std::mutex> lock(m_template_mutex);
        auto it = m_templates.find((uint64_t)descriptorUpdateTemplate);
        if (it != m_templates.end() && it->second->device == device) {
            tmpl = it->second;
        }
    }

    if (!tmpl || !pData) return true;

    auto real_update = m_update.load(std::memory_order_acquire);
    if (!real_update) real_update = (PFN_vkUpdateDescriptorSets)get_real_proc(get_last_instance(),device,"vkUpdateDescriptorSets");
    if (!real_update) return true;

    emit_writes(*tmpl, descriptorSet, pData, [&](const VkWriteDescriptorSet& write) {
        real_update(device, 1, &write, 0, nullptr);
    });
    return true;
}

template<class Emit> void DescriptorUpdateTemplateModule::emit_writes(
    EmulatedTemplate& tmpl, VkDescriptorSet descriptorSet, const void* pData, Emit emit) {
    std::lock_guard<std::mutex> lock(tmpl.updateMutex);
    for (auto& storage : tmpl.entries) {
        const auto& entry = storage.info;
        if (entry.descriptorCount == 0) continue;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = entry.dstBinding;
        write.dstArrayElement = entry.dstArrayElement;
        write.descriptorCount = entry.descriptorCount;
        write.descriptorType = entry.descriptorType;

        // A single write preserves Vulkan's consecutive-binding semantics.
        // Gather strided data into typed storage valid for the driver call.
        const char* entryPtr = static_cast<const char*>(pData) + entry.offset;
        switch (entry.descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            case VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM:
            case VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM:
                write.pImageInfo = descriptor_infos(entryPtr, entry.descriptorCount, entry.stride, storage.images);
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                write.pBufferInfo = descriptor_infos(entryPtr, entry.descriptorCount, entry.stride, storage.buffers);
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                write.pTexelBufferView = descriptor_infos(entryPtr, entry.descriptorCount, entry.stride, storage.views);
                break;
            case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
                storage.inlineBlock.dataSize = entry.descriptorCount;
                storage.inlineBlock.pData = entryPtr;
                write.pNext = &storage.inlineBlock;
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                storage.accelerationInfo.accelerationStructureCount = entry.descriptorCount;
                storage.accelerationInfo.pAccelerationStructures = descriptor_infos(entryPtr, entry.descriptorCount, entry.stride, storage.acceleration);
                write.pNext = &storage.accelerationInfo;
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
                storage.accelerationNVInfo.accelerationStructureCount = entry.descriptorCount;
                storage.accelerationNVInfo.pAccelerationStructures = descriptor_infos(entryPtr, entry.descriptorCount, entry.stride, storage.accelerationNV);
                write.pNext = &storage.accelerationNVInfo;
                break;
            default:
                break;
        }
        emit(write);
    }
}

bool DescriptorUpdateTemplateModule::push_emulated_template(VkDevice device, VkCommandBuffer command,
    VkDescriptorUpdateTemplate handle, VkPipelineLayout layout, uint32_t set, const void* data,
    PFN_vkCmdPushDescriptorSetKHR push) {
    std::shared_ptr<EmulatedTemplate> tmpl;
    {
        std::lock_guard<std::mutex> lock(m_template_mutex);
        auto found = m_templates.find((uint64_t)handle);
        if (found == m_templates.end() || found->second->device != device) return false;
        tmpl = found->second;
    }
    if (!data || !push) return true;
    emit_writes(*tmpl, VK_NULL_HANDLE, data, [&](const VkWriteDescriptorSet& write) {
        push(command, tmpl->point, layout, set, 1, &write);
    });
    return true;
}

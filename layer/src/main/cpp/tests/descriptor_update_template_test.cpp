#include "modules/vulkan_1_1/descriptor_update_template.h"
#include "core/layer_manager.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
unsigned failures = 0, calls = 0, invalid = 0;
std::vector<uint64_t> values;
void check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
VKAPI_ATTR void VKAPI_CALL update(VkDevice, uint32_t count, const VkWriteDescriptorSet* writes,
                                  uint32_t, const VkCopyDescriptorSet*) {
    ++calls;
    for (uint32_t w = 0; w < count; ++w) {
        const auto& write = writes[w];
        // Binding 0 has two elements and binding 1 is its compatible successor.
        // The driver's consecutive-binding rule applies inside a single write;
        // starting a separate write at array element 2 of binding 0 is invalid.
        if (write.dstBinding == 0 && write.dstArrayElement >= 2) ++invalid;
        for (uint32_t i = 0; i < write.descriptorCount; ++i) {
            switch (write.descriptorType) {
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    values.push_back(uint64_t(write.pImageInfo[i].imageView)); break;
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    values.push_back(uint64_t(write.pBufferInfo[i].buffer)); break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    values.push_back(uint64_t(write.pTexelBufferView[i])); break;
                default: ++invalid; break;
            }
        }
    }
}
struct Element {
    VkDescriptorBufferInfo buffer;
    uint64_t padding;
    VkDescriptorImageInfo image;
    VkBufferView view;
};
}

void LayerManager::register_module_factory(ModuleFactory) {}
extern "C" VkInstance get_last_instance() { return VK_NULL_HANDLE; }
extern "C" void* get_real_proc(VkInstance, VkDevice, const char* name) {
    return std::strcmp(name, "vkUpdateDescriptorSets") == 0 ? (void*)update : nullptr;
}

int main() {
    DescriptorUpdateTemplateModule module;
    const auto a = reinterpret_cast<VkDevice>(uintptr_t(0x123));
    const auto b = reinterpret_cast<VkDevice>(uintptr_t(0x456));
    std::array<Element, 3> data{};
    for (size_t i = 0; i < data.size(); ++i) {
        data[i].buffer = {VkBuffer(0x100ULL + i), i * 64, 64};
        data[i].image = {VkSampler(0x200ULL + i), VkImageView(0x300ULL + i), VK_IMAGE_LAYOUT_GENERAL};
        data[i].view = VkBufferView(0x400ULL + i);
    }
    VkDescriptorUpdateTemplateEntry entries[] = {
        {0, 0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, offsetof(Element, buffer), sizeof(Element)},
        {2, 0, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, offsetof(Element, image), sizeof(Element)},
        {3, 0, 3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, offsetof(Element, view), sizeof(Element)}
    };
    VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    info.descriptorUpdateEntryCount = 3;
    info.pDescriptorUpdateEntries = entries;
    auto create = [&](VkDevice d) {
        VkDescriptorUpdateTemplate handle = VK_NULL_HANDLE;
        VkResult result = VK_ERROR_UNKNOWN;
        bool handled = module.on_create_descriptor_update_template(d, &info, nullptr, &handle, result);
        check(handled && result == VK_SUCCESS && handle != VK_NULL_HANDLE, "create emulated template");
        return handle;
    };
    auto apply = [&](VkDevice d, VkDescriptorUpdateTemplate handle) {
        calls = invalid = 0;
        values.clear();
        module.on_update_descriptor_set_with_template(d, VkDescriptorSet(0x789ULL), handle, data.data());
    };
    const auto ta = create(a), tb = create(b);
    apply(a, ta);
    check(invalid == 0, "template array spanning compatible bindings uses valid destination indices");
    check(calls == 3, "nine strided descriptors require three driver calls instead of nine");
    check(values == std::vector<uint64_t>{0x100, 0x101, 0x102, 0x300, 0x301, 0x302, 0x400, 0x401, 0x402},
          "gathered buffer, image and texel descriptors preserve stride and order");
    module.on_destroy_device(a);
    apply(b, tb);
    check(values.size() == 9 && calls == 3, "destroying one device preserves the other device's live template");
    apply(a, ta);
    check(calls == 0, "device destruction retires its own template");
    module.on_destroy_descriptor_update_template(a, tb, nullptr);
    apply(b, tb);
    check(values.size() == 9, "another device cannot retire this device's template");
    module.on_destroy_descriptor_update_template(b, tb, nullptr);
    apply(b, tb);
    check(calls == 0, "template destruction retires the stored metadata");
    entries[0].stride = 0;
    info.descriptorUpdateEntryCount = 1;
    const auto repeated = create(b);
    apply(b, repeated);
    check(values == std::vector<uint64_t>{0x100, 0x100, 0x100}, "zero stride repeats the same source descriptor");
    module.on_destroy_descriptor_update_template(b, repeated, nullptr);
    module.on_destroy_device(b);
    std::printf("Descriptor template regressions: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}

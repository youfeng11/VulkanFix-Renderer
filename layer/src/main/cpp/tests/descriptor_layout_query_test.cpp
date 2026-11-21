#include "core/descriptor_layout_query.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace { bool fail_allocation = false; unsigned failures = 0;
void check(bool value, const char* message) { printf("%s: %s\n", value ? "PASS" : "FAIL", message); failures += !value; }
}
void* operator new(size_t size) {
    if (fail_allocation) throw std::bad_alloc();
    if (auto p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
int main() {
    VkSampler immutable = (VkSampler)uint64_t(0x123400006789);
    VkDescriptorSetLayoutBinding bindings[] = {
        {17, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 0, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &immutable},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
    };
    VkDescriptorBindingFlags values[] = {VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, nullptr, 3, values};
    VkDescriptorSetLayoutCreateInfo input{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags, 0, 3, bindings};
    vkfix::DescriptorLayoutQuery query;
    check(query.prepare(input, true) == VK_SUCCESS && query.info().pBindings[0].descriptorCount == 4 &&
          query.info().pBindings[2].descriptorCount == 0 &&
          query.info().pBindings[2].stageFlags == (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT),
          "zero variable inline block is queried as four bytes, while an unused fixed binding stays empty");
    check(query.info().pBindings[0].binding == 17 && query.info().pBindings[1].pImmutableSamplers == &immutable &&
          query.info().pNext == &flags && flags.pBindingFlags == values &&
          !bindings[0].descriptorCount && bindings[2].stageFlags == VK_SHADER_STAGE_VERTEX_BIT,
          "unsorted bindings, shared immutable samplers, flags and original fields are preserved");
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    check(query.prepare(input, false) == VK_SUCCESS && query.info().pBindings[0].descriptorCount == 1,
          "zero variable ordinary binding is queried as one descriptor");
    values[0] = 0;
    fail_allocation = true;
    auto ordinary = query.prepare(input, false);
    fail_allocation = false;
    check(ordinary == VK_SUCCESS && query.info().pBindings == bindings && !bindings[0].descriptorCount,
          "fixed zero-count layout requires no allocation and keeps its definition");
    vkfix::DescriptorLayoutQuery fresh;
    values[0] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    fail_allocation = true;
    auto oom = fresh.prepare(input, false);
    fail_allocation = false;
    check(oom == VK_ERROR_OUT_OF_HOST_MEMORY && !bindings[0].descriptorCount && values[0] == VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
          "preparation OOM leaves application binding counts and flags unchanged");
    printf("Descriptor layout query failures: %u\n", failures);
    return failures ? 1 : 0;
}

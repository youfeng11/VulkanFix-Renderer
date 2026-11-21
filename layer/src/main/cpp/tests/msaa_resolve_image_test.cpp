#include "core/msaa_resolve_image.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#include <map>
#include <limits>
namespace {
unsigned failures = 0;
void check(bool value,const char* description) { std::printf("%s: %s\n",value ? "PASS" : "FAIL",description); if (!value) ++failures; }
unsigned calls = 0;
VkResult original_result = VK_SUCCESS, candidate_result = VK_SUCCESS;
VkImageFormatProperties original{}, candidate{};
std::vector<VkImageUsageFlags> queried_usages;
VkImageCreateFlags expected_flags = 0;
VKAPI_ATTR VkResult VKAPI_CALL query(VkPhysicalDevice physical,VkFormat format,VkImageType type,
    VkImageTiling tiling,VkImageUsageFlags usage,VkImageCreateFlags flags,VkImageFormatProperties* out) {
    check(physical == reinterpret_cast<VkPhysicalDevice>(uintptr_t(0x100)) && format == VK_FORMAT_R32G32B32A32_SFLOAT &&
        type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL && flags == expected_flags,
        "permission preparation queries the owning native device and preserves creation flags");
    queried_usages.push_back(usage);
    *out = calls ? candidate : original;
    return calls++ ? candidate_result : original_result;
}
void reset() {
    calls = 0; queried_usages.clear(); original_result = candidate_result = VK_SUCCESS; expected_flags = 0;
    original = {{4096,4096,1},12,256,VK_SAMPLE_COUNT_1_BIT|VK_SAMPLE_COUNT_2_BIT|VK_SAMPLE_COUNT_4_BIT,1ull<<32};
    candidate = original;
}
struct Image { VkMemoryRequirements requirements; };
std::map<VkImage,Image> live_images;
unsigned create_calls = 0, destroy_calls = 0, invalid_destroys = 0, requirement_calls = 0;
unsigned fail_at = std::numeric_limits<unsigned>::max();
VkResult creation_error = VK_ERROR_OUT_OF_HOST_MEMORY;
uint32_t original_types = 3, augmented_types = 3;
uintptr_t serial = 0x1000;
const VkAllocationCallbacks* expected_allocator = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice,const VkImageCreateInfo* info,const VkAllocationCallbacks* allocator,VkImage* image) {
    check(allocator == expected_allocator,"private image creation preserves the requested allocation callbacks");
    if (create_calls++ == fail_at) { *image = (VkImage)uintptr_t(0xdead); return creation_error; }
    *image = (VkImage)++serial;
    const bool transient = info->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    live_images[*image] = {{transient ? 4096u : 8192u,256,transient ? original_types : augmented_types}};
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice,VkImage image,const VkAllocationCallbacks* allocator) {
    check(allocator == expected_allocator,"temporary image destruction uses the matching allocation callbacks");
    ++destroy_calls; if (!live_images.erase(image)) ++invalid_destroys;
}
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice,VkImage image,VkMemoryRequirements* out) {
    ++requirement_calls; *out = live_images.at(image).requirements;
}
void reset_images() {
    check(live_images.empty(),"previous image creation transaction leaves no temporary native objects");
    create_calls = destroy_calls = invalid_destroys = requirement_calls = 0;
    fail_at = std::numeric_limits<unsigned>::max(); original_types = augmented_types = 3;
}
}
int main() {
    const auto physical = reinterpret_cast<VkPhysicalDevice>(uintptr_t(0x100));
    uint32_t families[]{2,5};
    VkImageCreateInfo source{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    source.imageType = VK_IMAGE_TYPE_2D; source.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    source.extent = {64,32,1}; source.mipLevels = 1; source.arrayLayers = 8;
    source.samples = VK_SAMPLE_COUNT_4_BIT; source.tiling = VK_IMAGE_TILING_OPTIMAL;
    source.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    source.sharingMode = VK_SHARING_MODE_CONCURRENT; source.queueFamilyIndexCount = 2; source.pQueueFamilyIndices = families;
    std::array<unsigned char,sizeof(source)> saved{}; std::memcpy(saved.data(),&source,sizeof(source));
    VkImageCreateInfo native{};
    reset();
    check(vkfix::prepare_msaa_resolve_image(physical,query,source,native) == VK_SUCCESS && calls == 2 &&
        native.usage == (source.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && native.pQueueFamilyIndices == families &&
        native.queueFamilyIndexCount == 2 && !std::memcmp(saved.data(),&source,sizeof(source)),
        "MSAA source gains validated private transfer access without changing caller storage");
    auto target = source; target.samples = VK_SAMPLE_COUNT_1_BIT; target.mipLevels = 3; target.arrayLayers = 1;
    reset();
    check(vkfix::prepare_msaa_resolve_image(physical,query,target,native) == VK_SUCCESS && calls == 2 &&
        native.usage == (target.usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT) && native.mipLevels == 3,
        "single-sample resolve targets retain their mip chain and gain destination permission");
    for (auto samples : {VK_SAMPLE_COUNT_1_BIT,VK_SAMPLE_COUNT_4_BIT}) {
        auto transient = source; transient.samples = samples; transient.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        reset();
        check(vkfix::prepare_msaa_resolve_image(physical,query,transient,native) == VK_SUCCESS &&
            !(native.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) &&
            (native.usage & (samples == VK_SAMPLE_COUNT_1_BIT ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) &&
            (transient.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) &&
            queried_usages[0] == transient.usage && queried_usages[1] == native.usage,
            "transient transfer candidates are prepared for a separate native memory-class comparison");
    }
    auto flagged = source; flagged.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_ALIAS_BIT;
    reset(); expected_flags = flagged.flags;
    check(vkfix::prepare_msaa_resolve_image(physical,query,flagged,native) == VK_SUCCESS && native.flags == flagged.flags,
        "native permission validation retains mutable and alias creation semantics");
    for (unsigned limit = 0; limit < 7; ++limit) {
        reset();
        switch (limit) {
        case 0: candidate.sampleCounts = VK_SAMPLE_COUNT_1_BIT; break;
        case 1: candidate.maxExtent.width = 63; break;
        case 2: candidate.maxExtent.height = 31; break;
        case 3: candidate.maxExtent.depth = 0; break;
        case 4: candidate.maxMipLevels = 0; break;
        case 5: candidate.maxArrayLayers = 7; break;
        case 6: candidate.maxResourceSize = original.maxResourceSize - 1; break;
        }
        check(vkfix::prepare_msaa_resolve_image(physical,query,source,native) == VK_SUCCESS && native.usage == source.usage,
            "augmentation never exceeds the requested image shape or reduces its native resource-size allowance");
    }
    reset(); candidate_result = VK_ERROR_FORMAT_NOT_SUPPORTED;
    check(vkfix::prepare_msaa_resolve_image(physical,query,source,native) == VK_SUCCESS && native.usage == source.usage,
        "unsupported native usage combinations preserve the original request for another lowering");
    for (auto result : {VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_OUT_OF_DEVICE_MEMORY}) for (bool second : {false,true}) {
        reset(); if (second) candidate_result = result; else original_result = result;
        check(vkfix::prepare_msaa_resolve_image(physical,query,source,native) == result && native.usage == source.usage,
            "native preparation allocation failures propagate before creating an image with missing permissions");
    }
    std::vector<VkImageCreateInfo> untouched;
    untouched.push_back(source); untouched.back().usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    untouched.push_back(source); untouched.back().format = VK_FORMAT_R8G8B8A8_UNORM;
    untouched.push_back(source); untouched.back().samples = VK_SAMPLE_COUNT_2_BIT;
    untouched.push_back(source); untouched.back().arrayLayers = 1;
    untouched.push_back(source); untouched.back().usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    untouched.push_back(source); untouched.back().pNext = &external;
    for (const auto& info : untouched) {
        reset();
        check(vkfix::prepare_msaa_resolve_image(physical,query,info,native) == VK_SUCCESS && !calls &&
            native.usage == info.usage && native.pNext == info.pNext,
            "unaffected or extended image requests retain their exact native contract");
    }
    reset();
    check(vkfix::prepare_msaa_resolve_image(physical,nullptr,source,native) == VK_SUCCESS && !calls && native.usage == source.usage,
        "missing native capability queries do not invent support");
    VkAllocationCallbacks allocator{}; expected_allocator = &allocator;
    const auto device = reinterpret_cast<VkDevice>(uintptr_t(0x200));
    auto transient = source; transient.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    for (bool same_class : {true,false}) {
        reset(); reset_images();
        if (!same_class) original_types = 7; // Original lazy-compatible class differs from non-transient backing.
        vkfix::prepare_msaa_resolve_image(physical,query,transient,native);
        VkImage image{};
        check(vkfix::create_msaa_resolve_image(device,transient,native,&allocator,&image,create_image,destroy_image,requirements) == VK_SUCCESS &&
            create_calls == 2 && destroy_calls == 1 && requirement_calls == 2 && live_images.size() == 1 &&
            live_images.at(image).requirements.memoryTypeBits == original_types &&
            bool(native.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) == !same_class,
            "native backing is selected only when it preserves the public transient memoryTypeBits class");
        destroy_image(device,image,&allocator);
        check(live_images.empty() && !invalid_destroys,"both memory-class outcomes destroy each temporary image exactly once");
    }
    for (auto error : {VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_OUT_OF_DEVICE_MEMORY}) for (unsigned failed : {0u,1u}) {
        reset(); reset_images(); creation_error = error; fail_at = failed;
        vkfix::prepare_msaa_resolve_image(physical,query,transient,native);
        VkImage image = (VkImage)uintptr_t(0xbeef);
        check(vkfix::create_msaa_resolve_image(device,transient,native,&allocator,&image,create_image,destroy_image,requirements) == error &&
            !image && live_images.empty() && destroy_calls == failed && !invalid_destroys,
            "native creation OOM rolls back valid objects, clears the output and never destroys undefined failed handles");
    }
    reset(); reset_images(); vkfix::prepare_msaa_resolve_image(physical,query,transient,native);
    VkImage image{};
    check(vkfix::create_msaa_resolve_image(device,transient,native,&allocator,&image,create_image,destroy_image,nullptr) == VK_SUCCESS &&
        native.usage == transient.usage && create_calls == 1 && !requirement_calls && !destroy_calls,
        "without memory-class queries transient creation retains its original contract");
    destroy_image(device,image,&allocator); reset_images();
    reset(); vkfix::prepare_msaa_resolve_image(physical,query,source,native);
    check(vkfix::create_msaa_resolve_image(device,source,native,&allocator,&image,create_image,destroy_image,requirements) == VK_SUCCESS &&
        create_calls == 1 && !requirement_calls && !destroy_calls && (native.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
        "ordinary images need one native creation and retain the native non-transient memory class");
    destroy_image(device,image,&allocator);
    check(live_images.empty() && !invalid_destroys,"all prepared image transactions release native objects");
    std::printf("MSAA image permission failures: %u\n",failures); return failures ? 1 : 0;
}

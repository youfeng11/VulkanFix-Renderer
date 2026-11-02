#include "core/multiview_attachments.h"
#include "core/driver_loader.h"
#include <cstdio>
#include <map>
#include <set>
#include <cstring>

using namespace vkfix;
namespace {
template<class T> T handle(uint64_t n) { return (T)(uintptr_t)n; }
unsigned failures = 0, next_handle = 0x10000, conversion_creates = 0;
bool fail_framebuffer = false;
struct View { VkDevice device; VkImageViewCreateInfo info; VkImageUsageFlags usage; };
struct Frame { VkDevice device; VkRenderPass pass; uint32_t width, height, layers; std::vector<VkImageView> views; };
std::map<VkImageView, View> views;
std::map<VkFramebuffer, Frame> frames;
std::set<VkRenderPass> passes;
std::set<VkSamplerYcbcrConversion> conversions;
VkSamplerYcbcrModelConversion last_model{};
void check(bool okay, const char* text) { std::printf("%s: %s\n", okay ? "PASS" : "FAIL", text); failures += !okay; }
VKAPI_ATTR VkResult VKAPI_CALL create_pass(VkDevice, const VkRenderPassCreateInfo* ci,
    const VkAllocationCallbacks* allocator, VkRenderPass* out) {
    if (allocator) return VK_ERROR_UNKNOWN;
    for (auto p = static_cast<const VkBaseInStructure*>(ci->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) return VK_ERROR_FEATURE_NOT_PRESENT;
    *out = handle<VkRenderPass>(++next_handle); passes.insert(*out); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_pass(VkDevice, VkRenderPass pass, const VkAllocationCallbacks*) { passes.erase(pass); }
VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice device, const VkImageViewCreateInfo* ci,
    const VkAllocationCallbacks* allocator, VkImageView* out) {
    if (allocator) return VK_ERROR_UNKNOWN;
    VkImageUsageFlags usage = 0;
    for (auto p = static_cast<const VkBaseInStructure*>(ci->pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO)
            usage = reinterpret_cast<const VkImageViewUsageCreateInfo*>(p)->usage;
        if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO &&
            !conversions.count(reinterpret_cast<const VkSamplerYcbcrConversionInfo*>(p)->conversion)) return VK_ERROR_UNKNOWN;
    }
    *out = handle<VkImageView>(++next_handle);
    views[*out] = {device, *ci, usage}; views[*out].info.pNext = nullptr; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice, VkImageView view, const VkAllocationCallbacks*) {
    for (const auto& fb : frames) for (auto attached : fb.second.views)
        if (attached == view) { check(false, "destroy framebuffer before its slice view"); }
    check(views.erase(view) == 1, "destroy each native slice once");
}
VKAPI_ATTR VkResult VKAPI_CALL create_frame(VkDevice device, const VkFramebufferCreateInfo* ci,
    const VkAllocationCallbacks* allocator, VkFramebuffer* out) {
    if (allocator || !passes.count(ci->renderPass)) return VK_ERROR_UNKNOWN;
    if (fail_framebuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = handle<VkFramebuffer>(++next_handle);
    Frame record{device, ci->renderPass, ci->width, ci->height, ci->layers, {}};
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
        if (!views.count(ci->pAttachments[i])) return VK_ERROR_UNKNOWN;
        record.views.push_back(ci->pAttachments[i]);
    }
    frames[*out] = std::move(record); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_frame(VkDevice, VkFramebuffer fb, const VkAllocationCallbacks*) {
    check(frames.erase(fb) == 1, "destroy each internal framebuffer once");
}
VKAPI_ATTR VkResult VKAPI_CALL create_conversion(VkDevice, const VkSamplerYcbcrConversionCreateInfo* ci,
    const VkAllocationCallbacks* allocator, VkSamplerYcbcrConversion* out) {
    if (allocator || ci->pNext) return VK_ERROR_UNKNOWN;
    last_model = ci->ycbcrModel; ++conversion_creates;
    *out = handle<VkSamplerYcbcrConversion>(++next_handle); conversions.insert(*out); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_conversion(VkDevice, VkSamplerYcbcrConversion conversion, const VkAllocationCallbacks*) {
    conversions.erase(conversion);
}
}
extern "C" VkInstance get_last_instance() { return handle<VkInstance>(1); }
extern "C" void* get_real_proc(VkInstance, VkDevice, const char* name) {
#define PROC(n, f) if (!std::strcmp(name, n)) return reinterpret_cast<void*>(f)
    PROC("vkCreateRenderPass", create_pass); PROC("vkDestroyRenderPass", destroy_pass);
    PROC("vkCreateImageView", create_view); PROC("vkDestroyImageView", destroy_view);
    PROC("vkCreateFramebuffer", create_frame); PROC("vkDestroyFramebuffer", destroy_frame);
    PROC("vkCreateSamplerYcbcrConversion", create_conversion); PROC("vkDestroySamplerYcbcrConversion", destroy_conversion);
#undef PROC
    return nullptr;
}
int main() {
    auto& registry = MultiviewAttachments::get();
    const auto device = handle<VkDevice>(0x100), other = handle<VkDevice>(0x200);
    const auto image = handle<VkImage>(0x100000010ull);
    const auto unused = handle<VkImageView>(0x100000020ull), used = handle<VkImageView>(0x200000020ull);
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D; ici.arrayLayers = 10; ici.mipLevels = 3; ici.extent = {64, 32, 1};
    registry.remember_image(device, image, ici);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 2, VK_REMAINING_MIP_LEVELS, 2, VK_REMAINING_ARRAY_LAYERS};
    registry.remember_view(device, unused, vci); registry.remember_view(device, used, vci);
    auto original_used = registry.acquire_view(device, used);
    VkImageView slice{};
    check(original_used->slice(4, 0, slice) == VK_SUCCESS &&
        views.at(slice).info.subresourceRange.baseArrayLayer == 6 &&
        views.at(slice).info.subresourceRange.layerCount == 1 &&
        views.at(slice).info.subresourceRange.levelCount == 1, "resolve remaining ranges and add sparse view index to base layer");
    const auto original_slice = slice;
    std::vector<VkImageMemoryBarrier> transitions;
    check(original_used->layout_barriers((1u << 0) | (1u << 2) | (1u << 3), true,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transitions) == VK_SUCCESS && transitions.size() == 2 &&
        transitions[0].image == image && transitions[0].subresourceRange.baseMipLevel == 2 &&
        transitions[0].subresourceRange.baseArrayLayer == 2 && transitions[0].subresourceRange.layerCount == 1 &&
        transitions[1].subresourceRange.baseArrayLayer == 4 && transitions[1].subresourceRange.layerCount == 2 &&
        transitions[0].oldLayout == VK_IMAGE_LAYOUT_GENERAL && transitions[0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        transitions[0].srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED,
        "unused sparse view transitions preserve the base mip/layer and group only consecutive active views");
    transitions.clear();
    original_used->layout_barriers((1u << 2) | (1u << 3), false, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transitions);
    original_used->layout_barriers((1u << 3) | (1u << 4), false, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transitions);
    original_used->layout_barriers((1u << 0) | (1u << 1), false, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transitions);
    check(transitions.size() == 1 && transitions[0].subresourceRange.baseArrayLayer == 2 && transitions[0].subresourceRange.layerCount == 5,
        "overlapping and adjacent alias ranges are joined into exactly one transition");
    original_used->layout_barriers(1u << 7, false, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transitions);
    original_used->layout_barriers(1u << 2, false, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transitions);
    check(transitions.size() == 3 && transitions[1].subresourceRange.baseArrayLayer == 9 &&
        transitions[2].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED,
        "transition merging preserves inactive layer holes and separate UNDEFINED discard semantics");
    check(original_used->slice(4, 0, slice) == VK_SUCCESS && slice == original_slice, "cache same native slice");
    check(original_used->slice(8, 0, slice) == VK_ERROR_FEATURE_NOT_PRESENT, "reject out-of-range layer");

    VkAttachmentDescription descriptions[2]{};
    for (auto& a : descriptions) { a.format = vci.format; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; }
    VkAttachmentReference color{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
    uint32_t mask = (1u << 2) | (1u << 4);
    VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO}; mv.subpassCount = 1; mv.pViewMasks = &mask;
    VkRenderPassCreateInfo pci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; pci.pNext = &mv; pci.attachmentCount = 2; pci.pAttachments = descriptions; pci.subpassCount = 1; pci.pSubpasses = &subpass;
    VkRenderPass public_pass{}, compatible{}; VkResult status{};
    auto& render_passes = MultiviewRenderPasses::get();
    check(render_passes.create_fallback(device, &pci, nullptr, &public_pass, true, status) && status == VK_SUCCESS, "capture multiview pass");
    check(render_passes.create_fallback(device, &pci, nullptr, &compatible, true, status) && status == VK_SUCCESS, "capture compatible active pass");
    VkImageView attachments[] = {unused, used};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fci.renderPass = public_pass;
    fci.width = 16; fci.height = 8; fci.layers = 1; fci.attachmentCount = 2; fci.pAttachments = attachments;
    const auto public_fb = handle<VkFramebuffer>(0x500);
    registry.remember_framebuffer(device, public_fb, fci);
    auto retained_fb = registry.acquire_framebuffer(device, public_fb);
    fci.width = 99; attachments[1] = unused;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = compatible; begin.framebuffer = public_fb;
    VkFramebuffer native{};
    check(registry.native_framebuffer(device, begin, 0, 4, true, true, true, native) == VK_SUCCESS &&
        frames.at(native).width == 16 && frames.at(native).height == 8 && frames.at(native).layers == 1 &&
        frames.at(native).views == std::vector<VkImageView>{original_slice}, "own framebuffer CI and compact original attachment indices");
    auto cached = native;
    check(registry.native_framebuffer(device, begin, 0, 4, true, true, true, native) == VK_SUCCESS && native == cached, "reuse same framebuffer chunk");
    registry.forget_view(device, unused);
    check(frames.count(cached) == 1, "destroying unused view keeps live chunk framebuffer");
    begin.renderPass = public_pass;
    check(registry.native_framebuffer(device, begin, 0, 4, true, true, true, native) == VK_SUCCESS && native != cached &&
        frames.at(native).pass != frames.at(cached).pass, "use active compatible pass identity in native cache");
    registry.forget_view(device, used);
    check(frames.empty() && views.empty(), "invalidate dependent framebuffers before native slices");
    registry.remember_view(device, used, vci);
    check(registry.acquire_view(device, used) != original_used, "reused handle gets a new resource identity");
    check(registry.native_framebuffer(device, begin, 0, 4, true, true, true, native) == VK_ERROR_FEATURE_NOT_PRESENT,
        "old framebuffer snapshot cannot attach replacement view with same handle");

    const auto imaginary = handle<VkFramebuffer>(0x600);
    fci.width = 16; fci.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT; fci.pAttachments = nullptr;
    registry.remember_framebuffer(device, imaginary, fci);
    VkImageView bound[] = {used, used};
    VkRenderPassAttachmentBeginInfo attachment_begin{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO}; attachment_begin.attachmentCount = 2; attachment_begin.pAttachments = bound;
    begin.framebuffer = imaginary; begin.pNext = &attachment_begin;
    fail_framebuffer = true;
    check(registry.native_framebuffer(device, begin, 0, 2, true, true, true, native) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !native,
        "propagate framebuffer creation failure without caching handle");
    fail_framebuffer = false;
    check(registry.native_framebuffer(device, begin, 0, 2, true, true, true, native) == VK_SUCCESS, "retry and resolve imageless attachment views at begin");
    registry.forget_framebuffer(device, imaginary);
    check(frames.empty(), "release imageless internal framebuffers on public destruction");

    registry.remember_image(other, image, ici); registry.remember_view(other, used, vci);
    auto other_used = registry.acquire_view(other, used);
    check(other_used != registry.acquire_view(device, used) && other_used->slice(2, 0, slice) == VK_SUCCESS &&
        views.at(slice).device == other, "qualify identical full-width image/view handles by device");

    const auto ds = handle<VkImageView>(0x710);
    vci.format = VK_FORMAT_D24_UNORM_S8_UINT; vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    registry.remember_view(device, ds, vci);
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    const auto depth_only = handle<VkImageView>(0x711); registry.remember_view(device, depth_only, vci);
    transitions.clear();
    check(registry.acquire_view(device, depth_only)->layout_barriers(1, false, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, transitions) == VK_SUCCESS && transitions.size() == 1 &&
        transitions[0].subresourceRange.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) &&
        (transitions[0].srcAccessMask & VK_ACCESS_MEMORY_WRITE_BIT),
        "Core depth/stencil discard transitions cover both aspects and synchronize earlier writes, including depth-only views");
    check(registry.acquire_view(device, ds)->slice(2, VK_IMAGE_ASPECT_STENCIL_BIT, slice) == VK_SUCCESS &&
        views.at(slice).info.subresourceRange.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT, "select stencil-only input attachment slice");

    const auto converted = handle<VkImageView>(0x720); const auto conversion = handle<VkSamplerYcbcrConversion>(0x730);
    VkSamplerYcbcrConversionCreateInfo cci{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO}; cci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    registry.remember_conversion(device, conversion, cci);
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO}; usage.usage = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkSamplerYcbcrConversionInfo converted_info{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO}; converted_info.conversion = conversion; converted_info.pNext = &usage;
    vci.pNext = &converted_info; vci.format = VK_FORMAT_R8G8B8A8_UNORM; vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    registry.remember_view(device, converted, vci);
    registry.forget_conversion(device, conversion); cci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY; usage.usage = 0;
    check(registry.acquire_view(device, converted)->slice(2, 0, slice) == VK_SUCCESS && conversion_creates == 1 &&
        last_model == VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709 && conversions.empty() &&
        views.at(slice).usage == VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, "own usage and conversion parameters beyond original conversion lifetime");

    const auto swapchain = handle<VkSwapchainKHR>(0x800);
    const auto swap_image = handle<VkImage>(0x810);
    const auto swap_view = handle<VkImageView>(0x820);
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.imageArrayLayers = 6;
    registry.remember_swapchain(device, swapchain, sci); registry.remember_swapchain_images(device, swapchain, 1, &swap_image);
    vci.pNext = nullptr; vci.image = swap_image; vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 1, VK_REMAINING_ARRAY_LAYERS};
    registry.remember_view(device, swap_view, vci);
    auto swap_record = registry.acquire_view(device, swap_view);
    const auto image_identity = swap_record->image();
    registry.remember_swapchain_images(device, swapchain, 1, &swap_image);
    check(swap_record->slice(4, 0, slice) == VK_SUCCESS && image_identity == swap_record->image() &&
        views.at(slice).info.subresourceRange.baseArrayLayer == 5, "track swapchain array layers and preserve identity across image enumeration");
    registry.forget_swapchain(device, swapchain);
    check(swap_record->slice(0, 0, slice) == VK_ERROR_FEATURE_NOT_PRESENT, "retire swapchain slices before native swapchain destruction");

    const auto volume = handle<VkImage>(0x900);
    const auto volume_view = handle<VkImageView>(0x910);
    ici.imageType = VK_IMAGE_TYPE_3D; ici.extent.depth = 16; ici.mipLevels = 4; ici.arrayLayers = 1;
    registry.remember_image(device, volume, ici);
    vci.image = volume; vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 1, 1, VK_REMAINING_ARRAY_LAYERS};
    registry.remember_view(device, volume_view, vci);
    transitions.clear();
    check(registry.acquire_view(device, volume_view)->layout_barriers((1u << 0) | (1u << 2), false,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transitions) == VK_SUCCESS && transitions.size() == 1 &&
        transitions[0].subresourceRange.baseMipLevel == 2 && transitions[0].subresourceRange.levelCount == 1 &&
        transitions[0].subresourceRange.baseArrayLayer == 0 && transitions[0].subresourceRange.layerCount == 1,
        "wholly unused 3D attachments transition the entire mip once rather than depth slices");
    transitions.clear();
    check(registry.acquire_view(device, volume_view)->layout_barriers(1, true,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transitions) == VK_SUCCESS && transitions.empty(),
        "referenced 3D attachments do not repeat their whole-mip transition for unused views");
    check(registry.acquire_view(device, volume_view)->slice(2, 0, slice) == VK_SUCCESS &&
        views.at(slice).info.subresourceRange.baseArrayLayer == 3 &&
        registry.acquire_view(device, volume_view)->slice(3, 0, slice) == VK_ERROR_FEATURE_NOT_PRESENT,
        "2D view of a 3D image uses selected mip depth for remaining layers");
    registry.forget_image(device, volume);
    check(registry.acquire_view(device, volume_view)->slice(0, 0, slice) == VK_ERROR_FEATURE_NOT_PRESENT,
        "image destruction retires retained view slices");

    const auto extended_view = handle<VkImageView>(0x920);
    VkImageViewASTCDecodeModeEXT decode{VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT};
    vci.image = image; vci.pNext = &decode;
    registry.remember_view(device, extended_view, vci);
    check(registry.acquire_view(device, extended_view)->slice(0, 0, slice) == VK_ERROR_FEATURE_NOT_PRESENT,
        "refuse unsupported extension semantics rather than dropping pNext");

    const auto empty_fb = handle<VkFramebuffer>(0x930);
    VkRenderPass empty_pass{};
    pci.attachmentCount = 0; pci.pAttachments = nullptr; subpass.colorAttachmentCount = 0; subpass.pColorAttachments = nullptr;
    check(render_passes.create_fallback(device, &pci, nullptr, &empty_pass, true, status) && status == VK_SUCCESS,
        "capture attachmentless multiview pass");
    fci.renderPass = empty_pass; fci.attachmentCount = 0; fci.pAttachments = nullptr;
    registry.remember_framebuffer(device, empty_fb, fci);
    begin.renderPass = empty_pass; begin.framebuffer = empty_fb; begin.pNext = nullptr;
    check(registry.native_framebuffer(device, begin, 0, 2, true, true, true, native) == VK_SUCCESS &&
        frames.at(native).views.empty(), "attachmentless imageless pass needs no attachment-begin structure");

    registry.forget_framebuffer(device, public_fb);
    check(retained_fb->native_framebuffer(render_passes.acquire(device, public_pass), 0, 4, true, true, true,
        retained_fb->attachments(), native) == VK_ERROR_FEATURE_NOT_PRESENT, "retained metadata cannot resurrect destroyed public framebuffer");
    registry.forget_device(device); registry.forget_device(other);
    check(views.empty() && frames.empty(), "device teardown releases retained slice resources");
    check(other_used->slice(0, 0, slice) == VK_ERROR_FEATURE_NOT_PRESENT, "retained view cannot create after device teardown");
    render_passes.forget_device(device); destroy_pass(device, public_pass, nullptr); destroy_pass(device, compatible, nullptr);
    destroy_pass(device, empty_pass, nullptr);
    check(passes.empty(), "release private and public test render passes");
    std::printf("%u failures\n", failures); return failures ? 1 : 0;
}

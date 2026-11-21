#include "core/singleton_device_group.h"
#include "core/vk_pnext.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

static std::atomic<bool> fail_new{false};
void* operator new(size_t bytes) {
    if (fail_new.exchange(false)) throw std::bad_alloc();
    if (auto* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,size_t) noexcept { std::free(p); }
using namespace vkfix;
namespace {
unsigned failures = 0;
void check(bool okay,const char* name) { printf("%s: %s\n",okay ? "PASS" : "FAIL",name); failures += !okay; }
template<class T> T handle(uint64_t value) { return (T)(uintptr_t)value; }
uint32_t query_api = VK_API_VERSION_1_0, extension_queries = 0;
bool query_group_extension = false;
VkResult query_status = VK_SUCCESS;
VKAPI_ATTR void VKAPI_CALL query_properties(VkPhysicalDevice,VkPhysicalDeviceProperties* out) { *out = {}; out->apiVersion = query_api; }
VKAPI_ATTR VkResult VKAPI_CALL query_extensions(VkPhysicalDevice,const char*,uint32_t* count,VkExtensionProperties* out) {
    ++extension_queries;
    if (query_status != VK_SUCCESS) return query_status;
    if (!out) { *count = query_group_extension ? 1 : 0; return VK_SUCCESS; }
    if (query_group_extension && *count) { out[0] = {}; std::strcpy(out[0].extensionName,VK_KHR_DEVICE_GROUP_EXTENSION_NAME); *count = 1; }
    else *count = 0;
    return VK_SUCCESS;
}
}
int main() {
    InputPNextCopy chain;
    VkDeviceGroupCommandBufferBeginInfo initial{VK_STRUCTURE_TYPE_DEVICE_GROUP_COMMAND_BUFFER_BEGIN_INFO}; initial.deviceMask = 1;
    VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}; inheritance.subpass = 3;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.pNext = &initial; begin.pInheritanceInfo = &inheritance;
    begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT; VkCommandBufferBeginInfo native_begin{};
    check(lower_singleton_group(begin,native_begin,chain) == VK_SUCCESS && !native_begin.pNext && native_begin.pInheritanceInfo == &inheritance && native_begin.flags == begin.flags,
        "singleton command begin removes only the group node and preserves secondary inheritance");
    check(begin.pNext == &initial && initial.deviceMask == 1 && !initial.pNext,"command begin never mutates caller-owned links or masks");
    initial.deviceMask = 2;
    check(lower_singleton_group(begin,native_begin,chain) == VK_ERROR_FEATURE_NOT_PRESENT,"singleton command begin cannot silently select another physical device"); initial.deviceMask = 1;

    VkImageView views[] = {handle<VkImageView>(12)};
    VkRenderPassAttachmentBeginInfo attachments{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO}; attachments.attachmentCount = 1; attachments.pAttachments = views;
    VkRenderPassSampleLocationsBeginInfoEXT samples{VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT};
    const VkRect2D selected{{3,4},{5,6}};
    VkDeviceGroupRenderPassBeginInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO}; group.deviceMask = group.deviceRenderAreaCount = 1; group.pDeviceRenderAreas = &selected;
    attachments.pNext = &group; group.pNext = &samples;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.pNext = &attachments; rp.renderArea = {{0,0},{16,16}};
    VkRenderPassBeginInfo native_rp{};
    SingletonRenderPassBeginChain render_chain;
    check(lower_singleton_group(rp,native_rp,render_chain) == VK_SUCCESS && std::memcmp(&native_rp.renderArea,&selected,sizeof(selected)) == 0,
        "singleton render begin honors its explicit device render area");
    const auto* a = static_cast<const VkRenderPassAttachmentBeginInfo*>(native_rp.pNext);
    check(a && a != &attachments && a->pAttachments == views && a->attachmentCount == 1 && a->pNext &&
        static_cast<const VkBaseInStructure*>(a->pNext)->sType == samples.sType && !vku::has_pnext(native_rp.pNext,group.sType),
        "filtering a middle node retains typed prefix/tail and unrelated immediate-call payloads");
    check(rp.pNext == &attachments && attachments.pNext == &group && group.pNext == &samples,"render input filtering does not splice the caller chain");
    group.deviceRenderAreaCount = 0;
    check(lower_singleton_group(rp,native_rp,render_chain) == VK_SUCCESS && std::memcmp(&native_rp.renderArea,&rp.renderArea,sizeof(rp.renderArea)) == 0,
        "zero per-device areas retains ordinary renderArea"); group.deviceRenderAreaCount = 1;
    fail_new = true;
    check(lower_singleton_group(rp,native_rp,render_chain) == VK_SUCCESS && fail_new && attachments.pNext == &group,
        "void render begin lowers a mixed chain without any heap allocation"); fail_new = false;
    group.pNext = &attachments;
    check(lower_singleton_group(rp,native_rp,render_chain) == VK_ERROR_INITIALIZATION_FAILED && !render_chain.head(),"typed filtering detects a cyclic chain even through an omitted node"); group.pNext = &samples;

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.buffer = handle<VkBuffer>(24);
    VkExportMemoryAllocateInfo external{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO}; external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; flags.flags = VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT; flags.deviceMask = 1;
    external.pNext = &flags; flags.pNext = &dedicated;
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memory.pNext = &external; memory.allocationSize = 4096; memory.memoryTypeIndex = 7;
    VkMemoryAllocateInfo native_memory{};
    fail_new = true;
    check(lower_singleton_group(memory,native_memory,chain) == VK_ERROR_OUT_OF_HOST_MEMORY && !fail_new && !chain.head() && external.pNext == &flags,
        "allocation input OOM leaves no partial chain and never changes caller links");
    check(lower_singleton_group(memory,native_memory,chain) == VK_SUCCESS && native_memory.allocationSize == 4096 && native_memory.memoryTypeIndex == 7 &&
        !vku::has_pnext(native_memory.pNext,flags.sType) && vku::find_pnext<VkMemoryDedicatedAllocateInfo>(native_memory.pNext,dedicated.sType)->buffer == dedicated.buffer,
        "allocation mask lowering preserves external export and dedicated allocation metadata");
    flags.flags = 0; flags.deviceMask = 0xdead;
    check(lower_singleton_group(memory,native_memory,chain) == VK_SUCCESS && !vku::has_pnext(native_memory.pNext,flags.sType),
        "an ignored allocation deviceMask is not mistaken for an active multi-device request");
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT | VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; flags.deviceMask = 1;
    check(lower_singleton_group(memory,native_memory,chain) == VK_SUCCESS &&
        vku::find_pnext<VkMemoryAllocateFlagsInfo>(native_memory.pNext,flags.sType)->flags == VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT &&
        flags.flags == (VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT | VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) && external.pNext == &flags,
        "independent allocation flags survive without modifying caller storage");

    VkSubpassDependency deps[2]{}; deps[0].dependencyFlags = VK_DEPENDENCY_DEVICE_GROUP_BIT | VK_DEPENDENCY_BY_REGION_BIT; deps[1].dependencyFlags = VK_DEPENDENCY_VIEW_LOCAL_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; pass.dependencyCount = 2; pass.pDependencies = deps;
    VkRenderPassCreateInfo native_pass{}; std::vector<VkSubpassDependency> native_deps;
    lower_singleton_dependencies(pass,native_pass,native_deps);
    check(native_pass.pDependencies != deps && native_pass.pDependencies[0].dependencyFlags == VK_DEPENDENCY_BY_REGION_BIT &&
        native_pass.pDependencies[1].dependencyFlags == VK_DEPENDENCY_VIEW_LOCAL_BIT && deps[0].dependencyFlags == (VK_DEPENDENCY_DEVICE_GROUP_BIT | VK_DEPENDENCY_BY_REGION_BIT),
        "ordinary render-pass dependency lowering preserves native multiview and region semantics");

    uint32_t indices[] = {0}, masks[] = {1,0,1};
    VkCommandBuffer commands[] = {handle<VkCommandBuffer>(101),handle<VkCommandBuffer>(102),handle<VkCommandBuffer>(103)};
    VkSemaphore semaphore = handle<VkSemaphore>(201); VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkProtectedSubmitInfo protected_info{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};
    VkDeviceGroupSubmitInfo submit_group{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO}; submit_group.pNext = &protected_info;
    submit_group.commandBufferCount = 3; submit_group.pCommandBufferDeviceMasks = masks;
    submit_group.waitSemaphoreCount = submit_group.signalSemaphoreCount = 1; submit_group.pWaitSemaphoreDeviceIndices = submit_group.pSignalSemaphoreDeviceIndices = indices;
    uint64_t value = 0; VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}; timeline.pNext = &submit_group;
    timeline.waitSemaphoreValueCount = timeline.signalSemaphoreValueCount = 1; timeline.pWaitSemaphoreValues = timeline.pSignalSemaphoreValues = &value;
    VkSubmitInfo submits[2]{}; for (auto& s : submits) { s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; s.commandBufferCount = 3; s.pCommandBuffers = commands; }
    submits[0].pNext = &timeline; submits[0].waitSemaphoreCount = submits[0].signalSemaphoreCount = 1;
    submits[0].pWaitSemaphores = submits[0].pSignalSemaphores = &semaphore; submits[0].pWaitDstStageMask = &stage;
    SingletonGroupBatch batch; const VkSubmitInfo* native_submits{};
    check(batch.submits(2,submits,native_submits) == VK_SUCCESS && native_submits != submits && native_submits[0].commandBufferCount == 2 &&
        native_submits[0].pCommandBuffers[0] == commands[0] && native_submits[0].pCommandBuffers[1] == commands[2] && native_submits[1].pCommandBuffers == commands,
        "zero command masks suppress only those buffers, retaining batch and execution order");
    check(native_submits[0].pWaitSemaphores == &semaphore && native_submits[0].pSignalSemaphores == &semaphore &&
        native_submits[0].pWaitDstStageMask == &stage && vku::has_pnext(native_submits[0].pNext,timeline.sType) &&
        vku::has_pnext(native_submits[0].pNext,protected_info.sType) && !vku::has_pnext(native_submits[0].pNext,submit_group.sType),
        "masked submit lowering retains semaphore operations, timeline values and protected input");
    check(timeline.pNext == &submit_group && submit_group.pNext == &protected_info && submits[0].commandBufferCount == 3 && masks[1] == 0,
        "submit compaction leaves caller commands, masks and chain unchanged");
    masks[0] = masks[2] = 0;
    check(batch.submits(2,submits,native_submits) == VK_SUCCESS && native_submits[0].commandBufferCount == 0 && native_submits[0].signalSemaphoreCount == 1,
        "an all-zero command mask still submits its semaphore operations and caller fence");
    fail_new = true;
    check(batch.submits(2,submits,native_submits) == VK_ERROR_OUT_OF_HOST_MEMORY && !fail_new && native_submits == submits,
        "submit preparation OOM returns before any native batch can be submitted");
    indices[0] = 1;
    check(batch.submits(2,submits,native_submits) == VK_ERROR_FEATURE_NOT_PRESENT,"singleton submit does not silently execute foreign-device semaphore operations"); indices[0] = 0;
    VkDeviceGroupBindSparseInfo sparse_group{VK_STRUCTURE_TYPE_DEVICE_GROUP_BIND_SPARSE_INFO};
    auto sparse_timeline = timeline; sparse_timeline.pNext = &sparse_group;
    VkBindSparseInfo sparse{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO}; sparse.pNext = &sparse_timeline;
    const VkBindSparseInfo* native_sparse{};
    check(batch.sparse(1,&sparse,native_sparse) == VK_SUCCESS && !vku::has_pnext(native_sparse->pNext,sparse_group.sType) &&
        vku::has_pnext(native_sparse->pNext,timeline.sType) && sparse_timeline.pNext == &sparse_group,
        "sparse binding lowers singleton device indices without losing timeline metadata");
    VkSubmitInfo ordinary{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    fail_new = true;
    check(batch.submits(1,&ordinary,native_submits) == VK_SUCCESS && native_submits == &ordinary && fail_new,
        "ordinary submission fast path performs no allocation"); fail_new = false;
    uint32_t local = 0;
    VkBindBufferMemoryDeviceGroupInfo buffer_group{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO}; buffer_group.deviceIndexCount = 1; buffer_group.pDeviceIndices = &local;
    VkBindBufferMemoryInfo buffer_binds[2]{};
    for (auto& bind : buffer_binds) { bind.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO; bind.buffer = handle<VkBuffer>(301); bind.memory = handle<VkDeviceMemory>(401); bind.memoryOffset = 0x100000040ull; }
    buffer_binds[1].pNext = &buffer_group; const VkBindBufferMemoryInfo* native_buffers{};
    check(batch.buffers(2,buffer_binds,native_buffers) == VK_SUCCESS && native_buffers != buffer_binds && !native_buffers[1].pNext &&
        native_buffers[1].buffer == buffer_binds[1].buffer && native_buffers[1].memory == buffer_binds[1].memory && native_buffers[1].memoryOffset == 0x100000040ull,
        "buffer binding lowers local mappings and preserves full-width native handles/offsets");
    check(buffer_binds[1].pNext == &buffer_group && buffer_group.pDeviceIndices == &local,"buffer binding leaves the application's chain and indices unchanged");
    local = 1;
    check(batch.buffers(2,buffer_binds,native_buffers) == VK_ERROR_FEATURE_NOT_PRESENT && native_buffers == buffer_binds,
        "a foreign mapping in a later buffer bind is rejected before publishing any native batch"); local = 0;
    fail_new = true;
    check(batch.buffers(2,buffer_binds,native_buffers) == VK_ERROR_OUT_OF_HOST_MEMORY && !fail_new && native_buffers == buffer_binds,
        "buffer bind preparation OOM cannot leave an earlier native binding applied");
    buffer_group.deviceIndexCount = 0; buffer_group.pDeviceIndices = reinterpret_cast<const uint32_t*>(uintptr_t(1));
    check(batch.buffers(2,buffer_binds,native_buffers) == VK_SUCCESS && !native_buffers[1].pNext,
        "zero buffer deviceIndexCount ignores its unused pointer payload");
    VkBindImageMemoryDeviceGroupInfo image_group{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO}; image_group.deviceIndexCount = 1; image_group.pDeviceIndices = &local;
    VkBindImagePlaneMemoryInfo plane{VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO}; plane.planeAspect = VK_IMAGE_ASPECT_PLANE_1_BIT; plane.pNext = &image_group;
    VkBindImageMemoryInfo image_bind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO}; image_bind.pNext = &plane; image_bind.image = handle<VkImage>(501); image_bind.memory = handle<VkDeviceMemory>(601); image_bind.memoryOffset = 0x100000080ull;
    const VkBindImageMemoryInfo* native_images{};
    check(batch.images(1,&image_bind,native_images) == VK_SUCCESS && native_images->memoryOffset == image_bind.memoryOffset &&
        vku::find_pnext<VkBindImagePlaneMemoryInfo>(native_images->pNext,plane.sType)->planeAspect == plane.planeAspect &&
        !vku::has_pnext(native_images->pNext,image_group.sType) && plane.pNext == &image_group,
        "image binding retains a native plane prefix while removing only the group node");
    VkBindImageMemorySwapchainInfoKHR swapchain{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR}; swapchain.swapchain = handle<VkSwapchainKHR>(701); swapchain.imageIndex = 3;
    image_group.pNext = &swapchain; image_bind.pNext = &image_group;
    check(batch.images(1,&image_bind,native_images) == VK_SUCCESS &&
        vku::find_pnext<VkBindImageMemorySwapchainInfoKHR>(native_images->pNext,swapchain.sType)->imageIndex == 3 && image_group.pNext == &swapchain,
        "image binding retains a native swapchain tail without modifying caller links");
    image_group.pNext = nullptr; image_group.deviceIndexCount = 0; image_group.splitInstanceBindRegionCount = 1;
    VkRect2D whole_image{{0,0},{16,16}}; image_group.pSplitInstanceBindRegions = &whole_image;
    check(batch.images(1,&image_bind,native_images) == VK_SUCCESS && !native_images->pNext && native_images->image == image_bind.image &&
        native_images->memory == image_bind.memory && native_images->memoryOffset == image_bind.memoryOffset && image_group.splitInstanceBindRegionCount == 1,
        "the valid N=1 whole-image split lowers to the same ordinary memory mapping");
    image_group.splitInstanceBindRegionCount = 2;
    check(batch.images(1,&image_bind,native_images) == VK_ERROR_FEATURE_NOT_PRESENT,"singleton image binding cannot silently accept a multi-device split");
    image_group.splitInstanceBindRegionCount = 1; fail_new = true;
    check(batch.images(1,&image_bind,native_images) == VK_ERROR_OUT_OF_HOST_MEMORY && !fail_new && native_images == &image_bind,
        "image bind OOM leaves no partially published native batch");
    image_group.splitInstanceBindRegionCount = 0; image_group.pSplitInstanceBindRegions = reinterpret_cast<const VkRect2D*>(uintptr_t(1));
    check(batch.images(1,&image_bind,native_images) == VK_SUCCESS && !native_images->pNext,"zero split count ignores its unused rectangle pointer");
    image_bind.pNext = nullptr; fail_new = true;
    check(batch.images(1,&image_bind,native_images) == VK_SUCCESS && native_images == &image_bind && fail_new,
        "ordinary image binding retains the allocation-free path"); fail_new = false;
    bool supported = false;
    check(native_device_group_support({},query_properties,query_extensions,supported) == VK_SUCCESS && !supported,
        "image-query lowering detects a native API1.0 provider without device-group support");
    query_group_extension = true;
    check(native_device_group_support({},query_properties,query_extensions,supported) == VK_SUCCESS && supported,
        "image-query lowering preserves physical-device native KHR group support before device creation");
    query_api = VK_API_VERSION_1_1; extension_queries = 0;
    check(native_device_group_support({},query_properties,query_extensions,supported) == VK_SUCCESS && supported && !extension_queries,
        "native Core1.1 image-query flags bypass extension enumeration");
    query_api = VK_API_VERSION_1_0; query_status = VK_ERROR_OUT_OF_HOST_MEMORY;
    check(native_device_group_support({},query_properties,query_extensions,supported) == VK_ERROR_OUT_OF_HOST_MEMORY && !supported,
        "physical extension-query failures are propagated without inventing format support");
    query_status = VK_SUCCESS; fail_new = true;
    check(native_device_group_support({},query_properties,query_extensions,supported) == VK_ERROR_OUT_OF_HOST_MEMORY && !fail_new,
        "physical capability-query allocation failures return host OOM");
    // An aspect declaration can be omitted at the legacy driver boundary, but
    // the multiview prefix and all application-owned links must survive.
    VkInputAttachmentAspectReference input_aspect{0, 0, VK_IMAGE_ASPECT_COLOR_BIT};
    VkRenderPassInputAttachmentAspectCreateInfo aspects{VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO};
    aspects.aspectReferenceCount = 1; aspects.pAspectReferences = &input_aspect;
    const uint32_t mask = 3;
    VkRenderPassMultiviewCreateInfo view_info{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO, &aspects};
    view_info.subpassCount = 1; view_info.pViewMasks = &mask;
    const auto saved_aspects = aspects; const auto saved_view = view_info;
    InputPNextCopy aspect_chain;
    check(aspect_chain.copy(&view_info, InputChainKind::RenderPassCreate, false, aspects.sType) == VK_SUCCESS &&
          aspect_chain.head() != &view_info && !vku::has_pnext(aspect_chain.head(), aspects.sType),
          "maintenance2 aspect removal owns the remaining render-pass chain");
    const auto* copied_view = vku::find_pnext<VkRenderPassMultiviewCreateInfo>(aspect_chain.head(), view_info.sType);
    check(copied_view && copied_view->pViewMasks == &mask && copied_view->subpassCount == 1 && !copied_view->pNext &&
          !memcmp(&aspects,&saved_aspects,sizeof aspects) && !memcmp(&view_info,&saved_view,sizeof view_info),
          "aspect removal retains multiview masks and never changes caller bytes or links");
    fail_new = true;
    check(aspect_chain.copy(&view_info, InputChainKind::RenderPassCreate, false, aspects.sType) == VK_ERROR_OUT_OF_HOST_MEMORY &&
          !fail_new && !aspect_chain.head() && view_info.pNext == &aspects,
          "aspect-chain OOM leaves no native input or mutated caller chain");
    printf("Singleton device group failures: %u\n",failures); return failures ? 1 : 0;
}

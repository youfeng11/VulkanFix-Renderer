#pragma once
#include "native_probe_resources.h"

namespace vkfix::nativeprobe {
// The binary control executes the same writes, copies and barriers. Only the
// timeline subject waits for a future host signal and reports queue progress
// through a counter. Run the native subject in a disposable process.
template<class Context, class Ready>
std::string probe_timeline_queue(Context &p, VkPhysicalDevice physical, uint32_t family,
                                 bool timeline, bool core, Ready ready, Sync2Route route = Sync2Route::Legacy) {
    Resources<Context> gpu(p,physical,family);
    constexpr uint32_t words=64;
    constexpr VkDeviceSize bytes=words*sizeof(uint32_t);
    constexpr uint32_t expected=0x2468acdf;
    auto source=gpu.buffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto output=gpu.buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto produce=gpu.begin(), consume=gpu.begin();
    p.template dev<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(produce,source.handle,0,bytes,expected);
    VkBufferCopy copy{0,0,bytes};
    p.template dev<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(consume,source.handle,output.handle,1,&copy);
    gpu.host_barrier(consume,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    const auto end=p.template dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
    check(end(produce),"end timeline producer");
    check(end(consume),"end timeline consumer");

    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue=3;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext=timeline ? &type : nullptr;
    VkSemaphore semaphore{};
    check(p.template dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(p.device,&sci,nullptr,&semaphore),"create queue probe semaphore");
    auto destroy_semaphore=p.template dev<PFN_vkDestroySemaphore>("vkDestroySemaphore");
    gpu.objects.push_back([=,&p] { destroy_semaphore(p.device,semaphore,nullptr); });
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{};
    check(p.template dev<PFN_vkCreateFence>("vkCreateFence")(p.device,&fci,nullptr,&fence),"create queue probe fence");
    auto destroy_fence=p.template dev<PFN_vkDestroyFence>("vkDestroyFence");
    gpu.objects.push_back([=,&p] { destroy_fence(p.device,fence,nullptr); });

    VkQueue queue{};
    p.template dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(p.device,family,0,&queue);
    const bool modern=route!=Sync2Route::Legacy;
    auto submit=modern ? nullptr : p.template dev<PFN_vkQueueSubmit>("vkQueueSubmit");
    auto submit2=modern ? p.template dev<PFN_vkQueueSubmit2>(route==Sync2Route::Core ? "vkQueueSubmit2" : "vkQueueSubmit2KHR") : nullptr;
    auto fence_wait=p.template dev<PFN_vkWaitForFences>("vkWaitForFences");
    auto signal=timeline ? p.template dev<PFN_vkSignalSemaphore>(core ? "vkSignalSemaphore" : "vkSignalSemaphoreKHR") : nullptr;
    auto wait=timeline ? p.template dev<PFN_vkWaitSemaphores>(core ? "vkWaitSemaphores" : "vkWaitSemaphoresKHR") : nullptr;
    auto counter=timeline ? p.template dev<PFN_vkGetSemaphoreCounterValue>(core ? "vkGetSemaphoreCounterValue" : "vkGetSemaphoreCounterValueKHR") : nullptr;

    const uint64_t release=5, produced=8, consumed=11;
    VkTimelineSemaphoreSubmitInfo values[2]{{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO},
                                             {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}};
    values[0].waitSemaphoreValueCount=values[0].signalSemaphoreValueCount=1;
    values[0].pWaitSemaphoreValues=&release; values[0].pSignalSemaphoreValues=&produced;
    values[1].waitSemaphoreValueCount=values[1].signalSemaphoreValueCount=1;
    values[1].pWaitSemaphoreValues=&produced; values[1].pSignalSemaphoreValues=&consumed;
    const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submits[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SUBMIT_INFO}};
    submits[0].pNext=timeline ? &values[0] : nullptr;
    submits[0].waitSemaphoreCount=timeline ? 1 : 0;
    submits[0].pWaitSemaphores=timeline ? &semaphore : nullptr;
    submits[0].pWaitDstStageMask=timeline ? &stage : nullptr;
    submits[0].commandBufferCount=1; submits[0].pCommandBuffers=&produce;
    submits[0].signalSemaphoreCount=1; submits[0].pSignalSemaphores=&semaphore;
    submits[1].pNext=timeline ? &values[1] : nullptr;
    submits[1].waitSemaphoreCount=1; submits[1].pWaitSemaphores=&semaphore;
    submits[1].pWaitDstStageMask=&stage;
    submits[1].commandBufferCount=1; submits[1].pCommandBuffers=&consume;
    submits[1].signalSemaphoreCount=timeline ? 1 : 0;
    submits[1].pSignalSemaphores=timeline ? &semaphore : nullptr;

    // Tests call this only after their device is disposable. Once submitted,
    // error/timeout paths must preserve resources until the process exits.
    ready();
    VkResult submitted;
    if (modern) {
        VkCommandBufferSubmitInfo commands[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO},
                                               {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
        commands[0].commandBuffer=produce;
        commands[1].commandBuffer=consume;
        VkSemaphoreSubmitInfo wait_info[2]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},
                                            {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
        VkSemaphoreSubmitInfo signal_info[2]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},
                                              {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
        VkSubmitInfo2 batches[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO_2},{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}};
        for (unsigned i=0; i<2; ++i) {
            wait_info[i].semaphore=signal_info[i].semaphore=semaphore;
            wait_info[i].stageMask=VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            signal_info[i].stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            wait_info[i].value=timeline ? *values[i].pWaitSemaphoreValues : 0;
            signal_info[i].value=timeline ? *values[i].pSignalSemaphoreValues : 0;
            batches[i].waitSemaphoreInfoCount=submits[i].waitSemaphoreCount;
            batches[i].pWaitSemaphoreInfos=&wait_info[i];
            batches[i].commandBufferInfoCount=1;
            batches[i].pCommandBufferInfos=&commands[i];
            batches[i].signalSemaphoreInfoCount=submits[i].signalSemaphoreCount;
            batches[i].pSignalSemaphoreInfos=&signal_info[i];
        }
        submitted=submit2(queue,2,batches,fence);
    } else {
        submitted=submit(queue,2,submits,fence);
    }
    if (submitted!=VK_SUCCESS) return "queue semaphore submission failed: VkResult "+std::to_string(submitted);
    p.pending=true;
    std::string failure;
    if (timeline) {
        uint64_t value=0;
        const auto early=fence_wait(p.device,1,&fence,VK_TRUE,20000000);
        const auto queried=counter(p.device,semaphore,&value);
        // A driver that already ran the work can have a larger value. Never
        // issue an invalid decreasing host signal in that failure path.
        if (early!=VK_TIMEOUT || queried!=VK_SUCCESS || value!=3)
            return "timeline queue advanced or counter query failed before host signal";
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        si.semaphore=semaphore; si.value=release;
        if (signal(p.device,&si)!=VK_SUCCESS) return "host signal did not release pending timeline work";
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount=1; wi.pSemaphores=&semaphore; wi.pValues=&consumed;
        if (wait(p.device,&wi,1000000000)!=VK_SUCCESS) failure="host wait did not observe the queue signal";
    }
    if (fence_wait(p.device,1,&fence,VK_TRUE,1000000000)!=VK_SUCCESS)
        return "queue dependency did not complete within the fence timeout";
    p.pending=false;
    if (!failure.empty()) return failure;
    if (timeline) {
        uint64_t value=0;
        if (counter(p.device,semaphore,&value)!=VK_SUCCESS || value!=consumed)
            return "final timeline queue counter mismatch";
    }
    gpu.invalidate(output);
    const auto *data=static_cast<const uint32_t*>(output.mapped);
    for (uint32_t i=0;i<words;++i) if (data[i]!=expected) return "queue semaphore transfer readback mismatch";
    return {};
}
} // namespace vkfix::nativeprobe

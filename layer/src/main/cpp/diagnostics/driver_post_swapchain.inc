#ifdef __ANDROID__
void swapchain_smoke(Probe &p, VkPhysicalDevice physical, uint32_t family, PostWindow *window) {
    GpuProbe gpu(p,physical,family);
    VkBool32 supported=VK_FALSE;
    require(p.get<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR")(physical,family,p.surface,&supported), "query surface queue support");
    if (!supported) throw ProbeMismatch("selected graphics queue cannot present to Android surface");
    VkSurfaceCapabilitiesKHR caps{};
    require(p.get<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(physical,p.surface,&caps), "query surface capabilities");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        row("VK_KHR_swapchain.present", "SKIP", "surface cannot accept transfer clears; rendering-based present probe required", "functional");
        return;
    }
    auto get_formats=p.get<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
    auto formats=enumerate<VkSurfaceFormatKHR>([&](uint32_t *n,VkSurfaceFormatKHR *v){ return get_formats(physical,p.surface,n,v); });
    if (formats.empty()) throw std::runtime_error("surface has no formats");
    VkSurfaceFormatKHR format=formats.front();
    for (const auto &value:formats) if (value.format==VK_FORMAT_R8G8B8A8_UNORM || value.format==VK_FORMAT_B8G8R8A8_UNORM) { format=value; break; }
    if (format.format==VK_FORMAT_UNDEFINED) format.format=VK_FORMAT_B8G8R8A8_UNORM;
    auto extent=caps.currentExtent;
    if (extent.width==UINT32_MAX) {
        extent.width=std::clamp(uint32_t(std::max(1,ANativeWindow_getWidth(window))),caps.minImageExtent.width,caps.maxImageExtent.width);
        extent.height=std::clamp(uint32_t(std::max(1,ANativeWindow_getHeight(window))),caps.minImageExtent.height,caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) throw ProbeMismatch("surface has zero extent");
    uint32_t count=caps.minImageCount+1;
    if (caps.maxImageCount && count>caps.maxImageCount) count=caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface=p.surface; ci.minImageCount=count; ci.imageFormat=format.format; ci.imageColorSpace=format.colorSpace;
    ci.imageExtent=extent; ci.imageArrayLayers=1; ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.preTransform=caps.currentTransform;
    const uint32_t alpha=caps.supportedCompositeAlpha;
    if (!alpha) throw std::runtime_error("surface has no composite alpha mode");
    ci.compositeAlpha=VkCompositeAlphaFlagBitsKHR(alpha & (~alpha+1));
    ci.presentMode=VK_PRESENT_MODE_FIFO_KHR; ci.clipped=VK_TRUE;
    auto destroy=p.dev<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR");
    VkSwapchainKHR swapchain{};
    require(p.dev<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(p.device,&ci,nullptr,&swapchain), "create Android swapchain");
    gpu.objects.push_back([=,&p]{ destroy(p.device,swapchain,nullptr); });
    auto get_images=p.dev<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    auto images=enumerate<VkImage>([&](uint32_t *n,VkImage *v){ return get_images(p.device,swapchain,n,v); });
    if (images.empty()) throw std::runtime_error("swapchain has no images");
    auto destroy_sem=p.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore");
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired{},rendered{};
    require(p.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(p.device,&si,nullptr,&acquired), "create acquire semaphore");
    gpu.objects.push_back([=,&p]{ destroy_sem(p.device,acquired,nullptr); });
    require(p.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(p.device,&si,nullptr,&rendered), "create present semaphore");
    gpu.objects.push_back([=,&p]{ destroy_sem(p.device,rendered,nullptr); });
    uint32_t index=UINT32_MAX;
    auto result=p.dev<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR")(p.device,swapchain,3000000000ull,acquired,VK_NULL_HANDLE,&index);
    if (result!=VK_SUBOPTIMAL_KHR) require(result,"acquire swapchain image (3 seconds)");
    p.pending=true;
    if (index>=images.size()) throw std::runtime_error("swapchain image index out of range");
    auto cb=gpu.begin();
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.image=images[index]; ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    auto barrier=p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    barrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
    VkClearColorValue color{{0.f,1.f,0.f,1.f}};
    p.dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cb,images[index],ib.newLayout,&color,1,&ib.subresourceRange);
    ib.oldLayout=ib.newLayout; ib.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    ib.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask=0;
    barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&ib);
    require(p.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cb), "end swapchain commands");
    auto destroy_fence=p.dev<PFN_vkDestroyFence>("vkDestroyFence");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{};
    require(p.dev<PFN_vkCreateFence>("vkCreateFence")(p.device,&fi,nullptr,&fence), "create swapchain fence");
    gpu.objects.push_back([=,&p]{ destroy_fence(p.device,fence,nullptr); });
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&acquired;
    submit.pWaitDstStageMask=&stage; submit.commandBufferCount=1; submit.pCommandBuffers=&cb;
    submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&rendered;
    VkQueue queue{}; p.dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(p.device,family,0,&queue);
    require(p.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&submit,fence), "submit swapchain clear");
    p.pending=true;
    require(p.dev<PFN_vkWaitForFences>("vkWaitForFences")(p.device,1,&fence,VK_TRUE,3000000000ull), "wait swapchain fence (3 seconds)");
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.waitSemaphoreCount=1; present.pWaitSemaphores=&rendered;
    present.swapchainCount=1; present.pSwapchains=&swapchain; present.pImageIndices=&index;
    result=p.dev<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")(queue,&present);
    // Core WSI has no presentation-completion fence. WaitIdle cannot prove
    // that the presentation engine released the semaphore or swapchain:
    // https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html
    // The private, one-shot worker exits after delivering its report. Retain
    // WSI/device/instance resources until that exit rather than destroy in use.
    p.retain_device=true;
    p.live_present_resources=true;
    p.pending=false;
    if (result!=VK_SUBOPTIMAL_KHR) require(result,"present swapchain image");
    row("VK_KHR_swapchain.present", "PASS", "Android surface: create/acquire/clear/submit/present accepted; WSI resources retained until one-shot worker exits", "functional");
}
#endif

#include "swapchain.h"
#include "layer_manager.h"
#include "driver_loader.h"
#include <string.h>
#include <algorithm>
#include <atomic>

REGISTER_LAYER_MODULE(SwapchainModule);

SwapchainModule::SwapchainModule()
    : ExtensionModuleBase(VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION, 0) {
    LOGI("SwapchainModule initialized");
}

SwapchainModule::~SwapchainModule() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pair : m_surface_windows) {
        if (pair.second) {
            ANativeWindow_release(pair.second);
        }
    }
    m_surface_windows.clear();
    m_swapchains.clear();
}

bool SwapchainModule::query_native_support(VkPhysicalDevice physDev) {
    PFN_vkEnumerateDeviceExtensionProperties real_fn =
        (PFN_vkEnumerateDeviceExtensionProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    if (!real_fn) return false;

    uint32_t count = 0;
    if (real_fn(physDev, NULL, &count, NULL) == VK_SUCCESS && count > 0) {
        std::vector<VkExtensionProperties> exts(count);
        if (real_fn(physDev, NULL, &count, exts.data()) == VK_SUCCESS) {
            for (const auto& e : exts) {
                if (strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

void SwapchainModule::on_enumerate_device_extensions(
    VkPhysicalDevice physicalDevice,
    std::vector<VkExtensionProperties>& extensions
) {
    ExtensionModuleBase::on_enumerate_device_extensions(physicalDevice, extensions);
}

void SwapchainModule::on_post_create_device(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkResult result,
    void* pUserData
) {
    ExtensionModuleBase::on_post_create_device(physicalDevice, device, result, pUserData);
    if (result == VK_SUCCESS && device != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device_phys[(uint64_t)(uintptr_t)device] = physicalDevice;
    }
}

void SwapchainModule::on_destroy_device(VkDevice device) {
    ExtensionModuleBase::on_destroy_device(device);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_phys.erase((uint64_t)(uintptr_t)device);
}

uint32_t SwapchainModule::find_memory_type(VkDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_device_phys.find((uint64_t)(uintptr_t)device);
        if (it != m_device_phys.end()) {
            physDev = it->second;
        }
    }
    VkPhysicalDeviceMemoryProperties memProperties{};
    PFN_vkGetPhysicalDeviceMemoryProperties real_mp =
        (PFN_vkGetPhysicalDeviceMemoryProperties) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties");
    if (real_mp && physDev != VK_NULL_HANDLE) {
        real_mp(physDev, &memProperties);
    } else {
        PFN_vkEnumeratePhysicalDevices real_epd =
            (PFN_vkEnumeratePhysicalDevices) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkEnumeratePhysicalDevices");
        if (real_epd) {
            uint32_t pdCount = 1;
            VkPhysicalDevice pds[1];
            if (real_epd(get_last_instance(), &pdCount, pds) == VK_SUCCESS && pdCount > 0 && real_mp) {
                real_mp(pds[0], &memProperties);
            }
        }
    }

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if (typeFilter & (1 << i)) {
            return i;
        }
    }
    return 0;
}

bool SwapchainModule::on_create_android_surface(
    VkInstance instance,
    const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface,
    VkResult& outResult
) {
    PFN_vkCreateAndroidSurfaceKHR real_fn =
        (PFN_vkCreateAndroidSurfaceKHR) get_real_proc(instance, VK_NULL_HANDLE, "vkCreateAndroidSurfaceKHR");
    if (real_fn) {
        VkResult res = real_fn(instance, pCreateInfo, pAllocator, pSurface);
        if (res == VK_SUCCESS && pSurface && *pSurface != VK_NULL_HANDLE) {
            if (pCreateInfo && pCreateInfo->window) {
                ANativeWindow_acquire(pCreateInfo->window);
                std::lock_guard<std::mutex> lock(m_mutex);
                m_surface_windows[(uint64_t)(uintptr_t)*pSurface] = pCreateInfo->window;
            }
            outResult = res;
            return true;
        }
    }

    // Emulated synthetic surface
    if (!pCreateInfo || !pSurface) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    static std::atomic<uint64_t> s_fake_surf_id{0x5A000000};
    VkSurfaceKHR fakeSurf = (VkSurfaceKHR)(uintptr_t)s_fake_surf_id.fetch_add(1);
    ANativeWindow* win = pCreateInfo->window;
    if (win) {
        ANativeWindow_acquire(win);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_surface_windows[(uint64_t)(uintptr_t)fakeSurf] = win;
    }
    *pSurface = fakeSurf;
    outResult = VK_SUCCESS;
    LOGI("Created emulated VkSurfaceKHR %p (window=%p)", VK_HANDLE(fakeSurf), win);
    return true;
}

bool SwapchainModule::on_destroy_surface(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_surface_windows.find((uint64_t)(uintptr_t)surface);
        if (it != m_surface_windows.end()) {
            if (it->second) {
                ANativeWindow_release(it->second);
            }
            m_surface_windows.erase(it);
        }
    }
    PFN_vkDestroySurfaceKHR real_fn =
        (PFN_vkDestroySurfaceKHR) get_real_proc(instance, VK_NULL_HANDLE, "vkDestroySurfaceKHR");
    if (real_fn) {
        real_fn(instance, surface, pAllocator);
    }
    return true;
}

bool SwapchainModule::on_get_physical_device_surface_support(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported,
    VkResult& outResult
) {
    if (is_phys_device_native(physicalDevice)) return false;

    PFN_vkGetPhysicalDeviceSurfaceSupportKHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (real_fn && real_fn(physicalDevice, queueFamilyIndex, surface, pSupported) == VK_SUCCESS) {
        outResult = VK_SUCCESS;
        return true;
    }

    if (pSupported) *pSupported = VK_TRUE;
    outResult = VK_SUCCESS;
    return true;
}

bool SwapchainModule::on_get_physical_device_surface_capabilities(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities,
    VkResult& outResult
) {
    if (is_phys_device_native(physicalDevice)) return false;

    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (real_fn && real_fn(physicalDevice, surface, pSurfaceCapabilities) == VK_SUCCESS) {
        outResult = VK_SUCCESS;
        return true;
    }

    if (!pSurfaceCapabilities) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    uint32_t w = 1920, h = 1080;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_surface_windows.find((uint64_t)(uintptr_t)surface);
        if (it != m_surface_windows.end() && it->second) {
            int nw_w = ANativeWindow_getWidth(it->second);
            int nw_h = ANativeWindow_getHeight(it->second);
            if (nw_w > 0 && nw_h > 0) {
                w = (uint32_t)nw_w;
                h = (uint32_t)nw_h;
            }
        }
    }

    pSurfaceCapabilities->minImageCount = 2;
    pSurfaceCapabilities->maxImageCount = 4;
    pSurfaceCapabilities->currentExtent = {w, h};
    pSurfaceCapabilities->minImageExtent = {1, 1};
    pSurfaceCapabilities->maxImageExtent = {4096, 4096};
    pSurfaceCapabilities->maxImageArrayLayers = 1;
    pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->supportedCompositeAlpha =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    pSurfaceCapabilities->supportedUsageFlags =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    outResult = VK_SUCCESS;
    return true;
}

bool SwapchainModule::on_get_physical_device_surface_formats(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats,
    VkResult& outResult
) {
    if (is_phys_device_native(physicalDevice)) return false;

    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (real_fn && real_fn(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats) == VK_SUCCESS) {
        outResult = VK_SUCCESS;
        return true;
    }

    static const VkSurfaceFormatKHR s_formats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
    };
    uint32_t numFormats = sizeof(s_formats) / sizeof(s_formats[0]);

    if (!pSurfaceFormats) {
        *pSurfaceFormatCount = numFormats;
        outResult = VK_SUCCESS;
        return true;
    }

    uint32_t toCopy = std::min(*pSurfaceFormatCount, numFormats);
    for (uint32_t i = 0; i < toCopy; ++i) {
        pSurfaceFormats[i] = s_formats[i];
    }
    *pSurfaceFormatCount = toCopy;
    outResult = (toCopy < numFormats) ? VK_INCOMPLETE : VK_SUCCESS;
    return true;
}

bool SwapchainModule::on_get_physical_device_surface_present_modes(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes,
    VkResult& outResult
) {
    if (is_phys_device_native(physicalDevice)) return false;

    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR real_fn =
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (real_fn && real_fn(physicalDevice, surface, pPresentModeCount, pPresentModes) == VK_SUCCESS) {
        outResult = VK_SUCCESS;
        return true;
    }

    static const VkPresentModeKHR s_modes[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR
    };
    uint32_t numModes = sizeof(s_modes) / sizeof(s_modes[0]);

    if (!pPresentModes) {
        *pPresentModeCount = numModes;
        outResult = VK_SUCCESS;
        return true;
    }

    uint32_t toCopy = std::min(*pPresentModeCount, numModes);
    for (uint32_t i = 0; i < toCopy; ++i) {
        pPresentModes[i] = s_modes[i];
    }
    *pPresentModeCount = toCopy;
    outResult = (toCopy < numModes) ? VK_INCOMPLETE : VK_SUCCESS;
    return true;
}

bool SwapchainModule::on_create_swapchain(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain,
    VkResult& outResult
) {
    if (is_device_native(device)) return false;

    if (!pCreateInfo || !pSwapchain) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    // Try native swapchain creation first
    PFN_vkCreateSwapchainKHR real_fn =
        (PFN_vkCreateSwapchainKHR) get_real_proc(get_last_instance(), device, "vkCreateSwapchainKHR");
    if (real_fn) {
        VkResult r = real_fn(device, pCreateInfo, pAllocator, pSwapchain);
        if (r == VK_SUCCESS) {
            outResult = VK_SUCCESS;
            return true;
        }
    }

    // Emulated swapchain
    PFN_vkCreateImage real_ci = (PFN_vkCreateImage) get_real_proc(get_last_instance(), device, "vkCreateImage");
    PFN_vkGetImageMemoryRequirements real_gmr = (PFN_vkGetImageMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory real_am = (PFN_vkAllocateMemory) get_real_proc(get_last_instance(), device, "vkAllocateMemory");
    PFN_vkBindImageMemory real_bim = (PFN_vkBindImageMemory) get_real_proc(get_last_instance(), device, "vkBindImageMemory");

    if (!real_ci || !real_gmr || !real_am || !real_bim) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    auto sc = std::make_shared<EmulatedSwapchain>();
    sc->device = device;
    sc->surface = pCreateInfo->surface;
    sc->width = pCreateInfo->imageExtent.width;
    sc->height = pCreateInfo->imageExtent.height;
    sc->format = pCreateInfo->imageFormat;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_surface_windows.find((uint64_t)(uintptr_t)pCreateInfo->surface);
        if (it != m_surface_windows.end()) {
            sc->window = it->second;
        }
    }

    if (sc->window) {
        ANativeWindow_setBuffersGeometry(sc->window, (int32_t)sc->width, (int32_t)sc->height, WINDOW_FORMAT_RGBA_8888);
    }

    uint32_t count = std::max(pCreateInfo->minImageCount, 2u);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = pCreateInfo->imageFormat;
        ici.extent = {sc->width, sc->height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = pCreateInfo->imageSharingMode;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage img = VK_NULL_HANDLE;
        if (real_ci(device, &ici, pAllocator, &img) != VK_SUCCESS) continue;

        VkMemoryRequirements memReqs{};
        real_gmr(device, img, &memReqs);

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReqs.size;
        mai.memoryTypeIndex = find_memory_type(device, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (real_am(device, &mai, pAllocator, &mem) != VK_SUCCESS) continue;

        real_bim(device, img, mem, 0);
        sc->images.push_back({img, mem});
    }

    if (sc->images.empty()) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    // Allocate readback buffer for presenting to ANativeWindow
    if (sc->window) {
        PFN_vkCreateBuffer real_cb = (PFN_vkCreateBuffer) get_real_proc(get_last_instance(), device, "vkCreateBuffer");
        PFN_vkGetBufferMemoryRequirements real_gbm = (PFN_vkGetBufferMemoryRequirements) get_real_proc(get_last_instance(), device, "vkGetBufferMemoryRequirements");
        PFN_vkMapMemory real_mm = (PFN_vkMapMemory) get_real_proc(get_last_instance(), device, "vkMapMemory");
        PFN_vkBindBufferMemory real_bbm = (PFN_vkBindBufferMemory) get_real_proc(get_last_instance(), device, "vkBindBufferMemory");
        PFN_vkCreateCommandPool real_ccp = (PFN_vkCreateCommandPool) get_real_proc(get_last_instance(), device, "vkCreateCommandPool");
        PFN_vkAllocateCommandBuffers real_acb = (PFN_vkAllocateCommandBuffers) get_real_proc(get_last_instance(), device, "vkAllocateCommandBuffers");

        uint32_t qf = 0;
        VkQueue q = VK_NULL_HANDLE;
        LayerManager::get().get_device_queue_info(device, q, qf);

        VkDeviceSize bufSize = (VkDeviceSize)sc->width * sc->height * 4;
        sc->readbackSize = bufSize;

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bufSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (real_cb && real_cb(device, &bci, pAllocator, &sc->readbackBuffer) == VK_SUCCESS) {
            VkMemoryRequirements bufReqs{};
            real_gbm(device, sc->readbackBuffer, &bufReqs);

            VkMemoryAllocateInfo bufMai{};
            bufMai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            bufMai.allocationSize = bufReqs.size;
            bufMai.memoryTypeIndex = find_memory_type(device, bufReqs.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            if (real_am && real_am(device, &bufMai, pAllocator, &sc->readbackMemory) == VK_SUCCESS) {
                real_bbm(device, sc->readbackBuffer, sc->readbackMemory, 0);
                real_mm(device, sc->readbackMemory, 0, bufReqs.size, 0, &sc->mappedData);
            }
        }

        if (real_ccp && real_acb) {
            VkCommandPoolCreateInfo cpci{};
            cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpci.queueFamilyIndex = qf;
            if (real_ccp(device, &cpci, pAllocator, &sc->cmdPool) == VK_SUCCESS) {
                VkCommandBufferAllocateInfo cbai{};
                cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cbai.commandPool = sc->cmdPool;
                cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cbai.commandBufferCount = 1;
                real_acb(device, &cbai, &sc->cmdBuffer);
            }
        }
    }

    static std::atomic<uint64_t> s_fake_sc_id{0x6A000000};
    VkSwapchainKHR fakeSc = (VkSwapchainKHR)(uintptr_t)s_fake_sc_id.fetch_add(1);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_swapchains[(uint64_t)(uintptr_t)fakeSc] = sc;
    }

    *pSwapchain = fakeSc;
    outResult = VK_SUCCESS;
    LOGI("Created emulated VkSwapchainKHR %p (%ux%u, %zu images, window=%p)",
         VK_HANDLE(fakeSc), sc->width, sc->height, sc->images.size(), sc->window);
    return true;
}

bool SwapchainModule::on_destroy_swapchain(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator
) {
    std::shared_ptr<EmulatedSwapchain> sc = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_swapchains.find((uint64_t)(uintptr_t)swapchain);
        if (it != m_swapchains.end()) {
            sc = it->second;
            m_swapchains.erase(it);
        }
    }
    if (!sc) return false;

    PFN_vkDestroyImage real_di = (PFN_vkDestroyImage) get_real_proc(get_last_instance(), device, "vkDestroyImage");
    PFN_vkFreeMemory real_fm = (PFN_vkFreeMemory) get_real_proc(get_last_instance(), device, "vkFreeMemory");
    PFN_vkDestroyBuffer real_db = (PFN_vkDestroyBuffer) get_real_proc(get_last_instance(), device, "vkDestroyBuffer");
    PFN_vkDestroyCommandPool real_dcp = (PFN_vkDestroyCommandPool) get_real_proc(get_last_instance(), device, "vkDestroyCommandPool");
    PFN_vkUnmapMemory real_um = (PFN_vkUnmapMemory) get_real_proc(get_last_instance(), device, "vkUnmapMemory");

    for (auto& img : sc->images) {
        if (img.image != VK_NULL_HANDLE && real_di) real_di(device, img.image, pAllocator);
        if (img.memory != VK_NULL_HANDLE && real_fm) real_fm(device, img.memory, pAllocator);
    }
    sc->images.clear();

    if (sc->mappedData && real_um) {
        real_um(device, sc->readbackMemory);
        sc->mappedData = nullptr;
    }
    if (sc->readbackBuffer != VK_NULL_HANDLE && real_db) {
        real_db(device, sc->readbackBuffer, pAllocator);
    }
    if (sc->readbackMemory != VK_NULL_HANDLE && real_fm) {
        real_fm(device, sc->readbackMemory, pAllocator);
    }
    if (sc->cmdPool != VK_NULL_HANDLE && real_dcp) {
        real_dcp(device, sc->cmdPool, pAllocator);
    }

    LOGI("Destroyed emulated VkSwapchainKHR %p", VK_HANDLE(swapchain));
    return true;
}

bool SwapchainModule::on_get_swapchain_images(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages,
    VkResult& outResult
) {
    std::shared_ptr<EmulatedSwapchain> sc = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_swapchains.find((uint64_t)(uintptr_t)swapchain);
        if (it != m_swapchains.end()) sc = it->second;
    }
    if (!sc) return false;

    if (!pSwapchainImages) {
        *pSwapchainImageCount = (uint32_t)sc->images.size();
        outResult = VK_SUCCESS;
        return true;
    }

    uint32_t toCopy = std::min(*pSwapchainImageCount, (uint32_t)sc->images.size());
    for (uint32_t i = 0; i < toCopy; ++i) {
        pSwapchainImages[i] = sc->images[i].image;
    }
    *pSwapchainImageCount = toCopy;
    outResult = (toCopy < sc->images.size()) ? VK_INCOMPLETE : VK_SUCCESS;
    return true;
}

bool SwapchainModule::on_acquire_next_image(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex,
    VkResult& outResult
) {
    std::shared_ptr<EmulatedSwapchain> sc = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_swapchains.find((uint64_t)(uintptr_t)swapchain);
        if (it != m_swapchains.end()) sc = it->second;
    }
    if (!sc) return false;

    if (!pImageIndex || sc->images.empty()) {
        outResult = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }

    *pImageIndex = sc->nextImageIndex;
    sc->nextImageIndex = (sc->nextImageIndex + 1) % sc->images.size();

    // Signal the semaphore and/or fence via dummy submit
    if (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE) {
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t qf = 0;
        LayerManager::get().get_device_queue_info(device, queue, qf);
        if (queue != VK_NULL_HANDLE) {
            PFN_vkQueueSubmit real_qs = (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueSubmit");
            if (real_qs) {
                VkSubmitInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                if (semaphore != VK_NULL_HANDLE) {
                    si.signalSemaphoreCount = 1;
                    si.pSignalSemaphores = &semaphore;
                }
                real_qs(queue, (semaphore != VK_NULL_HANDLE) ? 1 : 0, (semaphore != VK_NULL_HANDLE) ? &si : nullptr, fence);
            }
        }
    }

    outResult = VK_SUCCESS;
    return true;
}

bool SwapchainModule::on_queue_present(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo,
    VkResult& outResult
) {
    if (!pPresentInfo || pPresentInfo->swapchainCount == 0) {
        outResult = VK_SUCCESS;
        return true;
    }

    std::shared_ptr<EmulatedSwapchain> sc = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_swapchains.find((uint64_t)(uintptr_t)pPresentInfo->pSwapchains[0]);
        if (it != m_swapchains.end()) sc = it->second;
    }
    if (!sc) return false;

    PFN_vkQueueSubmit real_qs = (PFN_vkQueueSubmit) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueSubmit");
    PFN_vkQueueWaitIdle real_qwi = (PFN_vkQueueWaitIdle) get_real_proc(get_last_instance(), VK_NULL_HANDLE, "vkQueueWaitIdle");

    // 1. Wait on present wait semaphores
    if (pPresentInfo->waitSemaphoreCount > 0 && real_qs) {
        std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
        si.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
        si.pWaitDstStageMask = waitStages.data();
        real_qs(queue, 1, &si, VK_NULL_HANDLE);
    }

    // 2. Read back & post to ANativeWindow if window is attached
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
        std::shared_ptr<EmulatedSwapchain> curSc = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_swapchains.find((uint64_t)(uintptr_t)pPresentInfo->pSwapchains[i]);
            if (it != m_swapchains.end()) curSc = it->second;
        }
        if (!curSc) continue;

        uint32_t imgIdx = pPresentInfo->pImageIndices[i];
        if (imgIdx >= curSc->images.size()) continue;

        if (curSc->window && curSc->readbackBuffer && curSc->mappedData && curSc->cmdBuffer) {
            PFN_vkBeginCommandBuffer real_bcb = (PFN_vkBeginCommandBuffer) get_real_proc(get_last_instance(), curSc->device, "vkBeginCommandBuffer");
            PFN_vkEndCommandBuffer real_ecb = (PFN_vkEndCommandBuffer) get_real_proc(get_last_instance(), curSc->device, "vkEndCommandBuffer");
            PFN_vkCmdPipelineBarrier real_cpb = (PFN_vkCmdPipelineBarrier) get_real_proc(get_last_instance(), curSc->device, "vkCmdPipelineBarrier");
            PFN_vkCmdCopyImageToBuffer real_citb = (PFN_vkCmdCopyImageToBuffer) get_real_proc(get_last_instance(), curSc->device, "vkCmdCopyImageToBuffer");

            if (real_bcb && real_ecb && real_cpb && real_citb) {
                VkCommandBufferBeginInfo cbbi{};
                cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                real_bcb(curSc->cmdBuffer, &cbbi);

                VkImageMemoryBarrier imb{};
                imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                imb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imb.image = curSc->images[imgIdx].image;
                imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                imb.subresourceRange.baseMipLevel = 0;
                imb.subresourceRange.levelCount = 1;
                imb.subresourceRange.baseArrayLayer = 0;
                imb.subresourceRange.layerCount = 1;

                real_cpb(curSc->cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);

                VkBufferImageCopy region{};
                region.bufferOffset = 0;
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageOffset = {0, 0, 0};
                region.imageExtent = {curSc->width, curSc->height, 1};

                real_citb(curSc->cmdBuffer, curSc->images[imgIdx].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          curSc->readbackBuffer, 1, &region);

                real_ecb(curSc->cmdBuffer);

                VkSubmitInfo copySubmit{};
                copySubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                copySubmit.commandBufferCount = 1;
                copySubmit.pCommandBuffers = &curSc->cmdBuffer;
                if (real_qs) {
                    real_qs(queue, 1, &copySubmit, VK_NULL_HANDLE);
                }
                if (real_qwi) {
                    real_qwi(queue);
                }

                ANativeWindow_Buffer winBuf{};
                if (ANativeWindow_lock(curSc->window, &winBuf, nullptr) == 0) {
                    uint8_t* src = (uint8_t*)curSc->mappedData;
                    uint8_t* dst = (uint8_t*)winBuf.bits;
                    uint32_t copyWidth = std::min<uint32_t>(curSc->width, (uint32_t)winBuf.width);
                    uint32_t copyHeight = std::min<uint32_t>(curSc->height, (uint32_t)winBuf.height);
                    uint32_t rowBytes = copyWidth * 4;
                    uint32_t dstStrideBytes = (uint32_t)winBuf.stride * 4;
                    uint32_t srcStrideBytes = curSc->width * 4;

                    for (uint32_t y = 0; y < copyHeight; ++y) {
                        memcpy(dst + y * dstStrideBytes, src + y * srcStrideBytes, rowBytes);
                    }
                    ANativeWindow_unlockAndPost(curSc->window);
                }
            }
        }
    }

    if (pPresentInfo->pResults) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            pPresentInfo->pResults[i] = VK_SUCCESS;
        }
    }
    outResult = VK_SUCCESS;
    return true;
}

#ifndef SWAPCHAIN_H
#define SWAPCHAIN_H

#include "extension_module_base.h"
#include <android/native_window.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>

class SwapchainModule : public ExtensionModuleBase {
public:
    SwapchainModule();
    ~SwapchainModule() override;

    void on_enumerate_device_extensions(
        VkPhysicalDevice physicalDevice,
        std::vector<VkExtensionProperties>& extensions) override;

    bool on_create_swapchain(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSwapchainKHR* pSwapchain,
        VkResult& outResult) override;

    bool on_destroy_swapchain(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* pAllocator) override;

    bool on_get_swapchain_images(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint32_t* pSwapchainImageCount,
        VkImage* pSwapchainImages,
        VkResult& outResult) override;

    bool on_acquire_next_image(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint64_t timeout,
        VkSemaphore semaphore,
        VkFence fence,
        uint32_t* pImageIndex,
        VkResult& outResult) override;

    bool on_queue_present(
        VkQueue queue,
        const VkPresentInfoKHR* pPresentInfo,
        VkResult& outResult) override;

    bool on_get_physical_device_surface_support(
        VkPhysicalDevice physicalDevice,
        uint32_t queueFamilyIndex,
        VkSurfaceKHR surface,
        VkBool32* pSupported,
        VkResult& outResult) override;

    bool on_get_physical_device_surface_capabilities(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        VkSurfaceCapabilitiesKHR* pSurfaceCapabilities,
        VkResult& outResult) override;

    bool on_get_physical_device_surface_formats(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        uint32_t* pSurfaceFormatCount,
        VkSurfaceFormatKHR* pSurfaceFormats,
        VkResult& outResult) override;

    bool on_get_physical_device_surface_present_modes(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        uint32_t* pPresentModeCount,
        VkPresentModeKHR* pPresentModes,
        VkResult& outResult) override;

    bool on_create_android_surface(
        VkInstance instance,
        const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSurfaceKHR* pSurface,
        VkResult& outResult) override;

    bool on_destroy_surface(
        VkInstance instance,
        VkSurfaceKHR surface,
        const VkAllocationCallbacks* pAllocator) override;

    void on_post_create_device(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkResult result,
        void* pUserData) override;

    void on_destroy_device(VkDevice device) override;

protected:
    bool query_native_support(VkPhysicalDevice physDev) override;

private:
    struct EmulatedImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct EmulatedSwapchain {
        VkDevice device = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::vector<EmulatedImage> images;
        uint32_t nextImageIndex = 0;
        ANativeWindow* window = nullptr;

        // Presentation readback resources
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
        VkBuffer readbackBuffer = VK_NULL_HANDLE;
        VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
        VkDeviceSize readbackSize = 0;
        void* mappedData = nullptr;
    };

    uint32_t find_memory_type(VkDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties);

    std::mutex m_mutex;
    std::unordered_map<uint64_t, VkPhysicalDevice> m_device_phys;
    std::unordered_map<uint64_t, ANativeWindow*> m_surface_windows;
    std::unordered_map<uint64_t, std::shared_ptr<EmulatedSwapchain>> m_swapchains;
};

#endif // SWAPCHAIN_H

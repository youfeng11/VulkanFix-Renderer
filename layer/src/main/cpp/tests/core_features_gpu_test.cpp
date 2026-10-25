#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <set>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
static unsigned failures = 0;
static void check(bool ok, const char *name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
static bool ok(VkResult result, const char *name) {
    if (result == VK_SUCCESS)
        return true;
    printf("FAIL: %s (%d)\n", name, result);
    ++failures;
    return false;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("VULKAN_FIX_LOG_PATH", "/data/local/tmp/vulkanfix-core-goal/core-features.log", 1);
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance{};
    if (!ok(vkCreateInstance(&ci, nullptr, &instance), "create instance"))
        return 1;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (!count ||
        !ok(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate device"))
        return 1;
    auto physical = devices.front();
    void *library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    auto props = (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    auto raw_query =
        (PFN_vkGetPhysicalDeviceFeatures2)gipa(instance, "vkGetPhysicalDeviceFeatures2");
    if (!raw_query)
        raw_query =
            (PFN_vkGetPhysicalDeviceFeatures2)gipa(instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!raw_query) {
        printf("FAIL: native features2 unavailable\n");
        return 1;
    }
    VkPhysicalDeviceProperties native_properties{};
    props(physical, &native_properties);
    printf("Native driver: %s / Vulkan %u.%u\n", native_properties.deviceName,
           VK_API_VERSION_MAJOR(native_properties.apiVersion),
           VK_API_VERSION_MINOR(native_properties.apiVersion));
    auto raw_enumerate = (PFN_vkEnumerateDeviceExtensionProperties)gipa(
        instance, "vkEnumerateDeviceExtensionProperties");
    raw_enumerate(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> ext(count);
    raw_enumerate(physical, nullptr, &count, ext.data());
    std::set<std::string> extensions;
    for (auto &e : ext)
        extensions.insert(e.extensionName);
    VkPhysicalDeviceVulkan11Features actual11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features actual12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    actual11.pNext = &actual12;
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &actual11;
    vkGetPhysicalDeviceFeatures2(physical, &query);
    printf("Core 1.2 mandatory status: subgroupBroadcastDynamicId=%u imagelessFramebuffer=%u "
           "uniformBufferStandardLayout=%u shaderSubgroupExtendedTypes=%u "
           "separateDepthStencilLayouts=%u hostQueryReset=%u timelineSemaphore=%u\n",
           actual12.subgroupBroadcastDynamicId, actual12.imagelessFramebuffer,
           actual12.uniformBufferStandardLayout, actual12.shaderSubgroupExtendedTypes,
           actual12.separateDepthStencilLayouts, actual12.hostQueryReset,
           actual12.timelineSemaphore);
    check(query.pNext == &actual11 && actual11.pNext == &actual12 && actual12.pNext == nullptr,
          "aggregate query restores caller links");
    VkPhysicalDeviceVulkan11Features expected11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features expected12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    if (native_properties.apiVersion >= VK_API_VERSION_1_2) {
        expected11.pNext = &expected12;
        VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        direct.pNext = &expected11;
        raw_query(physical, &direct);
    } else {
        if (native_properties.apiVersion >= VK_API_VERSION_1_1 ||
            extensions.count("VK_KHR_16bit_storage")) {
            VkPhysicalDevice16BitStorageFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected11.storageBuffer16BitAccess = native.storageBuffer16BitAccess;
            expected11.uniformAndStorageBuffer16BitAccess =
                native.uniformAndStorageBuffer16BitAccess;
            expected11.storagePushConstant16 = native.storagePushConstant16;
            expected11.storageInputOutput16 = native.storageInputOutput16;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_1 ||
            extensions.count("VK_KHR_multiview")) {
            VkPhysicalDeviceMultiviewFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected11.multiview = native.multiview;
            expected11.multiviewGeometryShader = native.multiviewGeometryShader;
            expected11.multiviewTessellationShader = native.multiviewTessellationShader;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_1 ||
            extensions.count("VK_KHR_variable_pointers")) {
            VkPhysicalDeviceVariablePointersFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected11.variablePointersStorageBuffer = native.variablePointersStorageBuffer;
            expected11.variablePointers = native.variablePointers;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_1) {
            VkPhysicalDeviceProtectedMemoryFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected11.protectedMemory = native.protectedMemory;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_1 ||
            extensions.count("VK_KHR_sampler_ycbcr_conversion")) {
            VkPhysicalDeviceSamplerYcbcrConversionFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected11.samplerYcbcrConversion = native.samplerYcbcrConversion;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_1 ||
            extensions.count("VK_KHR_shader_draw_parameters")) {
            VkPhysicalDeviceShaderDrawParametersFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected11.shaderDrawParameters = native.shaderDrawParameters;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_8bit_storage")) {
            VkPhysicalDevice8BitStorageFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.storageBuffer8BitAccess = native.storageBuffer8BitAccess;
            expected12.uniformAndStorageBuffer8BitAccess = native.uniformAndStorageBuffer8BitAccess;
            expected12.storagePushConstant8 = native.storagePushConstant8;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_shader_atomic_int64")) {
            VkPhysicalDeviceShaderAtomicInt64Features native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.shaderBufferInt64Atomics = native.shaderBufferInt64Atomics;
            expected12.shaderSharedInt64Atomics = native.shaderSharedInt64Atomics;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_shader_float16_int8")) {
            VkPhysicalDeviceShaderFloat16Int8Features native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.shaderFloat16 = native.shaderFloat16;
            expected12.shaderInt8 = native.shaderInt8;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_EXT_descriptor_indexing")) {
            VkPhysicalDeviceDescriptorIndexingFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.shaderInputAttachmentArrayDynamicIndexing =
                native.shaderInputAttachmentArrayDynamicIndexing;
            expected12.shaderUniformTexelBufferArrayDynamicIndexing =
                native.shaderUniformTexelBufferArrayDynamicIndexing;
            expected12.shaderStorageTexelBufferArrayDynamicIndexing =
                native.shaderStorageTexelBufferArrayDynamicIndexing;
            expected12.shaderUniformBufferArrayNonUniformIndexing =
                native.shaderUniformBufferArrayNonUniformIndexing;
            expected12.shaderSampledImageArrayNonUniformIndexing =
                native.shaderSampledImageArrayNonUniformIndexing;
            expected12.shaderStorageBufferArrayNonUniformIndexing =
                native.shaderStorageBufferArrayNonUniformIndexing;
            expected12.shaderStorageImageArrayNonUniformIndexing =
                native.shaderStorageImageArrayNonUniformIndexing;
            expected12.shaderInputAttachmentArrayNonUniformIndexing =
                native.shaderInputAttachmentArrayNonUniformIndexing;
            expected12.shaderUniformTexelBufferArrayNonUniformIndexing =
                native.shaderUniformTexelBufferArrayNonUniformIndexing;
            expected12.shaderStorageTexelBufferArrayNonUniformIndexing =
                native.shaderStorageTexelBufferArrayNonUniformIndexing;
            expected12.descriptorBindingUniformBufferUpdateAfterBind =
                native.descriptorBindingUniformBufferUpdateAfterBind;
            expected12.descriptorBindingSampledImageUpdateAfterBind =
                native.descriptorBindingSampledImageUpdateAfterBind;
            expected12.descriptorBindingStorageImageUpdateAfterBind =
                native.descriptorBindingStorageImageUpdateAfterBind;
            expected12.descriptorBindingStorageBufferUpdateAfterBind =
                native.descriptorBindingStorageBufferUpdateAfterBind;
            expected12.descriptorBindingUniformTexelBufferUpdateAfterBind =
                native.descriptorBindingUniformTexelBufferUpdateAfterBind;
            expected12.descriptorBindingStorageTexelBufferUpdateAfterBind =
                native.descriptorBindingStorageTexelBufferUpdateAfterBind;
            expected12.descriptorBindingUpdateUnusedWhilePending =
                native.descriptorBindingUpdateUnusedWhilePending;
            expected12.descriptorBindingPartiallyBound = native.descriptorBindingPartiallyBound;
            expected12.descriptorBindingVariableDescriptorCount =
                native.descriptorBindingVariableDescriptorCount;
            expected12.runtimeDescriptorArray = native.runtimeDescriptorArray;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_EXT_scalar_block_layout")) {
            VkPhysicalDeviceScalarBlockLayoutFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.scalarBlockLayout = native.scalarBlockLayout;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_imageless_framebuffer")) {
            VkPhysicalDeviceImagelessFramebufferFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.imagelessFramebuffer = native.imagelessFramebuffer;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_uniform_buffer_standard_layout")) {
            VkPhysicalDeviceUniformBufferStandardLayoutFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.uniformBufferStandardLayout = native.uniformBufferStandardLayout;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_shader_subgroup_extended_types")) {
            VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.shaderSubgroupExtendedTypes = native.shaderSubgroupExtendedTypes;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_separate_depth_stencil_layouts")) {
            VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.separateDepthStencilLayouts = native.separateDepthStencilLayouts;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_EXT_host_query_reset")) {
            VkPhysicalDeviceHostQueryResetFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.hostQueryReset = native.hostQueryReset;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_timeline_semaphore")) {
            VkPhysicalDeviceTimelineSemaphoreFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.timelineSemaphore = native.timelineSemaphore;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_buffer_device_address")) {
            VkPhysicalDeviceBufferDeviceAddressFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.bufferDeviceAddress = native.bufferDeviceAddress;
            expected12.bufferDeviceAddressCaptureReplay = native.bufferDeviceAddressCaptureReplay;
            expected12.bufferDeviceAddressMultiDevice = native.bufferDeviceAddressMultiDevice;
        }
        if (native_properties.apiVersion >= VK_API_VERSION_1_2 ||
            extensions.count("VK_KHR_vulkan_memory_model")) {
            VkPhysicalDeviceVulkanMemoryModelFeatures native{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
            VkPhysicalDeviceFeatures2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            direct.pNext = &native;
            raw_query(physical, &direct);
            expected12.vulkanMemoryModel = native.vulkanMemoryModel;
            expected12.vulkanMemoryModelDeviceScope = native.vulkanMemoryModelDeviceScope;
            expected12.vulkanMemoryModelAvailabilityVisibilityChains =
                native.vulkanMemoryModelAvailabilityVisibilityChains;
        }
    }
    check(actual11.storageBuffer16BitAccess == expected11.storageBuffer16BitAccess,
          "native field preserved: storageBuffer16BitAccess");
    check(actual11.uniformAndStorageBuffer16BitAccess ==
              expected11.uniformAndStorageBuffer16BitAccess,
          "native field preserved: uniformAndStorageBuffer16BitAccess");
    check(actual11.storagePushConstant16 == expected11.storagePushConstant16,
          "native field preserved: storagePushConstant16");
    check(actual11.storageInputOutput16 == expected11.storageInputOutput16,
          "native field preserved: storageInputOutput16");
    check(actual11.multiview == expected11.multiview, "native field preserved: multiview");
    check(actual11.multiviewGeometryShader == expected11.multiviewGeometryShader,
          "native field preserved: multiviewGeometryShader");
    check(actual11.multiviewTessellationShader == expected11.multiviewTessellationShader,
          "native field preserved: multiviewTessellationShader");
    check(actual11.variablePointersStorageBuffer == expected11.variablePointersStorageBuffer,
          "native field preserved: variablePointersStorageBuffer");
    check(actual11.variablePointers == expected11.variablePointers,
          "native field preserved: variablePointers");
    check(actual11.protectedMemory == expected11.protectedMemory,
          "native field preserved: protectedMemory");
    check(actual11.samplerYcbcrConversion == expected11.samplerYcbcrConversion,
          "native field preserved: samplerYcbcrConversion");
    check(actual11.shaderDrawParameters == expected11.shaderDrawParameters,
          "native field preserved: shaderDrawParameters");
    check(actual12.storageBuffer8BitAccess == expected12.storageBuffer8BitAccess,
          "native field preserved: storageBuffer8BitAccess");
    check(actual12.uniformAndStorageBuffer8BitAccess ==
              expected12.uniformAndStorageBuffer8BitAccess,
          "native field preserved: uniformAndStorageBuffer8BitAccess");
    check(actual12.storagePushConstant8 == expected12.storagePushConstant8,
          "native field preserved: storagePushConstant8");
    check(actual12.shaderBufferInt64Atomics == expected12.shaderBufferInt64Atomics,
          "native field preserved: shaderBufferInt64Atomics");
    check(actual12.shaderSharedInt64Atomics == expected12.shaderSharedInt64Atomics,
          "native field preserved: shaderSharedInt64Atomics");
    check(actual12.shaderFloat16 == expected12.shaderFloat16,
          "native field preserved: shaderFloat16");
    check(actual12.shaderInt8 == expected12.shaderInt8, "native field preserved: shaderInt8");
    check(actual12.shaderInputAttachmentArrayDynamicIndexing ==
              expected12.shaderInputAttachmentArrayDynamicIndexing,
          "native field preserved: shaderInputAttachmentArrayDynamicIndexing");
    check(actual12.shaderUniformTexelBufferArrayDynamicIndexing ==
              expected12.shaderUniformTexelBufferArrayDynamicIndexing,
          "native field preserved: shaderUniformTexelBufferArrayDynamicIndexing");
    check(actual12.shaderStorageTexelBufferArrayDynamicIndexing ==
              expected12.shaderStorageTexelBufferArrayDynamicIndexing,
          "native field preserved: shaderStorageTexelBufferArrayDynamicIndexing");
    check(actual12.shaderUniformBufferArrayNonUniformIndexing ==
              expected12.shaderUniformBufferArrayNonUniformIndexing,
          "native field preserved: shaderUniformBufferArrayNonUniformIndexing");
    check(actual12.shaderSampledImageArrayNonUniformIndexing ==
              expected12.shaderSampledImageArrayNonUniformIndexing,
          "native field preserved: shaderSampledImageArrayNonUniformIndexing");
    check(actual12.shaderStorageBufferArrayNonUniformIndexing ==
              expected12.shaderStorageBufferArrayNonUniformIndexing,
          "native field preserved: shaderStorageBufferArrayNonUniformIndexing");
    check(actual12.shaderStorageImageArrayNonUniformIndexing ==
              expected12.shaderStorageImageArrayNonUniformIndexing,
          "native field preserved: shaderStorageImageArrayNonUniformIndexing");
    check(actual12.shaderInputAttachmentArrayNonUniformIndexing ==
              expected12.shaderInputAttachmentArrayNonUniformIndexing,
          "native field preserved: shaderInputAttachmentArrayNonUniformIndexing");
    check(actual12.shaderUniformTexelBufferArrayNonUniformIndexing ==
              expected12.shaderUniformTexelBufferArrayNonUniformIndexing,
          "native field preserved: shaderUniformTexelBufferArrayNonUniformIndexing");
    check(actual12.shaderStorageTexelBufferArrayNonUniformIndexing ==
              expected12.shaderStorageTexelBufferArrayNonUniformIndexing,
          "native field preserved: shaderStorageTexelBufferArrayNonUniformIndexing");
    check(actual12.descriptorBindingUniformBufferUpdateAfterBind ==
              expected12.descriptorBindingUniformBufferUpdateAfterBind,
          "native field preserved: descriptorBindingUniformBufferUpdateAfterBind");
    check(actual12.descriptorBindingSampledImageUpdateAfterBind ==
              expected12.descriptorBindingSampledImageUpdateAfterBind,
          "native field preserved: descriptorBindingSampledImageUpdateAfterBind");
    check(actual12.descriptorBindingStorageImageUpdateAfterBind ==
              expected12.descriptorBindingStorageImageUpdateAfterBind,
          "native field preserved: descriptorBindingStorageImageUpdateAfterBind");
    check(actual12.descriptorBindingStorageBufferUpdateAfterBind ==
              expected12.descriptorBindingStorageBufferUpdateAfterBind,
          "native field preserved: descriptorBindingStorageBufferUpdateAfterBind");
    check(actual12.descriptorBindingUniformTexelBufferUpdateAfterBind ==
              expected12.descriptorBindingUniformTexelBufferUpdateAfterBind,
          "native field preserved: descriptorBindingUniformTexelBufferUpdateAfterBind");
    check(actual12.descriptorBindingStorageTexelBufferUpdateAfterBind ==
              expected12.descriptorBindingStorageTexelBufferUpdateAfterBind,
          "native field preserved: descriptorBindingStorageTexelBufferUpdateAfterBind");
    check(actual12.descriptorBindingUpdateUnusedWhilePending ==
              expected12.descriptorBindingUpdateUnusedWhilePending,
          "native field preserved: descriptorBindingUpdateUnusedWhilePending");
    check(actual12.descriptorBindingPartiallyBound == expected12.descriptorBindingPartiallyBound,
          "native field preserved: descriptorBindingPartiallyBound");
    check(actual12.descriptorBindingVariableDescriptorCount ==
              expected12.descriptorBindingVariableDescriptorCount,
          "native field preserved: descriptorBindingVariableDescriptorCount");
    check(actual12.runtimeDescriptorArray == expected12.runtimeDescriptorArray,
          "native field preserved: runtimeDescriptorArray");
    check(actual12.scalarBlockLayout == expected12.scalarBlockLayout,
          "native field preserved: scalarBlockLayout");
    check(actual12.imagelessFramebuffer == expected12.imagelessFramebuffer,
          "native field preserved: imagelessFramebuffer");
    check(actual12.uniformBufferStandardLayout == expected12.uniformBufferStandardLayout,
          "native field preserved: uniformBufferStandardLayout");
    check(actual12.shaderSubgroupExtendedTypes == expected12.shaderSubgroupExtendedTypes,
          "native field preserved: shaderSubgroupExtendedTypes");
    check(actual12.separateDepthStencilLayouts == expected12.separateDepthStencilLayouts,
          "native field preserved: separateDepthStencilLayouts");
    check(actual12.hostQueryReset == VK_TRUE, "mandatory emulation available: hostQueryReset");
    check(actual12.timelineSemaphore == VK_TRUE,
          "mandatory emulation available: timelineSemaphore");
    check(actual12.bufferDeviceAddress == expected12.bufferDeviceAddress,
          "native field preserved: bufferDeviceAddress");
    check(actual12.bufferDeviceAddressCaptureReplay == expected12.bufferDeviceAddressCaptureReplay,
          "native field preserved: bufferDeviceAddressCaptureReplay");
    check(actual12.bufferDeviceAddressMultiDevice == expected12.bufferDeviceAddressMultiDevice,
          "native field preserved: bufferDeviceAddressMultiDevice");
    check(actual12.vulkanMemoryModel == expected12.vulkanMemoryModel,
          "native field preserved: vulkanMemoryModel");
    check(actual12.vulkanMemoryModelDeviceScope == expected12.vulkanMemoryModelDeviceScope,
          "native field preserved: vulkanMemoryModelDeviceScope");
    check(actual12.vulkanMemoryModelAvailabilityVisibilityChains ==
              expected12.vulkanMemoryModelAvailabilityVisibilityChains,
          "native field preserved: vulkanMemoryModelAvailabilityVisibilityChains");

    VkPhysicalDeviceVulkan11Features enable11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features enable12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enable11.multiview = actual11.multiview;
    enable11.storageBuffer16BitAccess = actual11.storageBuffer16BitAccess;
    enable12.shaderInt8 = actual12.shaderInt8;
    enable12.runtimeDescriptorArray = actual12.runtimeDescriptorArray;
    enable12.timelineSemaphore = actual12.timelineSemaphore;
    enable12.hostQueryReset = actual12.hostQueryReset;
    enable11.pNext = &enable12;
    const auto saved11 = enable11;
    const auto saved12 = enable12;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            family = i;
            break;
        }
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue;
    device_info.pNext = &enable11;
    VkDevice device{};
    if (ok(vkCreateDevice(physical, &device_info, nullptr, &device),
           "create device using aggregate features on old driver")) {
        check(!memcmp(&enable11, &saved11, sizeof(enable11)) &&
                  !memcmp(&enable12, &saved12, sizeof(enable12)),
              "real device creation leaves caller feature structures unchanged");
        vkDeviceWaitIdle(device);
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    dlclose(library);
    printf("Core GPU feature failures: %u\n", failures);
    return failures ? 1 : 0;
}

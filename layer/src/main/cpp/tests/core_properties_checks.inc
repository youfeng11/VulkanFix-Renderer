// Generated raw-driver property comparisons; do not edit.
if (native_api >= VK_API_VERSION_1_1 || instance_extensions.count("VK_KHR_external_memory_capabilities")) {
    VkPhysicalDeviceIDProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual11.deviceUUID, &native.deviceUUID, sizeof(native.deviceUUID)), "native property: deviceUUID");
    check(!memcmp(&actual11.driverUUID, &native.driverUUID, sizeof(native.driverUUID)), "native property: driverUUID");
    check(!memcmp(&actual11.deviceLUID, &native.deviceLUID, sizeof(native.deviceLUID)), "native property: deviceLUID");
    check(!memcmp(&actual11.deviceNodeMask, &native.deviceNodeMask, sizeof(native.deviceNodeMask)), "native property: deviceNodeMask");
    check(!memcmp(&actual11.deviceLUIDValid, &native.deviceLUIDValid, sizeof(native.deviceLUIDValid)), "native property: deviceLUIDValid");
}
if (native_api >= VK_API_VERSION_1_1) {
    VkPhysicalDeviceSubgroupProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual11.subgroupSize, &native.subgroupSize, sizeof(native.subgroupSize)), "native property: subgroupSize");
    check(!memcmp(&actual11.subgroupSupportedStages, &native.supportedStages, sizeof(native.supportedStages)), "native property: subgroupSupportedStages");
    check(!memcmp(&actual11.subgroupSupportedOperations, &native.supportedOperations, sizeof(native.supportedOperations)), "native property: subgroupSupportedOperations");
    check(!memcmp(&actual11.subgroupQuadOperationsInAllStages, &native.quadOperationsInAllStages, sizeof(native.quadOperationsInAllStages)), "native property: subgroupQuadOperationsInAllStages");
}
if (native_api >= VK_API_VERSION_1_1 || extensions.count("VK_KHR_maintenance2")) {
    VkPhysicalDevicePointClippingProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual11.pointClippingBehavior, &native.pointClippingBehavior, sizeof(native.pointClippingBehavior)), "native property: pointClippingBehavior");
}
if (native_api >= VK_API_VERSION_1_1 || extensions.count("VK_KHR_multiview")) {
    VkPhysicalDeviceMultiviewProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual11.maxMultiviewViewCount, &native.maxMultiviewViewCount, sizeof(native.maxMultiviewViewCount)), "native property: maxMultiviewViewCount");
    check(!memcmp(&actual11.maxMultiviewInstanceIndex, &native.maxMultiviewInstanceIndex, sizeof(native.maxMultiviewInstanceIndex)), "native property: maxMultiviewInstanceIndex");
}
if (native_api >= VK_API_VERSION_1_1) {
    VkPhysicalDeviceProtectedMemoryProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual11.protectedNoFault, &native.protectedNoFault, sizeof(native.protectedNoFault)), "native property: protectedNoFault");
}
if (native_api >= VK_API_VERSION_1_1 || extensions.count("VK_KHR_maintenance3")) {
    VkPhysicalDeviceMaintenance3Properties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual11.maxPerSetDescriptors, &native.maxPerSetDescriptors, sizeof(native.maxPerSetDescriptors)), "native property: maxPerSetDescriptors");
    check(!memcmp(&actual11.maxMemoryAllocationSize, &native.maxMemoryAllocationSize, sizeof(native.maxMemoryAllocationSize)), "native property: maxMemoryAllocationSize");
}
if (native_api >= VK_API_VERSION_1_2 || extensions.count("VK_KHR_driver_properties")) {
    VkPhysicalDeviceDriverProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual12.driverID, &native.driverID, sizeof(native.driverID)), "native property: driverID");
    check(!memcmp(&actual12.conformanceVersion, &native.conformanceVersion, sizeof(native.conformanceVersion)), "native property: conformanceVersion");
}
if (native_api >= VK_API_VERSION_1_2 || extensions.count("VK_KHR_shader_float_controls")) {
    VkPhysicalDeviceFloatControlsProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual12.denormBehaviorIndependence, &native.denormBehaviorIndependence, sizeof(native.denormBehaviorIndependence)), "native property: denormBehaviorIndependence");
    check(!memcmp(&actual12.roundingModeIndependence, &native.roundingModeIndependence, sizeof(native.roundingModeIndependence)), "native property: roundingModeIndependence");
    check(!memcmp(&actual12.shaderSignedZeroInfNanPreserveFloat16, &native.shaderSignedZeroInfNanPreserveFloat16, sizeof(native.shaderSignedZeroInfNanPreserveFloat16)), "native property: shaderSignedZeroInfNanPreserveFloat16");
    check(!memcmp(&actual12.shaderSignedZeroInfNanPreserveFloat32, &native.shaderSignedZeroInfNanPreserveFloat32, sizeof(native.shaderSignedZeroInfNanPreserveFloat32)), "native property: shaderSignedZeroInfNanPreserveFloat32");
    check(!memcmp(&actual12.shaderSignedZeroInfNanPreserveFloat64, &native.shaderSignedZeroInfNanPreserveFloat64, sizeof(native.shaderSignedZeroInfNanPreserveFloat64)), "native property: shaderSignedZeroInfNanPreserveFloat64");
    check(!memcmp(&actual12.shaderDenormPreserveFloat16, &native.shaderDenormPreserveFloat16, sizeof(native.shaderDenormPreserveFloat16)), "native property: shaderDenormPreserveFloat16");
    check(!memcmp(&actual12.shaderDenormPreserveFloat32, &native.shaderDenormPreserveFloat32, sizeof(native.shaderDenormPreserveFloat32)), "native property: shaderDenormPreserveFloat32");
    check(!memcmp(&actual12.shaderDenormPreserveFloat64, &native.shaderDenormPreserveFloat64, sizeof(native.shaderDenormPreserveFloat64)), "native property: shaderDenormPreserveFloat64");
    check(!memcmp(&actual12.shaderDenormFlushToZeroFloat16, &native.shaderDenormFlushToZeroFloat16, sizeof(native.shaderDenormFlushToZeroFloat16)), "native property: shaderDenormFlushToZeroFloat16");
    check(!memcmp(&actual12.shaderDenormFlushToZeroFloat32, &native.shaderDenormFlushToZeroFloat32, sizeof(native.shaderDenormFlushToZeroFloat32)), "native property: shaderDenormFlushToZeroFloat32");
    check(!memcmp(&actual12.shaderDenormFlushToZeroFloat64, &native.shaderDenormFlushToZeroFloat64, sizeof(native.shaderDenormFlushToZeroFloat64)), "native property: shaderDenormFlushToZeroFloat64");
    check(!memcmp(&actual12.shaderRoundingModeRTEFloat16, &native.shaderRoundingModeRTEFloat16, sizeof(native.shaderRoundingModeRTEFloat16)), "native property: shaderRoundingModeRTEFloat16");
    check(!memcmp(&actual12.shaderRoundingModeRTEFloat32, &native.shaderRoundingModeRTEFloat32, sizeof(native.shaderRoundingModeRTEFloat32)), "native property: shaderRoundingModeRTEFloat32");
    check(!memcmp(&actual12.shaderRoundingModeRTEFloat64, &native.shaderRoundingModeRTEFloat64, sizeof(native.shaderRoundingModeRTEFloat64)), "native property: shaderRoundingModeRTEFloat64");
    check(!memcmp(&actual12.shaderRoundingModeRTZFloat16, &native.shaderRoundingModeRTZFloat16, sizeof(native.shaderRoundingModeRTZFloat16)), "native property: shaderRoundingModeRTZFloat16");
    check(!memcmp(&actual12.shaderRoundingModeRTZFloat32, &native.shaderRoundingModeRTZFloat32, sizeof(native.shaderRoundingModeRTZFloat32)), "native property: shaderRoundingModeRTZFloat32");
    check(!memcmp(&actual12.shaderRoundingModeRTZFloat64, &native.shaderRoundingModeRTZFloat64, sizeof(native.shaderRoundingModeRTZFloat64)), "native property: shaderRoundingModeRTZFloat64");
}
if (native_api >= VK_API_VERSION_1_2 || extensions.count("VK_EXT_descriptor_indexing")) {
    VkPhysicalDeviceDescriptorIndexingProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual12.maxUpdateAfterBindDescriptorsInAllPools, &native.maxUpdateAfterBindDescriptorsInAllPools, sizeof(native.maxUpdateAfterBindDescriptorsInAllPools)), "native property: maxUpdateAfterBindDescriptorsInAllPools");
    check(!memcmp(&actual12.shaderUniformBufferArrayNonUniformIndexingNative, &native.shaderUniformBufferArrayNonUniformIndexingNative, sizeof(native.shaderUniformBufferArrayNonUniformIndexingNative)), "native property: shaderUniformBufferArrayNonUniformIndexingNative");
    check(!memcmp(&actual12.shaderSampledImageArrayNonUniformIndexingNative, &native.shaderSampledImageArrayNonUniformIndexingNative, sizeof(native.shaderSampledImageArrayNonUniformIndexingNative)), "native property: shaderSampledImageArrayNonUniformIndexingNative");
    check(!memcmp(&actual12.shaderStorageBufferArrayNonUniformIndexingNative, &native.shaderStorageBufferArrayNonUniformIndexingNative, sizeof(native.shaderStorageBufferArrayNonUniformIndexingNative)), "native property: shaderStorageBufferArrayNonUniformIndexingNative");
    check(!memcmp(&actual12.shaderStorageImageArrayNonUniformIndexingNative, &native.shaderStorageImageArrayNonUniformIndexingNative, sizeof(native.shaderStorageImageArrayNonUniformIndexingNative)), "native property: shaderStorageImageArrayNonUniformIndexingNative");
    check(!memcmp(&actual12.shaderInputAttachmentArrayNonUniformIndexingNative, &native.shaderInputAttachmentArrayNonUniformIndexingNative, sizeof(native.shaderInputAttachmentArrayNonUniformIndexingNative)), "native property: shaderInputAttachmentArrayNonUniformIndexingNative");
    check(!memcmp(&actual12.robustBufferAccessUpdateAfterBind, &native.robustBufferAccessUpdateAfterBind, sizeof(native.robustBufferAccessUpdateAfterBind)), "native property: robustBufferAccessUpdateAfterBind");
    check(!memcmp(&actual12.quadDivergentImplicitLod, &native.quadDivergentImplicitLod, sizeof(native.quadDivergentImplicitLod)), "native property: quadDivergentImplicitLod");
    check(!memcmp(&actual12.maxPerStageDescriptorUpdateAfterBindSamplers, &native.maxPerStageDescriptorUpdateAfterBindSamplers, sizeof(native.maxPerStageDescriptorUpdateAfterBindSamplers)), "native property: maxPerStageDescriptorUpdateAfterBindSamplers");
    check(!memcmp(&actual12.maxPerStageDescriptorUpdateAfterBindUniformBuffers, &native.maxPerStageDescriptorUpdateAfterBindUniformBuffers, sizeof(native.maxPerStageDescriptorUpdateAfterBindUniformBuffers)), "native property: maxPerStageDescriptorUpdateAfterBindUniformBuffers");
    check(!memcmp(&actual12.maxPerStageDescriptorUpdateAfterBindStorageBuffers, &native.maxPerStageDescriptorUpdateAfterBindStorageBuffers, sizeof(native.maxPerStageDescriptorUpdateAfterBindStorageBuffers)), "native property: maxPerStageDescriptorUpdateAfterBindStorageBuffers");
    check(!memcmp(&actual12.maxPerStageDescriptorUpdateAfterBindSampledImages, &native.maxPerStageDescriptorUpdateAfterBindSampledImages, sizeof(native.maxPerStageDescriptorUpdateAfterBindSampledImages)), "native property: maxPerStageDescriptorUpdateAfterBindSampledImages");
    check(!memcmp(&actual12.maxPerStageDescriptorUpdateAfterBindStorageImages, &native.maxPerStageDescriptorUpdateAfterBindStorageImages, sizeof(native.maxPerStageDescriptorUpdateAfterBindStorageImages)), "native property: maxPerStageDescriptorUpdateAfterBindStorageImages");
    check(!memcmp(&actual12.maxPerStageDescriptorUpdateAfterBindInputAttachments, &native.maxPerStageDescriptorUpdateAfterBindInputAttachments, sizeof(native.maxPerStageDescriptorUpdateAfterBindInputAttachments)), "native property: maxPerStageDescriptorUpdateAfterBindInputAttachments");
    check(!memcmp(&actual12.maxPerStageUpdateAfterBindResources, &native.maxPerStageUpdateAfterBindResources, sizeof(native.maxPerStageUpdateAfterBindResources)), "native property: maxPerStageUpdateAfterBindResources");
    check(!memcmp(&actual12.maxDescriptorSetUpdateAfterBindSamplers, &native.maxDescriptorSetUpdateAfterBindSamplers, sizeof(native.maxDescriptorSetUpdateAfterBindSamplers)), "native property: maxDescriptorSetUpdateAfterBindSamplers");
    check(!memcmp(&actual12.maxDescriptorSetUpdateAfterBindUniformBuffers, &native.maxDescriptorSetUpdateAfterBindUniformBuffers, sizeof(native.maxDescriptorSetUpdateAfterBindUniformBuffers)), "native property: maxDescriptorSetUpdateAfterBindUniformBuffers");
    check(!memcmp(&actual12.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic, &native.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic, sizeof(native.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic)), "native property: maxDescriptorSetUpdateAfterBindUniformBuffersDynamic");
    check(!memcmp(&actual12.maxDescriptorSetUpdateAfterBindStorageBuffers, &native.maxDescriptorSetUpdateAfterBindStorageBuffers, sizeof(native.maxDescriptorSetUpdateAfterBindStorageBuffers)), "native property: maxDescriptorSetUpdateAfterBindStorageBuffers");
    check(!memcmp(&actual12.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic, &native.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic, sizeof(native.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic)), "native property: maxDescriptorSetUpdateAfterBindStorageBuffersDynamic");
    check(!memcmp(&actual12.maxDescriptorSetUpdateAfterBindSampledImages, &native.maxDescriptorSetUpdateAfterBindSampledImages, sizeof(native.maxDescriptorSetUpdateAfterBindSampledImages)), "native property: maxDescriptorSetUpdateAfterBindSampledImages");
    check(!memcmp(&actual12.maxDescriptorSetUpdateAfterBindStorageImages, &native.maxDescriptorSetUpdateAfterBindStorageImages, sizeof(native.maxDescriptorSetUpdateAfterBindStorageImages)), "native property: maxDescriptorSetUpdateAfterBindStorageImages");
    check(!memcmp(&actual12.maxDescriptorSetUpdateAfterBindInputAttachments, &native.maxDescriptorSetUpdateAfterBindInputAttachments, sizeof(native.maxDescriptorSetUpdateAfterBindInputAttachments)), "native property: maxDescriptorSetUpdateAfterBindInputAttachments");
}
if (native_api >= VK_API_VERSION_1_2 || extensions.count("VK_KHR_depth_stencil_resolve")) {
    VkPhysicalDeviceDepthStencilResolveProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual12.supportedDepthResolveModes, &native.supportedDepthResolveModes, sizeof(native.supportedDepthResolveModes)), "native property: supportedDepthResolveModes");
    check(!memcmp(&actual12.supportedStencilResolveModes, &native.supportedStencilResolveModes, sizeof(native.supportedStencilResolveModes)), "native property: supportedStencilResolveModes");
    check(!memcmp(&actual12.independentResolveNone, &native.independentResolveNone, sizeof(native.independentResolveNone)), "native property: independentResolveNone");
    check(!memcmp(&actual12.independentResolve, &native.independentResolve, sizeof(native.independentResolve)), "native property: independentResolve");
}
if (native_api >= VK_API_VERSION_1_2 || extensions.count("VK_EXT_sampler_filter_minmax")) {
    VkPhysicalDeviceSamplerFilterMinmaxProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual12.filterMinmaxSingleComponentFormats, &native.filterMinmaxSingleComponentFormats, sizeof(native.filterMinmaxSingleComponentFormats)), "native property: filterMinmaxSingleComponentFormats");
    check(!memcmp(&actual12.filterMinmaxImageComponentMapping, &native.filterMinmaxImageComponentMapping, sizeof(native.filterMinmaxImageComponentMapping)), "native property: filterMinmaxImageComponentMapping");
}
if (native_api >= VK_API_VERSION_1_2 || extensions.count("VK_KHR_timeline_semaphore")) {
    VkPhysicalDeviceTimelineSemaphoreProperties native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES};
    VkPhysicalDeviceProperties2 direct{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native};
    raw_query(physical, &direct);
    check(!memcmp(&actual12.maxTimelineSemaphoreValueDifference, &native.maxTimelineSemaphoreValueDifference, sizeof(native.maxTimelineSemaphoreValueDifference)), "native property: maxTimelineSemaphoreValueDifference");
}

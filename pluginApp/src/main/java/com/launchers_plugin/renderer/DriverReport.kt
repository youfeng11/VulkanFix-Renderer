package com.launchers_plugin.renderer

import org.json.JSONObject

internal data class DriverCheck(
    val name: String,
    val status: String,
    val evidence: String,
    val detail: String
) {
    val title: String get() = checkTitle(name)
    val statusText: String get() = when (status) {
        "PASS" -> if (evidence == "query") "支持" else "通过"
        "FAIL" -> if (evidence == "query") "未提供" else "异常"
        "WARN" -> if (detail.startsWith("repair-confirmed:")) "修复可用" else "需关注"
        "SKIP" -> "未测试"
        else -> "信息"
    }
}

internal class DriverReport(raw: String) {
    val rows: List<DriverCheck> = raw.lineSequence().filter { it.isNotBlank() }.map { line ->
        try {
            val json = JSONObject(line)
            DriverCheck(json.optString("name"), json.optString("status"),
                json.optString("evidence"), json.optString("detail"))
        } catch (_: Exception) {
            DriverCheck("report", "FAIL", "error", "报告行无法解析\n$line")
        }
    }.toList()
    val complete = rows.lastOrNull()?.evidence == "completion"
    val checks = rows.filter { it.evidence in listOf("functional", "error") && it.name != "core11.summary" }
    val failures = checks.filter { it.status == "FAIL" }
    val warnings = checks.filter { it.status == "WARN" }
    val passed = checks.filter { it.status == "PASS" }
    val skipped = checks.filter { it.status == "SKIP" }
    val capabilities = rows.filter { it.evidence == "query" }
    private val device = rows.firstOrNull { it.name == "device" }?.detail.orEmpty()
    val deviceName = device.substringAfter(": ", "").substringBefore("; API ")
    val apiVersion = device.substringAfter("; API ", "").substringBefore(";")
}

private val checkTitles = mapOf(
    "loader" to "Vulkan 加载器",
    "device" to "设备信息",
    "probe" to "检测中止",
    "report" to "报告读取",
    "complete" to "检测结束",
    "VK_KHR_dynamic_rendering" to "动态渲染",
    "VK_KHR_push_descriptor" to "推送描述符",
    "VK_KHR_synchronization2" to "同步 2",
    "VK_EXT_vertex_attribute_divisor" to "实例属性除数",
    "VK_KHR_swapchain" to "交换链",
    "multiDrawIndirect" to "多重间接绘制",
    "fillModeNonSolid" to "线框与点绘制",
    "drawIndirectFirstInstance" to "间接绘制首实例",
    "samplerAnisotropy" to "各向异性过滤",
    "vertexPipelineStoresAndAtomics" to "顶点存储与原子操作",
    "shaderDrawParameters" to "着色器绘制参数",
    "storageBuffer16BitAccess" to "16 位存储缓冲区",
    "samplerYcbcrConversion" to "YCbCr 采样转换",
    "timelineSemaphore" to "时间线信号量",
    "hostQueryReset" to "主机查询池重置",
    "synchronization2" to "同步 2 特性",
    "dynamicRendering" to "动态渲染特性",
    "vertexAttributeInstanceRateDivisor" to "实例属性步进",
    "driverTrust.samplerAliases" to "采样器别名",
    "driverTrust.largeUniform" to "大型统一缓冲区",
    "driverTrust.pushDescriptor" to "推送描述符读写",
    "driverTrust.tilerPressure" to "分块渲染压力",
    "driverTrust.msaaArrayResolve" to "多重采样数组解析",
    "driverTrust.crossFormatImageSlices" to "跨格式图像分层复制",
    "driverTrust.rasterizerDiscardState" to "光栅化丢弃后的动态状态",
    "driverTrust.storageArrayStatic" to "存储缓冲区数组静态索引",
    "driverTrust.storageArrayDynamic" to "存储缓冲区数组动态索引",
    "driverTrust.depthStencilResolve" to "深度与模板解析",
    "driverTrust.rgb9e5TransferBuffer" to "RGB9_E5 缓冲区传输",
    "driverTrust.rgb9e5TransferImage" to "RGB9_E5 图像传输",
    "driverTrust.queue2" to "设备队列获取",
    "driverTrust.timelineQueue" to "时间线队列依赖",
    "driverTrust.sync2Events" to "同步 2 事件",
    "driverTrust.sync2Submit" to "同步 2 队列提交",
    "driverTrust.timestampQueries" to "GPU 时间戳",
    "driverTrust.hostQueryReset" to "原生主机查询池重置",
    "driverTrust.workerControl" to "独立探测进程",
    "bufferTransfer" to "缓冲区传输",
    "core11.requiredCapabilities" to "Vulkan 1.1 必需能力",
    "Vulkan 1.0.0 Core" to "Vulkan 1.0 核心入口",
    "Vulkan 1.1.0 Core" to "Vulkan 1.1 核心入口",
    "Vulkan 1.2.0 Core" to "Vulkan 1.2 核心入口",
    "core11.queryChain" to "能力查询链",
    "core11.externalProperties" to "外部资源能力",
    "core11.propertiesAggregate" to "属性查询一致性",
    "core11.featureAggregate" to "特性查询一致性",
    "core11.multiviewRenderPass" to "多视图渲染",
    "core11.sparseMemoryRequirements2" to "稀疏图像内存查询",
    "core11.aggregateRepair" to "聚合查询兼容",
    "core11.dispatchBase" to "计算工作组基址",
    "core11.dispatchBaseNonZero" to "非零工作组基址",
    "core11.dispatchBaseNonZero2" to "多次工作组基址",
    "core11.dispatchBaseYZ" to "三维工作组基址",
    "core11.dispatchBaseImage" to "工作组基址图像写入",
    "core11.dispatchBaseAtomic" to "工作组基址原子操作",
    "core11.storage16" to "16 位存储读写",
    "core11.variablePointers" to "可变指针",
    "core11.variablePointersStorageBuffer" to "存储缓冲区可变指针",
    "core11.spirv13Downgrade" to "SPIR-V 着色器转换",
    "core11.subgroupBasic" to "子组基础操作",
    "core11.relaxedBlockLayout" to "紧凑向量缓冲区布局",
    "core11.tessellationDomainOriginUpper" to "曲面细分左上原点",
    "core11.tessellationDomainOriginLower" to "曲面细分左下原点",
    "core11.inputAttachmentAspects" to "输入附件颜色与深度声明",
    "core11.formatTransferFeatures" to "图像格式复制能力",
    "core11.imageTypeCopy" to "数组与三维图像复制",
    "core11.volumeAttachment" to "三维切片渲染与采样",
    "core11.volumeInputAttachment" to "三维切片子通道读取",
    "core11.storageBufferArrays" to "存储缓冲区数组动态写回",
    "core11.storageBufferArraysStatic" to "存储缓冲区数组静态写回",
    "core11.negativeViewport" to "负高度视口",
    "core11.dynamicNegativeViewport" to "动态负高度视口",
    "core11.staticViewportPositionRead" to "静态视口位置读写",
    "core11.dynamicViewportPositionRead" to "动态视口位置读写",
    "core11.staticViewportGeometry" to "几何阶段静态视口",
    "core11.dynamicViewportGeometry" to "几何阶段动态视口",
    "core11.staticViewportTessellation" to "曲面细分静态视口",
    "core11.dynamicViewportTessellation" to "曲面细分动态视口",
    "core11.dynamicNegativeViewportIndirect" to "间接绘制负高度视口",
    "core11.dynamicNegativeViewportIndexed" to "索引绘制负高度视口",
    "core11.dynamicNegativeViewportIndexedIndirect" to "索引间接绘制负高度视口",
    "core11.dynamicPositiveViewport" to "动态正高度视口",
    "core11.staticMixedViewport" to "静态多视口",
    "core11.dynamicMixedViewport" to "动态多视口",
    "core11.staticMixedNegativeViewport" to "静态混合高度视口",
    "core11.dynamicMixedNegativeViewport" to "动态混合高度视口",
    "core11.staticConstantViewportIndex" to "静态视口索引",
    "core11.dynamicConstantViewportIndex" to "动态视口索引",
    "deviceQueue2.sameQueue" to "设备队列一致性",
    "core11.bufferMemory2" to "缓冲区内存绑定",
    "core11.imageMemory2" to "图像内存绑定",
    "core11.descriptorUpdateTemplate" to "描述符更新模板",
    "core11.descriptorSetLayoutSupport" to "描述符布局支持",
    "core11.samplerYcbcrConversion" to "YCbCr 转换读回",
    "deviceGroup.enumerate" to "设备组枚举",
    "timelineSemaphore.hostSignalWait" to "主机信号量通知与等待",
    "hostQueryReset.reset" to "查询池局部重置",
    "synchronization2.transfer" to "同步 2 传输",
    "dynamicRendering.clear" to "动态渲染清屏",
    "pushDescriptor.compute" to "推送描述符计算",
    "graphics.draw" to "基础绘制",
    "multiDrawIndirect.draw" to "多重间接绘制读回",
    "drawIndirectFirstInstance.draw" to "首实例间接绘制读回",
    "vertexAttributeInstanceRateDivisor.draw" to "实例属性绘制",
    "fillModeNonSolid.line" to "线框绘制",
    "shaderDrawParameters.draw" to "绘制参数读回",
    "vertexPipelineStoresAndAtomics.draw" to "顶点存储与原子操作读回",
    "samplerAnisotropy.sample" to "各向异性采样",
    "VK_KHR_swapchain.present" to "画面呈现"
)

internal fun checkTitle(name: String): String = checkTitles[name] ?: name

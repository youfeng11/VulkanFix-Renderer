# VulkanFix

Android 端 Minecraft 的 Vulkan 兼容插件，适用于 FCL、ZalithLauncher 2 等支持渲染器插件的启动器，面向 Iris Vulkan 等 Vulkan 渲染模组。

在原生 Vulkan 驱动之上补充部分缺失的接口和特性。设备需要具备 Vulkan 驱动，插件最低支持 Android 8.0。

## 使用

1. 安装插件 APK。
2. 在启动器的渲染器设置中选择 **VulkanFix**。
3. 将游戏或渲染模组的图形 API 设为 **Vulkan**，再启动游戏。

## 功能与限制

- Vulkan 1.1 / 1.2 部分核心接口、特性查询和扩展兼容。
- 动态渲染、同步 2、push descriptor 和 timeline semaphore 的兼容实现。
- SPIR-V 着色器转换、管线缓存及部分绘制功能模拟。

顶点存储模拟目前不支持间接绘制、传统 render pass、几何和曲面细分阶段。

完整 Vulkan 1.1 / 1.2 核心兼容仍在开发中。

反馈问题时，请附设备型号、驱动信息、游戏与模组版本、光影包版本，以及相关日志。

## 编译

需要 JDK 17、Android SDK 34、NDK r27d（`27.3.13750724`）和 CMake 3.20 或更新版本。

```sh
git clone --recurse-submodules https://github.com/youfeng11/VulkanFix-Renderer.git
cd VulkanFix-Renderer
./gradlew :pluginApp:assembleDebug
```

Windows 使用 `gradlew.bat :pluginApp:assembleDebug`。已有仓库可运行 `git submodule update --init --recursive` 补齐子模块。

APK 输出到 `pluginApp/build/outputs/apk/debug/`，包含 `arm64-v8a`、`armeabi-v7a`、`x86` 和 `x86_64` 四种架构。
主库文件名为 `libvulkanfix.so`。

Release 使用 `./gradlew :pluginApp:assembleRelease`，输出到 `pluginApp/build/outputs/apk/release/`。
添加 `-PsplitApks=true` 可同时生成各架构的独立 APK 和通用 APK，独立 APK 的下载体积更小。

## 设备驱动自检

游戏启动时会自动测量 sampler 别名和大型 UBO 的读回结果。原始路径出错、修复对照前后均正确时，才启用 sampler 去重或未使用 UBO 成员裁剪；健康驱动保持原始 shader。控制失败或不支持测试条件时，报告“不确定”，不据此推断驱动缺陷。

原生 push descriptor 还需通过独立进程内的跨管线布局测试，未通过确认时使用 descriptor set 模拟。测试进程 `libvulkanfix_probe.so` 随 APK 安装，崩溃和超时由父进程处理。结果按设备、驱动及探针版本在进程内复用，不依赖厂商名单。`VULKAN_FIX_DEDUP_SAMPLER_ALIASES=0/1`、`VULKAN_FIX_PRUNE_UNUSED_UNIFORM_MEMBERS=0/1` 仍可用于诊断对照；push descriptor 保留启动器的 force/skip 设置。

tiler 保护通过直接绘制、间接绘制及可用的多重间接绘制压力探针选择，比较单个 render pass 与分批 LOAD 延续的输出。全部通过时保留原始路径；测出异常或无法确定时保留有界批次保护。需要保护的设备会使用可延续的 render pass 模拟动态渲染。`VULKAN_FIX_MALI_TILER_GUARD=1/0` 可强制启用或关闭，名称仅为兼容已有设置，判定不再限定 Mali。压力探针是代表性负载，仍需按实际游戏和光影回归。

打开 VulkanFix 应用，分别点击“检测原生驱动”和“检测 VulkanFix”。报告区分驱动声明的能力与实际执行结果，可复制用于反馈。每轮在独立进程和测试窗口中运行，超时或驱动崩溃不会结束游戏进程。

- Core：检查 Vulkan 1.0、1.1、1.2 的全部 178 个命令入口，并执行内存传输、离屏绘制、timeline semaphore 和 host query reset 等代表性探针。Vulkan 1.1 还验证 queue-family/memory/format/image-format/sparse-image-format 的 1.1 查询与 legacy 结果一致（format/memory/queue-family/image-format/sparse-image-format 原生结果错误会按 legacy 修复）、buffer/fence/semaphore external properties，`VkPhysicalDeviceVulkan11Features` 与 `VkPhysicalDeviceVulkan11Properties` 聚合查询与各原生子结构一致性（原生聚合值不一致时按子结构重建修复）、`vkGetDeviceQueue2(flags=0)` 与 `vkGetDeviceQueue` 的队列一致性、`vkGetBufferMemoryRequirements2`/`vkBindBufferMemory2` 和 `vkGetImageMemoryRequirements2`/`vkBindImageMemory2` 与 1.0 路径的一致性（原生 size/alignment 错误会按 legacy 查询修复）、descriptor update template 的创建和更新、descriptor set layout support、sampler YCbCr conversion、`vkCmdSetDeviceMask`/`vkCmdDispatchBase(base=0 与 base=1，后者使用 DISPATCH_BASE 管线；无原生入口时用 base 偏移的 compute pipeline 变体回退，并验证 SSBO、imageStore 坐标、atomicAdd 索引和 Y/Z base，`VULKAN_FIX_EMULATE_DISPATCH_BASE=1` 可强制验证)、静态与动态负 viewport 高度下的实际三角形绘制与上下翻转读回（无原生维护 1 时静态 viewport 走 SPIR-V Y 取反回退；动态 viewport 对 `vkCmdDraw`/`vkCmdDrawIndexed` 及 indirect 变体使用翻转管线变体回退，并验证正向 viewport 不会被误翻转；无 `gl_ViewportIndex` 或索引为常量时支持混合正负 viewport；`VULKAN_FIX_EMULATE_NEGATIVE_VIEWPORT=1` 可强制验证）、两 view 的 multiview render pass/pipeline 实际绘制并验证 `gl_ViewIndex` 按层输出不同颜色、StorageBuffer16BitAccess 的实际写入读回、VariablePointers/VariablePointersStorageBuffer 通过指针的 compute 写入读回、SPIR-V 1.4 compute 模块降级到 1.3 后的管线创建，以及 device group 枚举；原生结果异常时，包装层会返回规范要求的值并继续运行；报告末尾的 `core11.summary` 汇总所有 1.1 探针；当前 Mali 原生报告 32 passed / 2 failed（常量 `gl_ViewportIndex` 的负 viewport 被驱动忽略），包装和强制回退均为 34 passed / 0 skipped / 0 failed；`tools/verify_core11_post.py` 会自动区分原生缺陷与包装失败。
- 扩展：动态渲染清屏读回、push descriptor 计算写入、同步 2 屏障与提交、顶点 divisor 属性读取，以及 Android swapchain 创建、获取、提交和呈现。
- 特性：验证 multiDrawIndirect、fillModeNonSolid、drawIndirectFirstInstance、samplerAnisotropy、shaderDrawParameters、timelineSemaphore、hostQueryReset、synchronization2、dynamicRendering、vertexAttributeInstanceRateDivisor 和 vertexPipelineStoresAndAtomics。

查询通过表示声明支持；执行通过只证明对应探针，不能作为完整 Vulkan 一致性认证。在缺少原生非实心填充的设备上，当前兼容层会将线框替换为填充，线框执行探针会如实报告失败。

开发者也可使用独立工具 `vulkanfix_driver_post`，输出为 JSON Lines；`evidence=query` 表示声明和入口查询，`evidence=functional` 表示实际执行探针。命令行没有 Android 测试窗口时会跳过呈现探针。

工具可从 `layer/src/main/cpp/diagnostics` 独立配置 CMake（不需要构建 SPIRV-Tools），也可在主 CMake 构建中启用 `-DVULKAN_FIX_BUILD_DRIVER_POST=ON`，构建目标 `vulkanfix_driver_post`。使用 Android NDK 工具链和设备对应 ABI。
在设备独立进程中运行 `timeout 30 ./vulkanfix_driver_post`；可传入加载器路径以检查 VulkanFix，例如 `timeout 30 ./vulkanfix_driver_post /path/to/libvulkanfix.so`。不要在游戏进程中运行自检；外层超时用于处理驱动函数本身卡住的情况。

Core 命令清单由固定版本的 Khronos 注册表生成：`python tools/generate_driver_post_commands.py --check` 检查是否同步，`python -B tools/test_driver_post.py` 检查清单和分发表。

## 许可证

```
Copyright 2026 由风

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

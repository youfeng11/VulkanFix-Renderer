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
- SPIR-V 着色器转换（含旧驱动的 SSBO 存储类、relaxed block layout 转换及按管线特化值处理数组）、管线缓存及部分绘制功能模拟。

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

sampler、UBO、push descriptor、tiler 和 MSAA 数组 resolve 的驱动测量均在独立进程执行。测试进程 `libvulkanfix_probe.so` 随 APK 安装，崩溃和超时由父进程处理。运行时按各功能所需的测量请求结果，每项按设备、驱动及探针版本单独缓存；完整 POST 会逐项执行。`VULKAN_FIX_DEDUP_SAMPLER_ALIASES=0/1`、`VULKAN_FIX_PRUNE_UNUSED_UNIFORM_MEMBERS=0/1` 仍可用于诊断对照。

原生 push descriptor 需通过计算、片段和顶点阶段的读回测试，包括混合图像/缓冲资源、数组部分更新、共享资源堆，以及跨布局从 set 0 切换到 set 1。支持相应 descriptor indexing 特性时还测试 update-after-bind 堆。每组都与普通 descriptor set 路径作前后对照。未通过确认时使用 descriptor set 模拟，启动器的 force/skip 设置仍优先生效。

tiler 保护通过直接绘制、间接绘制及可用的多重间接绘制压力探针选择，比较单个 render pass 与分批 LOAD 延续的输出。全部通过时保留原始路径；测出异常或无法确定时保留有界批次保护。需要保护的设备会使用可延续的 render pass 模拟动态渲染。`VULKAN_FIX_MALI_TILER_GUARD=1/0` 可强制启用或关闭，名称仅为兼容已有设置，判定不再限定 Mali。压力探针是代表性负载，仍需按实际游戏和光影回归。

打开 VulkanFix 应用，切换“VulkanFix”或“原生驱动”，点击“开始检测”。首页汇总执行结果，检查项目可分组展开，点按项目查看原始诊断。“复制报告”保留两种模式的完整结果。检测过程中显示进度，可随时停止；连续 120 秒未收到新结果时结束探测进程。探测中止后会保存已返回的结果和中止原因。

- Core：检查 Vulkan 1.0、1.1、1.2 的 178 个命令入口，并执行内存、查询、绘制和同步探针。1.1 覆盖必需能力及部分最低限制、查询链、内存绑定、descriptor template、multiview、SSBO 数组特化、dispatch base 和负 viewport 等路径；负 viewport 包含 VS 内部 Position 读写、GS 发射顶点和 TES 输出的静态/动态绘制读回。多 viewport 和可选着色器阶段仅在对应特性受支持且已启用时测试。`VULKAN_FIX_EMULATE_DISPATCH_BASE=1` 和 `VULKAN_FIX_EMULATE_NEGATIVE_VIEWPORT=1` 可强制验证模拟路径。`core11.summary` 仅汇总这些探针；`tools/verify_core11_post.py` 还会拒绝必需能力缺失及其他功能探针失败的报告，不会把所有原生测试失败自动归因于驱动。
- 缺少原生 multiview 时，默认按视图回放绘制，支持 32 个视图；自检逐层读回不同的 `gl_ViewIndex` 颜色。`VULKAN_FIX_SOFTWARE_MULTIVIEW=0` 可关闭此回退用于诊断。
- 缺少原生 relaxed block layout 时，将缓冲区结构中的向量成员拆为保持原字节偏移的标量成员；自检检查偏移 4 的 `vec3`、动态分量写入、原子操作及相邻数据。
- 缺少 maintenance2 时，通过独立的曲面细分评估着色器变体模拟左下 domain origin，保持 `TessCoord` 值；自检比较左上/左下原点下的绕序和背面剔除结果。
- 输入附件 aspect 声明在缺少 maintenance2 的驱动边界省略，保留其他 render pass 状态；实际读取的颜色、深度和模板分量由附件视图及着色器决定。
- 缺少 maintenance1 时，数组与 3D 图像复制通过私有传输缓冲区完成，保留格式块、mip、图层和偏移；临时资源在命令缓冲区重录、重置或释放时回收。
- 3D 图像的二维附件视图使用每次命令录制独立持有的二维数组附件。按渲染区域和所选切片执行加载、写回，保留其他像素；子通道输入、后续三维纹理采样及事件依赖共用相应的视图和布局转换。自检中的“三维切片渲染与采样”和“三维切片子通道读取”会实际绘制并读回验证。
- 扩展：动态渲染清屏读回、push descriptor 计算写入、同步 2 屏障与提交、顶点 divisor 属性读取，以及 Android swapchain 创建、获取、提交和呈现。
- 特性：验证 multiDrawIndirect、fillModeNonSolid、drawIndirectFirstInstance、samplerAnisotropy、shaderDrawParameters、timelineSemaphore、hostQueryReset、synchronization2、dynamicRendering、vertexAttributeInstanceRateDivisor 和 vertexPipelineStoresAndAtomics。

查询通过表示声明支持；执行通过只证明对应探针，不能作为完整 Vulkan 一致性认证。线框与点填充是可选特性，默认保留驱动的能力声明。显式强制 `VULKAN_FIX_EMULATE_FILL_MODE_NON_SOLID=1` 或 `VULKAN_FIX_EMULATE_FILL_MODE_NON_SOLID_FEATURE=1` 可使用旧兼容模式，以实心填充代替线框或点绘制；该模式的线框执行探针会报告失败。

`driverTrust.timelineQueue` 在独立进程验证原生 timeline semaphore 的队列行为。它先提交等待未来值的工作，再从主机发信号，最后检查队列计数和传输读回。设备请求启用 timeline semaphore 时，自动模式可复用这项测量。原生路径读回失败或在开始测量后崩溃，且前后两次二进制信号量对照均通过时，设备会使用 timeline 模拟。自动切换需要异步调度器，启用了外部信号量句柄扩展的设备继续使用原生路径。驱动声明及探针结论分别列入自检报告。

`driverTrust.sync2Events` 检查 GPU 事件的发信号、等待、重置和复用。若原生重置后事件仍为有信号状态，探针会保留原生发信号和等待命令，只替换重置命令重新做前后对照。对照通过后，启用同步 2 的单 GPU 设备会对阶段掩码可等价表达的重置使用 `vkCmdResetEvent`。

`driverTrust.sync2Submit` 检查跨提交的信号量依赖及缓冲区读回，并用旧版 Vulkan 命令作前后对照。若提交探针确认原生故障，启用同步 2 的单 GPU 设备会把可等价表达的批次交给 `vkQueueSubmit`。带有独立信号阶段、扩展结构或设备掩码的批次仍可通过原生 `vkQueueSubmit2` 表达。

`driverTrust.crossFormatImageSlices` 检查原生驱动将 packed UINT 数组图像一次复制到 UNORM 三维图像时，是否只写入首层。探针会读回整个目标图像，核对八层和复制区域外的像素，并用逐层复制作为前后对照。运行时在首次创建匹配的三维图像时复用这项测量。确认首层缺陷后，图形队列上的对应 `vkCmdCopyImage` 会在同一条命令里使用逐层区域。

`driverTrust.rasterizerDiscardState` 通过像素读回检查 rasterizer discard 绘制之后继承的 viewport 和 scissor，覆盖同一渲染通道和跨通道两种情况。报告用重新设置动态状态及普通光栅化绘制作为对照。确认缺陷后，运行时会在离开静态 discard 管线、双方均声明标准 viewport/scissor 动态状态的管线切换处，重新设置仍有效的数组范围。静态状态覆盖、带计数的状态命令、命令缓冲重置和次级命令执行会使相应缓存失效。

`driverTrust.storageArrayStatic` 和 `driverTrust.storageArrayDynamic` 分别检查静态索引和动态索引的原生 SSBO 整数组写回，并用逐字段写回作前后对照。VulkanFix 的 Core 自检会另外执行同一批原始着色器，验证现有改写能否恢复正确的读回。

`driverTrust.depthStencilResolve` 检查 4x 深度／模板附件在无绘制的渲染通道结束时是否执行 sample-zero resolve 写回。探针对各样本填入同一已知值，逐像素核对可用格式的深度和模板值，并以前后两次着色器采样写回作为对照。结果用于诊断。

`driverTrust.rgb9e5TransferBuffer` 和 `driverTrust.rgb9e5TransferImage` 分别检查 RGB9_E5 到缓冲区、到同格式图像的复制是否保留原始位模式。探针覆盖 30×30 和 16×16，用 `R32_UINT` 视图读取原始字作为前后对照。报告会区分等值重新编码和其他数据损坏。

`driverTrust.queue2` 检查原生 `vkGetDeviceQueue2(flags=0)` 的队列句柄和传输读回，前后对照使用 `vkGetDeviceQueue`。VulkanFix 的公共入口和 timeline 调度器共用队列获取规则。普通队列走旧入口，带创建标志的队列使用原生 `vkGetDeviceQueue2`。

`driverTrust.timestampQueries` 在支持时间戳的图形或计算队列上执行两轮 GPU 填充和查询池复用，检查查询可用状态、有效位、计数器回绕以及 GPU 拷贝结果和主机读回的一致性。该项用于诊断，报告包含所选队列、时间戳精度和观测到的计数差。

`driverTrust.hostQueryReset` 在独立进程中用命令缓冲重置作前后对照，验证原生主机重置能否清除指定的两个查询并重新写入，同时保留相邻查询的可用状态和结果。`hostQueryReset.reset` 在所选渲染器上执行同一组读回。原生测量用于诊断。

渲染器与 POST 共用驱动测量控制器。控制器负责身份校验、缓存和对照判定，原生 GPU 探针只编入独立探针进程。每项测量按驱动身份缓存，缓存满时优先淘汰最久未使用的已完成结果。报告区分已观察到的缺陷和已验证的修复，兼容模块依据对应测量和可用的替代路径选择是否回退。

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

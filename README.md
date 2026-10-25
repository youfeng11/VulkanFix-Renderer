# Vulkan Fix

Android 端 Minecraft 的 Vulkan 兼容插件，适用于 FCL、ZalithLauncher 2 等支持渲染器插件的启动器，面向 Iris Vulkan 等 Vulkan 渲染模组。

在原生 Vulkan 驱动之上补充部分缺失的接口和特性。设备需要具备 Vulkan 驱动，插件最低支持 Android 8.0。

## 使用

1. 安装插件 APK。
2. 在启动器的渲染器设置中选择 **Vulkan Fix**。
3. 将游戏或渲染模组的图形 API 设为 **Vulkan**，再启动游戏。

## 功能与限制

- Vulkan 1.1 / 1.2 部分核心接口、特性查询和扩展兼容。
- 动态渲染、同步 2、push descriptor 和 timeline semaphore 的兼容实现。
- SPIR-V 着色器转换、管线缓存及部分绘制功能模拟。

顶点存储模拟目前不支持间接绘制、传统 render pass、几何和曲面细分阶段。

完整 Vulkan 1.1 / 1.2 核心兼容仍在开发中。

反馈问题时，请附设备型号、驱动信息、游戏与模组版本、光影包版本，以及相关日志。

## 编译

需要 JDK 17、Android SDK 34、NDK `25.2.9519653` 和 CMake 3.20 或更新版本。

```sh
git clone --recurse-submodules https://github.com/youfeng11/VulkanFix-Renderer.git
cd VulkanFix-Renderer
./gradlew :pluginApp:assembleDebug
```

Windows 使用 `gradlew.bat :pluginApp:assembleDebug`。已有仓库可运行 `git submodule update --init --recursive` 补齐子模块。

APK 输出到 `pluginApp/build/outputs/apk/debug/`，包含 `arm64-v8a`、`armeabi-v7a`、`x86` 和 `x86_64` 四种架构。

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
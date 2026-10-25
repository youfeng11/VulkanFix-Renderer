import com.launchers_plugin.renderer.buildscript.RendererConfig
import com.launchers_plugin.renderer.buildscript.buildEnvs
import com.launchers_plugin.renderer.buildscript.buildJsonValue
import com.launchers_plugin.renderer.buildscript.legacyManifest
import com.launchers_plugin.renderer.buildscript.nativePath
import com.launchers_plugin.renderer.buildscript.renderer
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.security.MessageDigest

plugins {
    id("com.launchers_plugin.renderer.dsl")
    alias(libs.plugins.android.application)
    id("com.google.devtools.ksp")
    id("kotlinx-serialization")
}

// Explicit diagnostic build only; ordinary offline APK builds do not download
// or package the validation provider.
val vulkanValidation = providers.gradleProperty("vulkanValidation").orNull
require(vulkanValidation == null || vulkanValidation in setOf("core", "sync")) {
    "vulkanValidation must be core or sync"
}
val diagnosticName = if (vulkanValidation == null) "Vulkan Fix" else
    "Vulkan Fix ($vulkanValidation validation)"

android {
    namespace = "com.launchers_plugin.renderer"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.youfeng.plugin.vulkanfix"
        minSdk = 26
        targetSdk = 34
        versionCode = rootProject.extra["pluginVersionCode"] as Int
        versionName = (rootProject.extra["pluginVersionName"] as String) +
            (vulkanValidation?.let { "-validation-$it" } ?: "")
    }

    buildFeatures {
        resValues = true
    }
    if (vulkanValidation != null) {
        val staged = layout.buildDirectory.dir("validation-jniLibs").get().asFile
        for (abi in listOf("arm64-v8a", "armeabi-v7a", "x86", "x86_64")) {
            for (library in listOf("libVkLayer_khronos_validation.so", "libvulkan_fix_validation_driver.so")) {
                require(staged.resolve("$abi/$library").isFile) {
                    "Missing validation payload $abi/$library. Run tools/prepare_validation_diagnostics.py first."
                }
            }
        }
        sourceSets.getByName("main").jniLibs.srcDir(staged)
        sourceSets.getByName("main").assets.srcDir(layout.buildDirectory.dir("validation-assets").get().asFile)
        val metadata = layout.buildDirectory.file("validation-assets/VulkanFix-validation-build.json").get().asFile
        val recordedSource = Regex("\"bridge_source_sha256\"\\s*:\\s*\"([0-9a-f]{64})\"")
            .find(metadata.readText())?.groupValues?.get(1)
        val currentSource = MessageDigest.getInstance("SHA-256")
            .digest(rootProject.file("layer/src/main/cpp/diagnostics/validation_driver.cpp").readBytes())
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        require(recordedSource == currentSource) { "Validation payload is stale; stage it again" }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    signingConfigs {
        getByName("debug") {
            storeFile = file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            signingConfig = signingConfigs.getByName("debug")
        }
        debug {
            signingConfig = signingConfigs.getByName("debug")
        }
        configureEach {
            // 新架构渲染器配置 (Vulkan Fix)
            resValue("string", "config", buildJsonValue {
                renderer(
                    displayName = diagnosticName,
                    rendererId = "opengles3",
                    rendererGLPath = nativePath("libvulkan_fix.so"),
                    rendererEGLPath = nativePath("libvulkan_fix.so"),
                    dlopenLibPaths = emptyList(),
                    env = buildEnvs {
                        normal("POJAV_VULKAN_WRAPPER", "1")
                        normal("LIBGL_ES", "3")
                        if (vulkanValidation != null) {
                            normal("VULKAN_FIX_VALIDATION_DRIVER", "1")
                            normal("VK_VALIDATION_VALIDATE_SYNC", if (vulkanValidation == "sync") "1" else "0")
                        }
                        toggleable(
                            key = "VULKAN_FIX_DEBUG",
                            value = "1",
                            toggle = false,
                            title = RendererConfig.MetaString("title_enable_debug")
                        )
                        selectable(
                            key = "VULKAN_FIX_EMULATE_DYNAMIC_RENDERING",
                            items = RendererConfig.EnvItems(
                                defaultValue = "auto",
                                values = listOf("force", "skip")
                            ),
                            title = RendererConfig.MetaString("title_emulate_dynamic_rendering_ext")
                        )
                        selectable(
                            key = "VULKAN_FIX_EMULATE_DYNAMIC_RENDERING_FEATURE",
                            items = RendererConfig.EnvItems(
                                defaultValue = "auto",
                                values = listOf("force", "skip")
                            ),
                            title = RendererConfig.MetaString("title_emulate_dynamic_rendering_feat")
                        )
                        selectable(
                            key = "VULKAN_FIX_EMULATE_PUSH_DESCRIPTOR",
                            items = RendererConfig.EnvItems(
                                defaultValue = "auto",
                                values = listOf("force", "skip")
                            ),
                            title = RendererConfig.MetaString("title_emulate_push_descriptor_ext")
                        )
                        selectable(
                            key = "VULKAN_FIX_EMULATE_FILL_MODE_NON_SOLID_FEATURE",
                            items = RendererConfig.EnvItems(
                                defaultValue = "auto",
                                values = listOf("force", "skip")
                            ),
                            title = RendererConfig.MetaString("title_emulate_fill_mode_non_solid_feat")
                        )
                        selectable(
                            key = "VULKAN_FIX_EMULATE_DRAW_INDIRECT_FIRST_INSTANCE_FEATURE",
                            items = RendererConfig.EnvItems(
                                defaultValue = "auto",
                                values = listOf("force", "skip")
                            ),
                            title = RendererConfig.MetaString("title_emulate_draw_indirect_first_instance_feat")
                        )
                        selectable(
                            key = "VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE",
                            items = RendererConfig.EnvItems(
                                defaultValue = "auto",
                                values = listOf("force", "skip")
                            ),
                            title = RendererConfig.MetaString("title_emulate_vertex_pipeline_stores_and_atomics_feat")
                        )
                        selectable(
                            key = "VULKAN_FIX_EMULATE_DIVISOR",
                            items = RendererConfig.EnvItems(
                                defaultValue = "auto",
                                values = listOf("force", "skip")
                            ),
                            title = RendererConfig.MetaString("title_emulate_divisor")
                        )
                        toggleable(
                            key = "VULKAN_FIX_OPTIMIZE_BARRIER",
                            value = "0",
                            toggle = false,
                            title = RendererConfig.MetaString("title_disable_optimize_barrier")
                        )
                        toggleable(
                            key = "VULKAN_FIX_DISABLE_PIPELINE_CACHE",
                            value = "1",
                            toggle = false,
                            title = RendererConfig.MetaString("title_disable_pipeline_cache")
                        )
                    },
                    minMCVer = null,
                    maxMCVer = null,
                )
            })

            // 兼容旧版渲染器插件架构
            manifestPlaceholders.putAll(legacyManifest {
                displayName     = diagnosticName
                rendererName    = diagnosticName
                rendererLib     = "libvulkan_fix.so"
                eglLib          = "libvulkan_fix.so"
                minMCVer        = ""
                maxMCVer        = ""

                boatEnv {
                    put("POJAV_RENDERER", "opengles3")
                    put("POJAV_VULKAN_WRAPPER", "1")
                    put("LIBGL_ES", "3")
                    if (vulkanValidation != null) {
                        put("VULKAN_FIX_VALIDATION_DRIVER", "1")
                        put("VK_VALIDATION_VALIDATE_SYNC", if (vulkanValidation == "sync") "1" else "0")
                    }
                }
                pojavEnv {
                    put("POJAV_RENDERER", "opengles3")
                    put("POJAV_VULKAN_WRAPPER", "1")
                    put("LIBGL_ES", "3")
                    if (vulkanValidation != null) {
                        put("VULKAN_FIX_VALIDATION_DRIVER", "1")
                        put("VK_VALIDATION_VALIDATE_SYNC", if (vulkanValidation == "sync") "1" else "0")
                    }
                }
            })
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_1_8)
    }
}

dependencies {
    implementation(libs.androidx.annotation)
    implementation(project(":layer"))
}

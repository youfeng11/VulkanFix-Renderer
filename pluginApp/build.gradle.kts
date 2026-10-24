import com.launchers_plugin.renderer.buildscript.RendererConfig
import com.launchers_plugin.renderer.buildscript.buildEnvs
import com.launchers_plugin.renderer.buildscript.buildJsonValue
import com.launchers_plugin.renderer.buildscript.legacyManifest
import com.launchers_plugin.renderer.buildscript.nativePath
import com.launchers_plugin.renderer.buildscript.renderer
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.launchers_plugin.renderer.dsl")
    alias(libs.plugins.android.application)
    id("com.google.devtools.ksp")
    id("kotlinx-serialization")
}

android {
    namespace = "com.launchers_plugin.renderer"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.youfeng.plugin.vulkanfix"
        minSdk = 26
        targetSdk = 34
        versionCode = rootProject.extra["pluginVersionCode"] as Int
        versionName = rootProject.extra["pluginVersionName"] as String
    }

    buildFeatures {
        resValues = true
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
                    displayName = "Vulkan Fix",
                    rendererId = "opengles3",
                    rendererGLPath = nativePath("libvulkan_fix.so"),
                    rendererEGLPath = nativePath("libvulkan_fix.so"),
                    dlopenLibPaths = emptyList(),
                    env = buildEnvs {
                        normal("POJAV_VULKAN_WRAPPER", "1")
                        normal("LIBGL_ES", "3")
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
                            value = "1",
                            toggle = false,
                            title = RendererConfig.MetaString("title_optimize_barrier")
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
                displayName     = "Vulkan Fix"
                rendererName    = "Vulkan Fix"
                rendererLib     = "libvulkan_fix.so"
                eglLib          = "libvulkan_fix.so"
                minMCVer        = ""
                maxMCVer        = ""

                boatEnv {
                    put("POJAV_RENDERER", "opengles3")
                    put("POJAV_VULKAN_WRAPPER", "1")
                    put("LIBGL_ES", "3")
                }
                pojavEnv {
                    put("POJAV_RENDERER", "opengles3")
                    put("POJAV_VULKAN_WRAPPER", "1")
                    put("LIBGL_ES", "3")
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

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

fun getGitCommitCount(): Int {
    return try {
        val process = ProcessBuilder("git", "rev-list", "--count", "HEAD")
            .directory(rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start()
        val count = process.inputStream.bufferedReader().use { it.readText().trim() }
        process.waitFor()
        count.toIntOrNull() ?: 1
    } catch (_: Exception) {
        1
    }
}

android {
    namespace = "com.launchers_plugin.renderer"
    compileSdk = 34
    ndkVersion = "30.0.14904198"

    defaultConfig {
        applicationId = "com.youfeng.plugin.vulkanfix"
        minSdk = 26
        targetSdk = 34
        versionCode = getGitCommitCount()
        versionName = "0.2.0"

        ndk {
            abiFilters.addAll(listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86"))
        }

        externalNativeBuild {
            cmake {
                cppFlags("-std=c++17")
                arguments(
                    "-DANDROID_STL=c++_static",
                    "-DPROJECT_VERSION_NAME=${versionName}",
                    "-DPROJECT_VERSION_CODE=${versionCode}"
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    buildFeatures {
        resValues = true
    }

    packagingOptions {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
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
                            key = "VK_FIX_DEBUG",
                            value = "1",
                            toggle = false,
                            title = RendererConfig.MetaString("title_enable_debug")
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
}

plugins {
    alias(libs.plugins.android.library)
}

/**
 * Native renderer / Vulkan layer implementation.
 *
 * All C/C++ sources live here; :pluginApp only owns the Android shell (manifest,
 * launcher metadata, resources) and packages the shared library produced by this
 * module.
 */
android {
    namespace = "com.launchers_plugin.renderer.layer"
    compileSdk = 34
    ndkVersion = providers.gradleProperty("vulkanFixNdkVersion").get()

    defaultConfig {
        minSdk = 26

        ndk {
            abiFilters.addAll(listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86"))
        }

        externalNativeBuild {
            cmake {
                cppFlags("-std=c++17")
                arguments(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DPROJECT_VERSION_NAME=${rootProject.extra["pluginVersionName"]}",
                    "-DPROJECT_VERSION_CODE=${rootProject.extra["pluginVersionCode"]}"
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }
}

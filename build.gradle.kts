// Top-level build file where you can add configuration options common to all sub-projects/modules.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.jetbrains.kotlin.android) apply false
    alias(libs.plugins.ksp.plugin) apply false
    kotlin("plugin.serialization") version libs.versions.kotlin apply false
}

// Single source of truth for the plugin version. Consumed by :pluginApp (APK
// versionCode/versionName) and :layer (PROJECT_VERSION_* passed to CMake).
fun getGitCommitCount(): Int {
    return try {
        val shallowFile = File(rootDir, ".git/shallow")
        if (shallowFile.exists()) {
            try {
                ProcessBuilder("git", "fetch", "--unshallow")
                    .directory(rootDir)
                    .redirectOutput(ProcessBuilder.Redirect.PIPE)
                    .redirectError(ProcessBuilder.Redirect.PIPE)
                    .start()
                    .waitFor()
            } catch (_: Exception) {}
        }
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

val pluginVersionName = "0.4.1"
val pluginVersionCode = getGitCommitCount()
extra["pluginVersionName"] = pluginVersionName
extra["pluginVersionCode"] = pluginVersionCode

val dslBuild = gradle.includedBuild("dsl")
tasks.register("publishToMavenLocal") {
    dependsOn(dslBuild.task(":publishToMavenLocal"))
}

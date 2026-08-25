pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "Lighthouse"
include(":app")

// SDL2's Java shim has to match the native SDL2 that CMake links, so both come from one
// checkout: this clone, which app/build.gradle.kts also passes to CMake as
// FETCHCONTENT_SOURCE_DIR_SDL2. Keep sdl2Tag in gradle.properties equal to the GIT_TAG in
// libultraship/cmake/dependencies/android.cmake.
val sdl2Tag: String = extra.properties["sdl2Tag"] as? String
    ?: file("gradle.properties").readLines()
        .firstOrNull { it.startsWith("sdl2Tag=") }?.substringAfter("=")?.trim()
    ?: error("sdl2Tag is not set in android/gradle.properties")

val sdl2Dir = rootDir.parentFile.resolve("build-android/sdl2-src")
// Inside the checkout, so the CI cache of build-android/sdl2-src carries it.
val sdl2Stamp = sdl2Dir.resolve(".lighthouse-sdl2-tag")
if (!sdl2Dir.resolve("android-project").isDirectory ||
    !sdl2Stamp.isFile ||
    sdl2Stamp.readText().trim() != sdl2Tag
) {
    sdl2Dir.parentFile.mkdirs()
    sdl2Dir.deleteRecursively()
    logger.lifecycle("Cloning SDL2 $sdl2Tag into ${sdl2Dir.path}")
    val result = providers.exec {
        commandLine(
            "git", "clone", "--depth", "1", "--branch", sdl2Tag,
            "https://github.com/libsdl-org/SDL.git", sdl2Dir.absolutePath
        )
        isIgnoreExitValue = true
    }
    if (result.result.get().exitValue != 0) {
        error("Failed to clone SDL2 $sdl2Tag:\n${result.standardError.asText.get()}")
    }
    sdl2Stamp.writeText(sdl2Tag)
}

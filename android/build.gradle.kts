plugins {
    id("com.android.application") version "8.7.3" apply false
}

// Keeps Gradle's own scratch out of android/, next to the modules' output in build-android/.
layout.buildDirectory.set(rootDir.parentFile.resolve("build-android/gradle"))

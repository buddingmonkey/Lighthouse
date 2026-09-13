import com.android.build.api.artifact.SingleArtifact
import java.net.URI

plugins {
    id("com.android.application")
}

val repoRoot: File = rootProject.projectDir.parentFile
val outputRoot: File = repoRoot.resolve("build-android")

layout.buildDirectory.set(outputRoot.resolve("app"))

val lighthouseVersion: String = Regex("""project\(Lighthouse\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)""")
    .find(repoRoot.resolve("CMakeLists.txt").readText())?.groupValues?.get(1)
    ?: error("Could not read the project version from ${repoRoot.resolve("CMakeLists.txt").path}")

val sdl2Src: File = outputRoot.resolve("sdl2-src")
val stagedAssets: File = layout.buildDirectory.dir("lighthouse-assets").get().asFile

android {
    namespace = "com.harbormasters.lighthouse"
    compileSdk = 35
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = providers.gradleProperty("applicationId").get()
        minSdk = 29
        targetSdk = 35
        versionCode = providers.gradleProperty("versionCode").getOrElse("1").toInt()
        versionName = lighthouseVersion

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DUSE_OPENGLES=ON",
                    "-DENABLE_OPENXR=ON",
                    "-DANDROID_STL=c++_shared",
                    "-DFETCHCONTENT_SOURCE_DIR_SDL2=${sdl2Src.absolutePath}",
                    "-DENABLE_DEBUG_TOOLS=${providers.gradleProperty("debugTools").getOrElse("OFF")}"
                )
                targets += "Lighthouse"
            }
        }

        ndk {
            abiFilters += providers.gradleProperty("abis").getOrElse("arm64-v8a").split(",")
        }
    }

    externalNativeBuild {
        cmake {
            path = repoRoot.resolve("CMakeLists.txt")
            version = "3.31.6"
            buildStagingDirectory = outputRoot.resolve(".cxx")
        }
    }

    sourceSets.getByName("main") {
        java.srcDir(sdl2Src.resolve("android-project/app/src/main/java"))
        assets.srcDir(stagedAssets)
    }

    val keystoreFile = providers.gradleProperty("keystoreFile").orNull
    if (keystoreFile != null) {
        signingConfigs.create("sideload") {
            storeFile = file(keystoreFile)
            storePassword = providers.gradleProperty("keystorePassword").get()
            keyAlias = providers.gradleProperty("keyAlias").get()
            keyPassword = providers.gradleProperty("keyPassword").get()
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            isJniDebuggable = false
            signingConfig = if (keystoreFile != null) {
                signingConfigs.getByName("sideload")
            } else {
                signingConfigs.getByName("debug")
            }
        }
        debug {
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs.useLegacyPackaging = false
    }
}

val fetchGameControllerDb by tasks.registering {
    val out = stagedAssets.resolve("gamecontrollerdb.txt")
    outputs.file(out)
    doLast {
        if (out.exists() && out.length() > 0L) return@doLast
        out.parentFile.mkdirs()
        try {
            val url = "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt"
            out.writeBytes(URI(url).toURL().readBytes())
        } catch (e: Exception) {
            logger.warn("Could not download gamecontrollerdb.txt (${e.message}); shipping without it")
            out.writeText("")
        }
    }
}

val stageLighthouseAssets by tasks.registering(Copy::class) {
    into(stagedAssets)
    from(repoRoot.resolve("config.yml"))
    from(repoRoot.resolve("assets/yaml")) { into("assets/yaml") }
    from(repoRoot.resolve("lighthouse.o2r"))
    doFirst {
        val o2r = repoRoot.resolve("lighthouse.o2r")
        if (!o2r.exists() || o2r.length() == 0L) {
            throw GradleException(
                "lighthouse.o2r is missing or empty at ${o2r.path}. Build it from a host tree first:\n" +
                    "  cmake -H. -Bbuild-cmake -GNinja && cmake --build build-cmake --target GeneratePortO2R"
            )
        }
    }
}

tasks.named("preBuild") {
    dependsOn(stageLighthouseAssets, fetchGameControllerDb)
}

androidComponents {
    onVariants { variant ->
        val suffix = variant.name.replaceFirstChar { it.uppercase() }
        val apkDir = variant.artifacts.get(SingleArtifact.APK)
        val target = outputRoot.resolve("lighthouse-${variant.name}.apk")
        val publish = tasks.register("publish${suffix}Apk") {
            outputs.file(target)
            outputs.upToDateWhen { false }
            doLast {
                val dir = apkDir.get().asFile
                val apk = dir.listFiles { f: File -> f.extension == "apk" }?.firstOrNull()
                    ?: throw GradleException("No APK was produced in $dir")
                apk.copyTo(target, overwrite = true)
            }
        }
        tasks.matching { it.name == "assemble$suffix" }.configureEach { finalizedBy(publish) }
    }
}

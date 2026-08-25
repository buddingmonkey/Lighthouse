# Building Lighthouse

## Windows

Requires:
  * At least 8GB of RAM (machines with 4GB have seen complier failures)
  * Visual Studio 2022 Community Edition with the C++ feature set
  * One of the Windows SDKs that comes with Visual Studio, for example the current Windows 10 version 10.0.19041.0
  * The `MSVC v143 - VS 2022 C++ build tools` component of Visual Studio
  * Python 3 (can be installed manually or as part of Visual Studio)
  * Git (can be installed manually or as part of Visual Studio)
  * Cmake version <= 3.3 (can be installed via chocolatey or manually)

During installation, check the "Desktop development with C++" feature set:

![image](https://user-images.githubusercontent.com/30329717/183511274-d11aceea-7900-46ec-acb6-3f2cc110021a.png)
Doing so should also check one of the Windows SDKs by default.  Then, in the installation details in the right-hand column, make sure you also check the v143 toolset. This is often done by default.

It is recommended that you install Python and Git standalone, the install process in VS Installer has given some issues in the past.

1. Clone the Lighthouse repository

_Note: Be sure to either clone with the ``--recursive`` flag or do ``git submodule update --init`` after cloning to pull in the libultraship submodule!_

2. Build and generate the `lighthouse.o2r` port assets archive.
3. Generate the `bk.o2r` ROM asset archive.

_Note: Instructions assume using powershell, however, it's recommended to use the GeneratePortO2R project in VS and the built-in extraction flow to create the asset archives
```powershell
# Navigate to the lighthouse repo within powershell. ie: cd "C:\yourpath\lighthouse"
cd lighthouse

# Setup cmake project
# Add `-DCMAKE_BUILD_TYPE:STRING=Release` if you're packaging
& 'C:\Program Files\CMake\bin\cmake' -S . -B "build/x64" -G "Visual Studio 17 2022" -T v143 -A x64

# Generate bk.o2r (extracts assets from ROM via Torch)
& 'C:\Program Files\CMake\bin\cmake.exe' --build .\build\x64 --target ExtractAssets

# Generate lighthouse.o2r (port-specific assets)
& 'C:\Program Files\CMake\bin\cmake.exe' --build .\build\x64 --target GeneratePortO2R

# Compile project
# Add `--config Release` if you're packaging
& 'C:\Program Files\CMake\bin\cmake.exe' --build .\build\x64

# Now you can run the executable in .\build\x64 or run in Visual Studio
```

### Developing Lighthouse
With the cmake build system you have two options for working on the project:

#### Visual Studio
To develop using Visual Studio you only need to use cmake to generate the solution file:
```powershell
# Generates lighthouse.sln at `build/x64` for Visual Studio 2022
& 'C:\Program Files\CMake\bin\cmake' -S . -B "build/x64" -G "Visual Studio 17 2022" -T v143 -A x64
```

#### Visual Studio Code or another editor
To develop using Visual Studio Code or another editor you only need to open the repository in it.
To build you'll need to follow the instructions from the building section.

_Note: If you're using Visual Studio Code, the [cpack plugin](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) makes it very easy to just press run and debug._

_Experimental: You can also use another build system entirely rather than MSVC like [Ninja](https://ninja-build.org/) for possibly better performance._


### Generating the distributable
After compiling the project you can generate the distributable by running:
```powershell
# Go to build folder
cd "build/x64"
# Generate
& 'C:\Program Files\CMake\bin\cpack.exe' -G ZIP
```

### Additional CMake Targets
#### Clean
```powershell
# If you need to clean the project you can run
C:\Program Files\CMake\bin\cmake.exe --build build-cmake --target clean
```

## Linux
### Install dependencies
#### Debian/Ubuntu
```sh
# using gcc
apt-get install gcc g++ git cmake ninja-build lsb-release libsdl2-dev libpng-dev libsdl2-net-dev libzip-dev zipcmp zipmerge ziptool nlohmann-json3-dev libtinyxml2-dev libspdlog-dev libboost-dev libopengl-dev libogg-dev libvorbis-dev

# or using clang
apt-get install clang git cmake ninja-build lsb-release libsdl2-dev libpng-dev libsdl2-net-dev libzip-dev zipcmp zipmerge ziptool nlohmann-json3-dev libtinyxml2-dev libspdlog-dev libboost-dev libopengl-dev libogg-dev libvorbis-dev
```
#### Arch
```sh
# using gcc
pacman -S gcc git cmake ninja lsb-release sdl2 libpng libzip nlohmann-json tinyxml2 spdlog sdl2_net boost libogg libvorbis

# or using clang
pacman -S clang git cmake ninja lsb-release sdl2 libpng libzip nlohmann-json tinyxml2 spdlog sdl2_net boost libogg libvorbis
```
#### Fedora
```sh
# using gcc
dnf install gcc gcc-c++ git cmake ninja-build lsb_release SDL2-devel libpng-devel libzip-devel libzip-tools nlohmann-json-devel tinyxml2-devel spdlog-devel boost-devel libogg-devel libvorbis-devel

# or using clang
dnf install clang git cmake ninja-build lsb_release SDL2-devel libpng-devel libzip-devel libzip-tools nlohmann-json-devel tinyxml2-devel spdlog-devel boost-devel libogg-devel libvorbis-devel
```
#### openSUSE
```sh
# using gcc
zypper in gcc gcc-c++ git cmake ninja SDL2-devel libpng16-devel libzip-devel libzip-tools nlohmann_json-devel tinyxml2-devel spdlog-devel libogg-devel libvorbis-devel

# or using clang
zypper in clang libstdc++-devel git cmake ninja SDL2-devel libpng16-devel libzip-devel libzip-tools nlohmann_json-devel tinyxml2-devel spdlog-devel libogg-devel libvorbis-devel
```

### Build

_Note: If you're using Visual Studio Code, the [CMake Tools plugin](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) makes it very easy to just press run and debug._

```bash
# Clone the repo and enter the directory
git clone https://github.com/HarbourMasters/lighthouse.git
cd lighthouse

# Clone the submodules
git submodule update --init

# Generate Ninja project
# Add `-DCMAKE_BUILD_TYPE:STRING=Release` if you're packaging
# Add `-DPython3_EXECUTABLE=$(which python3)` if you are using non-standard Python installations such as PyEnv
cmake -H. -Bbuild-cmake -GNinja

# Generate bk.o2r (extracts assets from ROM via Torch)
cmake --build build-cmake --target ExtractAssets

# Generate lighthouse.o2r (port-specific assets)
cmake --build build-cmake --target GeneratePortO2R

# Compile the project
# Add `--config Release` if you're packaging
cmake --build build-cmake

# Now you can run the executable in ./build-cmake/Lighthouse
# To develop the project open the repository in VSCode (or your preferred editor)
```

### Generate a distributable
After compiling the project you can generate a distributable by running of the following:
```bash
# Go to build folder
cd build-cmake
# Generate
cpack -G DEB
cpack -G ZIP
cpack -G External (creates appimage)
```

### Additional CMake Targets
#### Clean
```bash
# If you need to clean the project you can run
cmake --build build-cmake --target clean
```

## macOS
Requires Xcode (or xcode-tools) && `sdl2, libpng, glew, ninja, cmake, nlohmann-json, tinyxml2, libzip, vorbis-tools` (can be installed via homebrew, macports, etc)

**Important: For maximum performance make sure you have ninja build tools installed!**

_Note: If you're using Visual Studio Code, the [cpack plugin](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) makes it very easy to just press run and debug._

```bash
# Clone the repo
git clone https://github.com/HarbourMasters/lighthouse.git
cd lighthouse
# Clone the submodule libultraship
git submodule update --init

# Generate Ninja project
# Add `-DCMAKE_BUILD_TYPE:STRING=Release` if you're packaging
cmake -H. -Bbuild-cmake -GNinja

# Generate bk.o2r (extracts assets from ROM via Torch)
cmake --build build-cmake --target ExtractAssets

# Generate lighthouse.o2r (port-specific assets)
cmake --build build-cmake --target GeneratePortO2R

# Compile the project
# Add `--config Release` if you're packaging
cmake --build build-cmake

# Now you can run the executable file:
./build-cmake/Lighthouse
# To develop the project open the repository in VSCode (or your preferred editor)
```

### Generating a distributable
After compiling the project you can generate a distributable by running of the following:
```bash
# Go to build folder
cd build-cmake
# Generate
cpack
```

### Additional CMake Targets
#### Clean
```bash
# If you need to clean the project you can run
cmake --build build-cmake --target clean
```

## iOS (iPhone / iPad)
Requires a Mac with Xcode 15 or newer && `cmake, ninja` (can be installed via homebrew, macports, etc). SDL2, metal-cpp and libzip are fetched while configuring. The renderer is Metal.

_Note: the build cross-compiles, so `lighthouse.o2r` has to come from a macOS build tree and `bk.o2r` is extracted on the device._

_Note: `IOS_DEVELOPMENT_TEAM` is your 10-character Apple Developer Team ID, from <https://developer.apple.com/account>, and `PROJECT_ID` a bundle identifier your team owns. A free Apple ID works, added under Xcode > Settings > Accounts, but its profiles expire after 7 days. Leave the team unset to compile without an Apple account._

```bash
# Clone the repo
git clone https://github.com/HarbourMasters/lighthouse.git
cd lighthouse
# Clone the submodule libultraship
git submodule update --init

# Generate lighthouse.o2r (port-specific assets) on the host
cmake -H. -Bbuild-cmake -GNinja
cmake --build build-cmake --target GeneratePortO2R

# Generate the Xcode project, which the app bundle, asset catalog and signing all need
# CMAKE_IGNORE_PREFIX_PATH keeps the homebrew/macports SDL2 and libzip out, wrong arch
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
  -DPLATFORM=OS64 \
  -DDEPLOYMENT_TARGET=16.0 \
  -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;/opt/local" \
  -DPROJECT_ID=com.yourname.lighthouse \
  -DIOS_DEVELOPMENT_TEAM=YOURTEAMID

# Compile the project, or open build-ios/Lighthouse.xcodeproj and hit Run
# Build the Lighthouse target rather than ALL_BUILD to skip the SDL2 dylib and the host Torch
# Drop -allowProvisioningUpdates if you set no team
cmake --build build-ios --config Release --target Lighthouse -- -allowProvisioningUpdates
```

_Note: if the build reports that no profiles were found, open `build-ios/Lighthouse.xcodeproj` once and pick your team under the Lighthouse target's *Signing & Capabilities*._

### Getting the game onto a device
1. Install and launch the app once. It creates a `Lighthouse` folder under *On My iPhone* / *On My iPad* in the Files app.
2. Copy a supported Banjo-Kazooie ROM (`.z64`) into that folder.
3. Relaunch and let the app extract `bk.o2r`. This takes a few minutes and needs roughly 200 MB free.

Saves, `lighthouse.cfg.json` and the `mods` folder live in the same folder.

### Generating a distributable
Installing from Xcode needs Developer Mode turned on. A build signed for distribution (TestFlight or Ad Hoc, paid membership only) installs without it, but can't be debugged.

```bash
# Archive
xcodebuild -project build-ios/Lighthouse.xcodeproj -scheme Lighthouse \
  -configuration Release -destination 'generic/platform=iOS' \
  -archivePath build-ios/Lighthouse.xcarchive archive -allowProvisioningUpdates

# Should list Lighthouse.app, an empty Applications folder means the archive can't be exported
ls build-ios/Lighthouse.xcarchive/Products/Applications

# Export, with `teamID` and `method` (app-store-connect or release-testing) set in the plist
xcodebuild -exportArchive -archivePath build-ios/Lighthouse.xcarchive \
  -exportOptionsPlist <plist> -exportPath build-ios/export -allowProvisioningUpdates
```

### iOS notes
* Landscape only, full screen, at the display's native pixel resolution.
* Any MFi / Bluetooth controller SDL2 recognises works. Pair it in iOS Settings first; the on-screen pad hides itself while one is connected.
* The on-screen controls are configured in *Settings > Controls*, under **On-Screen Controls**: size, reach, opacity, edge margin, a left-handed layout, the phone/tablet layout and an optional D-pad. The **MENU** button at the top of the screen opens the port menu, which is otherwise bound to Escape.
* Mods work as on desktop — drop `.o2r`/`.otr` files into `Lighthouse/mods` via the Files app. Applying a mod list needs the app to be closed and reopened, since iOS apps can't relaunch themselves.
* Networking (Anchor multiplayer) is off, SDL2_net isn't part of the iOS dependency set.

## Android
Requires the Android SDK (platform 35, build-tools), NDK r29 and a JDK 17. Gradle drives CMake, and the renderer is OpenGL ES 3.0. Set `ANDROID_HOME` and `ANDROID_NDK_HOME`, or write them into `android/local.properties`.

_Note: the build cross-compiles, so `lighthouse.o2r` has to come from a host build tree and `bk.o2r` is extracted on the device._

_Note: Gradle clones SDL2 into `build-android/sdl2-src` on the first configure. The Java shim and the native library have to be the same version, so that one checkout supplies both. Keep `sdl2Tag` in `android/gradle.properties` equal to the SDL2 `GIT_TAG` in `libultraship/cmake/dependencies/android.cmake`._

```bash
# Clone the repo
git clone https://github.com/HarbourMasters/lighthouse.git
cd lighthouse
# Clone the submodule libultraship
git submodule update --init

# Generate lighthouse.o2r (port-specific assets) on the host
cmake -H. -Bbuild-cmake -GNinja
cmake --build build-cmake --target GeneratePortO2R

# Build the APK. Use -PapplicationId=com.yourname.lighthouse for your own identifier
cd android
./gradlew assembleRelease

# Install it
adb install -r ../build-android/lighthouse-release.apk
```

_Note: with no keystore given the release APK is signed with the debug key, which installs on your own device but is not fit to hand out. To sign it properly, pass `-PkeystoreFile`, `-PkeystorePassword`, `-PkeyAlias` and `-PkeyPassword`. Do not build `assembleDebug` to play: it compiles the game at `-O0`, and asset extraction then takes tens of minutes._

### Getting the game onto a device
1. Copy a supported Banjo-Kazooie ROM (`.z64`) onto the device, anywhere you like — Downloads is fine.
2. Install and launch the app. It asks for a ROM and opens the system file picker; choose the ROM.
3. Let the app extract `bk.o2r`. This takes a few minutes and needs roughly 200 MB free.

_Note: the app keeps its data in `Android/data/<applicationId>/files`, which Android 11 closed to the Files app, to USB, and to the file picker alike. That is why the ROM is imported through the picker rather than copied in by hand, and it is also why saves and mods are not reachable from the device itself; use `adb` for those._

### Android notes
* arm64-v8a only, landscape, full screen. A device without OpenGL ES 3.0 is not supported.
* Any controller SDL2 recognises works. Pair it in Android Settings first; the on-screen pad hides itself while one is connected.
* The on-screen controls are configured in *Settings > Controls*, under **On-Screen Controls**, and are sized from the reported screen density rather than a fixed point size.
* Mods go in the `mods` folder, which is only reachable over `adb` for the reason above. Applying a mod list needs the app to be closed and reopened, since Android apps can't relaunch themselves.
* Networking (Anchor multiplayer) is off, SDL2_net isn't part of the Android dependency set.

# Compatible Roms
Any retail version. See [the readme](https://github.com/HarbourMasters/Lighthouse/blob/develop/README.md#1-verify-your-rom-dump)

## Getting CI to work on your fork

The CI works via [Github Actions](https://github.com/features/actions) where we mostly make use of machines hosted by Github; except for the very first step of the CI process called "Extract assets". This step extracts assets from the ROM and generates the `bk.o2r` archive.

To get this step working on your fork, you'll need to enable actions for all sources in the Actions tab on GitHub. You can also add your own local runner on GitHub.

### Runner on Windows
You'll have to enable the ability to run unsigned scripts through PowerShell. To do this, open Powershell as administrator and run `set-executionpolicy remotesigned`. Most dependencies get installed as part of the CI process. You will also need to separately install 7z and add it to the PATH so `7z` can be run as a command. [Chocolatey](https://chocolatey.org/) or other package managers can be used to install it easily.

### Runner on UNIX systems
If you're on macOS or Linux take a look at `macports-deps.txt` or `apt-deps.txt` to see the dependencies expected to be on your machine.

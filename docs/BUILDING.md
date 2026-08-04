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

Requires a Mac with Xcode 15 or newer, plus `cmake` and `ninja` (Homebrew or MacPorts).
Everything else — SDL2, libzip, spdlog, tinyxml2, nlohmann-json and metal-cpp — is fetched
by libultraship during configure. The renderer is Metal; there is no OpenGL ES path.

The build cross-compiles, so the asset packer (Torch) has to run on the Mac itself. The
easiest route is to generate `lighthouse.o2r` once from a normal macOS build tree and let
the iOS configure pick it up.

```bash
git clone https://github.com/HarbourMasters/Lighthouse.git
cd Lighthouse
git submodule update --init

# 1. Generate the port asset archive on the host.
cmake -H. -Bbuild-cmake -GNinja
cmake --build build-cmake --target GeneratePortO2R

# 2. Configure the iOS project. -G Xcode is required: the app bundle, asset catalog
#    and code signing all go through Xcode's build phases.
#    CMAKE_IGNORE_PREFIX_PATH keeps the iOS configure from picking up the Homebrew /
#    MacPorts SDL2 and libzip you installed for the desktop build — they are the wrong
#    architecture, and libultraship would use them instead of the versions it fetches.
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
  -DPLATFORM=OS64 \
  -DDEPLOYMENT_TARGET=16.0 \
  -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;/opt/local" \
  -DPROJECT_ID=com.yourname.lighthouse \
  -DIOS_DEVELOPMENT_TEAM=YOURTEAMID

# 3. Build (or open build-ios/Lighthouse.xcodeproj and hit Run).
#    Everything after `--` goes to xcodebuild. -allowProvisioningUpdates lets it create
#    the provisioning profile for PROJECT_ID; without it the build fails before compiling
#    with "No profiles for '<bundle id>' were found". Xcode.app does this silently, the
#    command line does not. Drop the flag if you left IOS_DEVELOPMENT_TEAM unset.
cmake --build build-ios --config Release --target Lighthouse -- -allowProvisioningUpdates
```

Building the `Lighthouse` target rather than `ALL_BUILD` skips the SDL2 dylib (the app
links `SDL2::SDL2-static`) and `TorchExternal`, the host macOS Torch used by step 1. The
asset-bundling step below is attached to the `Lighthouse` target, so it still runs.

`IOS_DEVELOPMENT_TEAM` is your 10-character Apple Developer Team ID, from
<https://developer.apple.com/account> → Membership. Leave it unset to configure and compile
without an Apple account; the resulting app just can't be installed on a device.
`PROJECT_ID` must be a bundle identifier your team owns.

A free Apple ID works too — add it under Xcode → Settings → Accounts, and use the personal
team's ID (`xcrun security find-identity -v -p codesigning` lists it, or open the generated
project and pick the team in *Signing & Capabilities*). Free provisioning profiles expire
after 7 days, so the app has to be reinstalled weekly.

If step 3 still reports that no profiles were found, open `build-ios/Lighthouse.xcodeproj`
once, select the *Lighthouse* target → *Signing & Capabilities* and pick your team there.
Xcode registers the bundle ID and creates the profile; command-line builds work afterwards.
This is usually only needed with a free personal team.

`lighthouse.o2r`, `config.yml`, `assets/yaml` and `gamecontrollerdb.txt` are copied into the
`.app` after linking. If the build warns that `lighthouse.o2r` was not found, run step 1.

### Getting the game onto a device

`bk.o2r` is **not** built at compile time for iOS — the app extracts it on-device, the same
way the desktop builds do:

1. Install and launch the app once. It creates a `Lighthouse` folder under
   *On My iPhone* / *On My iPad* in the Files app.
2. Copy a supported Banjo-Kazooie ROM (`.z64`) into that folder.
3. Relaunch. Lighthouse finds the ROM, asks to extract, and writes `bk.o2r` next to it.
   Extraction takes a few minutes and needs roughly 200 MB free.

Saves, `lighthouse.cfg.json` and the `mods` folder all live in the same directory, so they
can be backed up or edited from the Files app or over USB in Finder.

### iOS notes

* **Layout**: landscape only, full screen, rendered at the display's native pixel
  resolution. ProMotion displays are allowed to run above 60 Hz.
* **Controllers**: any MFi / Bluetooth controller SDL2 recognises (Xbox, DualSense,
  DualShock 4, Backbone, 8BitDo...) works via the GameController framework. Pair it in
  iOS Settings first; the on-screen pad hides itself while one is connected.
* **On-screen controls**: drawn by `src/port/Controller/TouchControls.cpp` and merged into
  the pad in `OS_SiService`, so they feed the same path as a physical controller. Size,
  opacity, edge margin, stick deadzone and the optional D-pad live under
  *Settings → Controls*. The `MENU` button at the top of the screen opens the port menu,
  which is otherwise bound to Escape.
* **Mods** work exactly as on desktop — drop `.o2r`/`.otr` files into `Lighthouse/mods`
  via the Files app. Applying a mod list needs a restart, and since iOS apps can't relaunch
  themselves the menu asks you to close the app and reopen it from the Home Screen.
* **Networking** (Anchor multiplayer) is off by default on iOS because SDL2_net isn't part
  of the iOS dependency set.

### Companion libultraship changes

iOS support needs three small fixes in the `libultraship` submodule, which have to land
upstream (or in your fork) before this builds:

* `src/fast/backends/gfx_sdl2.cpp` — request `SDL_WINDOW_ALLOW_HIGHDPI` so the Metal
  drawable is the display's native pixel size instead of 1x (this is what macOS already
  does for Retina), and stop routing fullscreen through the Cocoa-only helpers, which
  aren't compiled for iOS and would fail to link.
* `src/fast/Fast3dWindow.cpp` — only advertise the OpenGL backend when it was compiled in.
  Without this a config carrying `"Window.Backend.Id": 2`, e.g. copied from a desktop
  install, selects a backend iOS has no case for and crashes on a null renderer.
* `src/ship/Context.cpp` — `GetAppBundlePath()` returns the read-only app bundle on iOS
  instead of `Documents`, so shipped assets and user data are separate.
* `src/CMakeLists.txt` — guard the code-signing tweak for the `zip` target with
  `if(TARGET zip)`; it only exists when libzip came from FetchContent, so configure fails
  outright on a machine that has libzip installed.

# Compatible Roms
Any retail version. See [the readme](https://github.com/HarbourMasters/Lighthouse/blob/develop/README.md#1-verify-your-rom-dump)

## Getting CI to work on your fork

The CI works via [Github Actions](https://github.com/features/actions) where we mostly make use of machines hosted by Github; except for the very first step of the CI process called "Extract assets". This step extracts assets from the ROM and generates the `bk.o2r` archive.

To get this step working on your fork, you'll need to enable actions for all sources in the Actions tab on GitHub. You can also add your own local runner on GitHub.

### Runner on Windows
You'll have to enable the ability to run unsigned scripts through PowerShell. To do this, open Powershell as administrator and run `set-executionpolicy remotesigned`. Most dependencies get installed as part of the CI process. You will also need to separately install 7z and add it to the PATH so `7z` can be run as a command. [Chocolatey](https://chocolatey.org/) or other package managers can be used to install it easily.

### Runner on UNIX systems
If you're on macOS or Linux take a look at `macports-deps.txt` or `apt-deps.txt` to see the dependencies expected to be on your machine.

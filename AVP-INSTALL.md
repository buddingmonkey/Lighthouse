# Installing Lighthouse (Mobile and XR) on Apple Vision Pro

[Lighthouse](https://github.com/HarbourMasters/Lighthouse) is the Harbour Masters port of Banjo-Kazooie. This fork adds Android, Meta Quest, Galaxy XR, iPhone, iPad and Apple Vision Pro builds. On Vision Pro the app is a native RealityKit shell that holds the game on a plate in a volume in the Shared Space, rendered with Metal. It's a window, not a head-mounted camera: head motion changes the projection, so the world reaches behind the glass.

## What you need

- A Mac with Xcode 26 or newer and the visionOS SDK, plus `cmake` and `ninja`
- An Apple ID. A free one works; its profile expires after 7 days, and you then build again. A paid membership gives a one-year profile.
- Apple Vision Pro. The app is tested on visionOS 26.
- A paired game controller. It's needed to play.
- Your own Banjo-Kazooie ROM, as a `.z64` file

## Your game files

Lighthouse holds no game data. The app makes `bk.o2r` from your ROM on the first start, on the headset. This takes a few minutes and needs about 200 MB free.

Any retail version works. Check your dump against these SHA-1 sums:

| ROM | SHA-1 |
| --- | --- |
| `baserom.us.v10.z64` | `1fe1632098865f639e22c11b9a81ee8f29c75d7a` |
| `baserom.us.v11.z64` | `ded6ee166e740ad1bc810fd678a84b48e245ab80` |
| `baserom.jp.z64` | `90726d7e7cd5bf6cdfd38f45c9acbf4d45bd9fd8` |
| `baserom.pal.z64` | `bb359a75941df74bf7290212c89fbc6e2c5601fe` |

The file must be `.z64`; convert an `.n64` with <https://hack64.net/tools/swapper.php>.

1. Install the app (below) and start it once. It makes a `Lighthouse` folder under *On My Apple Vision Pro* in the Files app.
2. Copy your `.z64` into that folder.
3. Start the app again. Answer **Yes** to *"No O2R files found. Generate one now?"*, then **Yes** to *"ROMs found in application directory. Would you like to process them?"*. Answering **No** to the first question closes the app; that isn't a fault, as the game can't start without `bk.o2r`.

Saves, `lighthouse.cfg.json` and the `mods` folder are in the same place.

## Build from source

There is no download for Apple Vision Pro; you build the app on a Mac and run it from Xcode. CMake generates the Xcode project once, and Xcode builds, signs and installs it. The build cross-compiles, so `lighthouse.o2r` comes from a macOS build tree, and `bk.o2r` is extracted on the headset.

From a checkout of this repository, with its submodules (`git submodule update --init` if you don't have them yet):

```bash
# Generate lighthouse.o2r (port-specific assets) on the host
cmake -H. -Bbuild-cmake -GNinja
cmake --build build-cmake --target GeneratePortO2R

# Generate the Xcode project
cmake -S . -B build-visionos -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
  -DPLATFORM=VISIONOS \
  -DDEPLOYMENT_TARGET=2.0 \
  -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;/opt/local" \
  -DPROJECT_ID=com.yourname.lighthouse.vision \
  -DIOS_DEVELOPMENT_TEAM=YOURTEAMID

open build-visionos/Lighthouse.xcodeproj
```

`IOS_DEVELOPMENT_TEAM` is your 10-character Apple Developer Team ID (from <https://developer.apple.com/account>), and `PROJECT_ID` a bundle identifier your team owns. A free Apple ID added under *Xcode > Settings > Accounts* works.

1. Pair the headset under *Window > Devices and Simulators*.
2. Select the **Lighthouse** scheme, choose your Vision Pro, and press **Run** (⌘R). If Xcode reports that no profiles were found, pick your team under the Lighthouse target's *Signing & Capabilities*.

Run the Release configuration, which the generated scheme already uses: a Debug build makes the first-start extraction about 60 times slower. `-DPLATFORM=SIMULATOR_VISIONOS` makes a Simulator project instead, which needs no signing; the Simulator reports one view, so it can't show the stereo result, but placement and the menu can be checked there. The full instructions are in [docs/BUILDING.md](docs/BUILDING.md#visionos-apple-vision-pro).

## Notes

- The paired controller plays the game. Look and pinch drive the menu, and the **Menu** button in the ornament under the volume opens it. A paired keyboard opens and closes the menu with Escape.
- *Settings → Graphics* has the headset settings: Diorama Depth (how deep the world reaches behind the glass; a small depth is easiest for long sessions), Window Range, Window Size, Edge Float, Edge Softness, Max Refresh Rate, Recenter Window, and Stereo (turn it off to halve the drawing cost).
- Mods (`.o2r` or `.otr`) go in the `mods` folder inside the `Lighthouse` folder in the Files app. Applying a mod list needs the app to be closed and opened again.
- Anchor multiplayer is off on the headsets.
- On Windows, Linux, macOS or Switch, use [HarbourMasters/Lighthouse](https://github.com/HarbourMasters/Lighthouse/releases) instead; it is the canonical port.

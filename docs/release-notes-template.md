## What this release holds

This release ships the **Android build only**: a phone, a Samsung Galaxy XR and a Meta Quest 3 or 3S.

For Windows, Linux and macOS, use the release from the main Lighthouse project at <https://github.com/HarbourMasters/Lighthouse/releases>. This project builds and tests those platforms on every change, but it does not publish them. Two sets of desktop binaries from two places would only put users on a build that nobody supports.

## What is new in this release

For the changes in 1.1.3 (foldable and dual-screen phones, menu access with L3 + R3, and the new defaults), see the 1.1.3 release.

### Dual-screen phones (Ayn Thor)

- The screen that does not show the game now shows the original box art. To show a different image or a black screen, go to **Settings > Graphics > Other Screen Image**. The choices are Cover Art, Hero, Jiggies, Poster, Cover Back, Box and Black. The app keeps your choice.
- When you move the game to the other screen with **Settings > Graphics > Game Screen**, the image moves to the screen that the game left. Your controller stays connected to the game.

### Menu

- The lists in the menu now show their choices in a fixed order. Before, some lists on Android showed their choices in reverse order, for example **Second Screen** above **Main Screen**.
- On a narrow screen or at a large menu scale, the top bar of the menu now wraps onto two rows. Before, **Rando** and **Search** were cut off, and you could not reach them by touch. The side bar is now wide enough for its labels. This applies, for example, to the second screen of the Ayn Thor.

### Fixes

- **Falling-jiggy transition:** on a screen narrower than 4:3, for example a foldable in portrait or the second screen of the Ayn Thor, the puzzle-piece transition showed streaks at the top and bottom of the screen. The transition now fills the screen correctly.

## Before you start

Lighthouse does not contain the game. You must supply your own Banjo-Kazooie ROM. The app asks for the ROM on the first start and makes `bk.o2r` from it.

## Where your files live

The app keeps its data in `Android/media/com.harbormasters.lighthouse` in internal storage. The Files app, a file manager in a headset and a PC over USB all reach that folder. Your saves, `lighthouse.cfg.json` and the `mods` folder are in it.

To load the game, copy your ROM into that folder and start the app. Answer **Yes** to *"No O2R files found. Generate one now?"*, then **Yes** to *"ROMs found in application directory"*. If the folder holds no ROM, the system document picker opens and takes a ROM from anywhere on the device.

If you have an earlier version, the app moves your data into the new folder the first time it starts, and it keeps your saves. Install the update over the old version. **Do not uninstall first**, because an uninstall deletes the data of the old version.

## One APK covers all three devices

There is no separate Quest file. The same package declares the phone, the Google XR and the Horizon OS feature sets, and it starts in the correct mode on each one.

The APK is `arm64-v8a` only. It runs on every shipping phone and headset. It does not run on an x86_64 emulator.

## Apple platforms

iOS and visionOS are not in this release. Build them from the source.


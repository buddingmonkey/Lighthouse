## What this release holds

This release ships the **Android build only**: a phone, a Samsung Galaxy XR and a Meta Quest 3 or 3S.

For Windows, Linux and macOS, use the release from the main Lighthouse project at <https://github.com/HarbourMasters/Lighthouse/releases>. This project builds and tests those platforms on every change, but it does not publish them. Two sets of desktop binaries from two places would only put users on a build that nobody supports.

## What is new in this release

### Foldable and dual-screen phones

- **Foldables (Pixel Fold, Galaxy Z Fold):** on the large inner screen, the game turns with the device. In portrait, the game is at the top and the touch controls are in the space below. The outer screen stays in landscape.
- **Dual-screen phones (Ayn Thor):** the game starts on the main screen. To move it to the other screen, go to **Settings > Graphics > Game Screen**. The app keeps your choice.

### Menu access

- Press both sticks in (**L3 + R3**) at the same time to open or close the menu. This works with a gamepad on a phone or tablet, with a gamepad on the Galaxy XR, and with the Touch controllers on the Meta Quest.
- **Phone or tablet:** while a gamepad is connected, the menu button is hidden. To show it, go to **Settings > Controls > Menu Button > Show Menu Button With Gamepad**.
- **Headsets:** the menu button stays on the window. To hide it while controllers are connected, clear **Settings > Controls > Menu Button > Show Menu Button With Controllers**.
- You can use the menu with a gamepad: the D-pad moves from item to item, **A** selects and **B** goes back. This is on by default. To change it, go to **Settings > General > Menu Controller Navigation**.

### Other changes

- **Frame rate:** the game now matches the refresh rate of your display by default. To change it, go to **Settings > Graphics > Match Refresh Rate**.
- **Anti-aliasing:** the default MSAA is now 2. A value that you set before does not change.
- **Renderer list:** **Settings > Graphics > Renderer API** now shows one OpenGL entry, not two.
- **Meta Quest:** after you add a mod with **Add Mod from File**, the game takes input again. Before, the controllers stopped working after the file picker closed, and you had to restart the app.

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


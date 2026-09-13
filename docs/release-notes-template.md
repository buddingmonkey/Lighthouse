## Before you start

Lighthouse does not contain the game. You must supply your own Banjo-Kazooie ROM. The app asks for
the ROM on the first start and makes `bk.o2r` from it. The `README` in the repository lists the supported
versions and their checksums.

## Which file do I need?

| File | Platform |
| --- | --- |
| `Lighthouse-*-android-universal.apk` | Android phone or tablet, Samsung Galaxy XR, Meta Quest 3 and 3S |
| `Lighthouse-*-windows-x64.zip` | Windows 10 and 11, 64 bit |
| `Lighthouse-*-linux-x86_64.AppImage` | Linux, 64 bit. Put `Lighthouse-*-linux-extras.zip` next to it. |

**One APK covers all three Android devices.** There is no separate Quest file. The same package
declares the phone, the Google XR and the Horizon OS feature sets, and it starts in the correct
mode on each one.

The APK is `arm64-v8a` only. It runs on every shipping phone and headset. It does not run on an
x86_64 emulator.

`Lighthouse-*-windows-x64-pdb.zip` holds the Windows debug symbols. You need it only to give a
usable crash report.

## Installing the APK

Copy it to an Android device and open it, or `adb install -r <file>`. On a Quest, turn on developer
mode in the Meta Horizon phone app first; the app is then in the library under *Unknown Sources*.
On a Galaxy XR, turn on Developer options and USB debugging.

## Apple platforms

Apple permits no sideloading, so iOS and visionOS have no download. Build the app on a Mac and run
it on your device from Xcode. A free Apple ID is enough, with a profile that expires after 7 days.
The steps are in `docs/BUILDING.md` in the repository.

## Windows, Linux and macOS

[HarbourMasters/Lighthouse](https://github.com/HarbourMasters/Lighthouse/releases) is the canonical
port for the desktop platforms. The desktop files here are built from the same tree as the mobile
and XR ones and are given only for convenience.

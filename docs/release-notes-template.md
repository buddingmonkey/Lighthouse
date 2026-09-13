## Before you start

Lighthouse does not contain the game. You must supply your own Banjo-Kazooie ROM. The app asks for
the ROM on the first start and makes `bk.o2r` from it.

## Which file do I need?

| File | Platform |
| --- | --- |
| `Lighthouse-*-windows-x64.zip` | Windows 10 and 11, 64 bit |
| `Lighthouse-*-linux-x86_64.AppImage` | Linux, 64 bit. Put `Lighthouse-*-linux-extras.zip` next to it. |
| `Lighthouse-*-android-universal.apk` | Android phone, Samsung Galaxy XR, Meta Quest 3 and 3S |

**One APK covers all three Android devices.** There is no separate Quest file. The same package
declares the phone, the Google XR and the Horizon OS feature sets, and it starts in the correct
mode on each one.

The APK is `arm64-v8a` only. It runs on every shipping phone and headset. It does not run on an
x86_64 emulator.

`Lighthouse-*-windows-x64-pdb.zip` holds the Windows debug symbols. You need it only to give a
usable crash report.

## Apple platforms

macOS, iOS and visionOS are not in this release. Build them from the source.

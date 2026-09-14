## What this release holds

This release ships the **Android build only**: a phone, a Samsung Galaxy XR and a Meta Quest 3
or 3S.

For Windows, Linux and macOS, use the release from the main Lighthouse project at
<https://github.com/HarbourMasters/Lighthouse/releases>. This project builds and tests those
platforms on every change, but it does not publish them. Two sets of desktop binaries from two
places would only put users on a build that nobody supports.

## Before you start

Lighthouse does not contain the game. You must supply your own Banjo-Kazooie ROM. The app asks for
the ROM on the first start and makes `bk.o2r` from it.

## One APK covers all three devices

There is no separate Quest file. The same package declares the phone, the Google XR and the
Horizon OS feature sets, and it starts in the correct mode on each one.

The APK is `arm64-v8a` only. It runs on every shipping phone and headset. It does not run on an
x86_64 emulator.

## Apple platforms

iOS and visionOS are not in this release. Build them from the source.

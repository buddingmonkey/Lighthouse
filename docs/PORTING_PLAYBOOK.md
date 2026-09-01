# Desktop → Mobile → XR Porting Playbook

This document records how the Lighthouse (Banjo-Kazooie) port went from desktop to iOS,
to Android, to three XR targets (Android XR, Meta Quest 3, Apple Vision Pro). It is written
so that an agent or an engineer can repeat the same port on another Harbour Masters
decompilation (Ship of Harkinian, 2 Ship 2 Harkinian, Starship, or a future port) with a
minimum of rediscovery.

Every phase below gives: the goal, the steps in order, the traps found the hard way
(symptom → root cause → fix), the verification that closed the phase, and the commits that
hold the full story. **The commit messages are the primary record.** Each one carries the
mechanism, the measurement, and the debugging story. Read them with
`git log --follow <file>` and `git show <hash>` before you re-investigate anything.

Commit references use two prefixes:

- `lh <hash>` — the superproject (this repository, branches `ios-support`,
  `android-support`, `xr-integration`).
- `lus <hash>` — the `libultraship` submodule (fork branches of the same names).

Machine-specific facts (device identifiers, hostnames, signing teams, personal upload
scripts) are deliberately not in this file. They belong in untracked local notes.

---

## Part 0 — The lay of the land

### 0.1 Architecture

Every Harbour Masters port has the same two-layer shape:

| Layer | Contents | Where a change belongs |
| --- | --- | --- |
| Superproject `src/` | Decompiled game code. Do not reformat or restyle it. | Only game-behavior patches, guarded and minimal. |
| Superproject `src/port/` | The port layer: engine loop, extractor, menu, save, controls. | Game-facing features: touch controls, menus, lifecycle glue, pacing. |
| `libultraship/` submodule | The engine: rendering (Fast3D interpreter + backends), audio, input, window, resource manager. | Everything platform: window backends, GL/Metal fixes, audio backends, XR. |

**The single most important discipline:** decide for every change which layer it belongs
to, and inside `libultraship`, whether it is generic (upstreamable to
`Kenix3/libultraship`) or fork-only. Decide this when you plan the change, not when you
submit it. See Part 12.

### 0.2 Branch topology that worked

Both repositories carry the same branch names in parallel, and the superproject pins the
submodule at each step:

```
develop (upstream)
  └─ ios-support          (phase 1–5: iOS)
       └─ android-support (phase 6: Android)
            └─ xr-integration  (phases 7–11: Android XR, Quest, perf, visionOS)
                 ├─ xr-window-range   (topic branch, merged)
                 └─ xr-eye-strain     (topic branch, merged)
```

Upstream-bound slices are cherry-picked onto clean upstream bases as `lus/<topic>` and
`lh/<topic>` branches (Part 12). The dev branches keep fuller comments and the fork
submodule pin; the PR branches keep the upstream pin and near-zero comments.

### 0.3 The target sequence, and why this order

1. **iOS first.** It shares the Metal backend and most build plumbing with macOS, which
   upstream already supports, so the delta is smallest. It also forces the mobile
   lifecycle work (backgrounding, audio sessions, touch) that Android reuses.
2. **Android second.** Reuses the mobile split from iOS. Its real content is GLES
   correctness on a strict driver (Mali) — bugs Mesa forgives on desktop.
3. **Android XR / Quest third.** OpenXR on top of the Android build. The engine keeps the
   SDL window and GLES context; OpenXR only replaces presentation.
4. **visionOS last.** No SDL video at all; a native RealityKit shell feeds an external
   Metal target and shows the result in a volumetric window. Hardest, but by then the XR
   camera model already exists and is backend-independent.

---

## Part 1 — iOS bring-up

**Goal:** the game boots, renders, and plays at full rate on an iPhone/iPad, built from
CMake with an Xcode generator.

### 1.1 Build system

Steps, in the order they were needed:

1. Add the app target behind a platform define, an `Info.plist.in`, an asset catalog, and
   a resource-bundling CMake script that copies the generated `.o2r` archives into the
   app bundle (lh `37f1b9d3`).
2. The asset packer (Torch) must run on the **host**. Pin it as an `ExternalProject`
   configured for the host, single-config (lh `37f1b9d3`).
3. Extraction of game assets from the ROM runs **on device**, with the extractor
   statically linked into the app.
4. Generate a shared Xcode scheme from CMake with a cache variable selecting its
   configuration (lh `033553ee`). Without a shared scheme, Xcode invents a Debug-only
   one — and a Debug app made on-device extraction ~60× slower.
5. Build only the app target, never `ALL_BUILD` (the host packer target fails inside
   xcodebuild). Document `-allowProvisioningUpdates` (lh `6bca01e9`).

Build traps, all of which will recur on any port using the same toolchain:

| Symptom | Root cause | Fix |
| --- | --- | --- |
| `call to undeclared function 'memcpy_s'` in libzip | `ios.toolchain.cmake` sets `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`, so link-based `check_function_exists` probes compile but never link — all 33 return true. Worse: libzip's own `project()` re-reads the toolchain file and restores the setting inside libzip's scope, so a scoped fix silently does nothing. | Run the link probes in the outer scope into private variables, then overwrite libzip's `HAVE_*` unconditionally (lus `c37f4033`, `5ff5a609`). Clear poisoned caches with a fresh build dir. |
| Undefined `_ZSTD_*` at link | libzip found the host package manager's x86_64 `libzstd.a`; the arm64 linker skipped it. | `ENABLE_ZSTD OFF`, `ENABLE_LZMA OFF`; bzip2 stays (resolves to the SDK's `.tbd`, right arch) (lus `ab25924c`). Also pass `CMAKE_IGNORE_PREFIX_PATH` for the package-manager prefixes at configure. |
| Concurrent host + iOS builds corrupt each other | `add_subdirectory(libultraship <source-dir>)` — the second argument is the *binary* dir and pointed into the source tree, so every build tree shared one output location. | Plain `add_subdirectory(libultraship)`; delete stale generated headers from the source tree once (lh `d7dca26c`, `ba89808a`). |
| On-device extraction takes ~45 min | Two causes: `ExternalProject` does not inherit `CMAKE_BUILD_TYPE` (host packer built unoptimized: 2313 s vs 38 s at `-O3`); and the on-device extractor inherits the app configuration, which Xcode defaulted to Debug. | Pass an explicit build type to the external project (lh `19fd5948`); always extract with a Release app. |
| Full-tree iOS build links a macOS tool against iOS | Empty `CMAKE_OSX_DEPLOYMENT_TARGET` emits no `-mmacosx-version-min`, so clang falls back to `IPHONEOS_DEPLOYMENT_TARGET`, which xcodebuild exports into script phases. | Pass the host's own OS version to the host tool's configure (lh `81a57aa1`). |

### 1.2 The bring-up bug chain

Eight bugs hid behind each other; found one at a time. Two are not iOS bugs at all. This
table is the template for what to expect on any new Apple target:

| # | Symptom | Root cause | Fix | Generality |
| --- | --- | --- | --- | --- |
| 1 | Fullscreen symbols unlinkable; wrong drawable size; null renderer; bundled assets unfindable | Four small platform assumptions in the window backend | Early-return fullscreen ops; request `SDL_WINDOW_ALLOW_HIGHDPI`; only advertise the GL backend when compiled in (a stale config id otherwise selects a nonexistent backend); return the real bundle path for read-only assets (lus `f84acbc5`) | iOS |
| 2 | Debug build fails: `Expected parameter declarator` in `ultra_assert.h` | Apple's `_assert.h` defines `__assert` as a function-like macro; ultralib re-declares it | Declare only under `#ifndef __assert` (lh `514330e8`) | **All Apple Debug builds** |
| 3 | SIGSEGV on every exit, inside spdlog from `~Context` | Every exit path nulled a **non-owning** raw pointer; the real owner (a static `unique_ptr`) destroyed `Context` during `exit()` teardown, after spdlog's statics died | Call `Context::DestroyInstance()` at the exit sites (lh `b4f7b493`) | **Every platform** |
| 4 | SIGSEGV at 0x0 during extraction exit | `~Context` assumed full initialization; the extractor quits before `InitLogging()`, so `mLogger->flush()` dereferenced null. Masked because visible logs come from spdlog's *default* logger | Null-check staged members in the destructor (lus `30bd8b06`) | Every platform with staged construction |
| 5 | Black screen; game alive at 60 fps behind it | The Metal renderer silently drops the whole ImGui frame when `DisplaySize × FramebufferScale` ≠ screen texture size. ImGui's SDL2 backend derives the scale from `SDL_GL_GetDrawableSize()`, which SDL's UIKit path implements **only for GL views**; a Metal view reports points, so scale = 1.0 on a 2× panel | Derive scale from `SDL_Metal_GetDrawableSize()`; make the silent guard log once (lus `68bbfae9`) | iOS (macOS escapes only on non-HiDPI) |
| 6 | ImGui window-stack corruption after dismissing a modal | `EndPopup()` called unconditionally outside `if (BeginPopupModal(...))` | Move it inside (lh `077565f7`) | **Every platform** |
| 7 | SIGSEGV in the graphics thread at an address like `0x100000005` | `OS_MESG_32(x)` writes 4 of the union's 8 bytes; receivers discriminate event-vs-pointer on all 64 bits. Stack garbage in the high word. x86_64 survived on lucky zeroes | Construct through `.ptr` (lus `6c733b8c`) | **Latent on every 64-bit target.** The fault-address fingerprint (correct low word, garbage high word) is the tell |
| 8 | After backgrounding, permanently ~1 fps | Frames rendered off-screen never present; three of them drain the `CAMetalLayer` drawable pool; `nextDrawable` then times out at **1017 ms per frame**, forever. metal-cpp is `objc_msgSend`, so the nil drawable propagated silently | Port layer: stop rendering while off screen, gated on will-resign-active / did-become-active (lh `e12dc2c7`). Engine: keep the previous texture, skip present on nil, log first occurrence (lus `62ba914e`) | The off-screen park is reused verbatim by Android and visionOS |

**Doctrine from this chain:** when a mobile bring-up misbehaves, suspect a silent failure
path first. Nearly every fix above added a one-shot diagnostic at the exact point that
failed silently. Only the app's own stderr is reachable on device — anything you must
know, the app has to print.

### 1.3 Audio (CoreAudio on iOS)

1. Port the desktop CoreAudio player: `kAudioUnitSubType_RemoteIO` instead of
   `HALOutput`; ring buffer and render callback unchanged. Add an `AVAudioSession`
   configuration file — the default `SoloAmbient` category is muted by the ring/silent
   switch, so the unit runs with no audible output (lus `ed6bf79e`).
2. Interruptions: a call/Siri/alarm stops the unit and iOS never restarts it. Observe the
   notifications; stop on begin, reactivate + restart on end; resume even without the
   ShouldResume hint. Run handlers under the lock that the remove-observer call takes, so
   close can fence in-flight handlers (lus `6ae78ce3`).
3. Surround fallback: every iOS route is stereo; a 6-channel request must retry in stereo
   rather than fall through to the null player (= silence). Keep the saved preference so
   desktop still retries surround (lus `6ae78ce3`).
4. Route loss (`OldDeviceUnavailable`) restarts like an interruption end; a
   media-services reset invalidates every CoreAudio object — dispose and rebuild the unit
   and session (lus `0846ae77`).
5. Lifecycle: add `AudioPlayer::Suspend()/Resume()` (default no-ops). iOS must deactivate
   the session on background — a suspended process otherwise keeps the audio route
   claimed against other apps (lus `6e6dc5c6`). Watch the lock order: dispose the unit
   *before* taking the ring-buffer mutex, because `AudioOutputUnitStop` waits on a render
   callback that would wait on that mutex.

### 1.4 On-screen touch controls

The controls live entirely in the port layer (`src/port/Controller/TouchControls.cpp`),
drawn with ImGui draw-list primitives, merged into the pad on the SDL-pumping thread
inside the SI service (lh `37f1b9d3`).

The design that survived:

- **Lay out in millimeters, not screen fractions.** Screen-fraction layout drew tablet
  buttons 2.6× too big. ImGui points are a fixed physical size; get density from
  `SDL_GetDisplayDPI`. Classify phone vs tablet and place shoulder buttons on a top rail
  (phone) or side edges (tablet). Controls sit on thumb arcs from the grip corners
  (lh `f89254de`).
- **Make the layout a pure function and fuzz it.** Driving `BuildLayout` over 6 device
  sizes × setting extremes (1296 combinations), asserting no off-screen and no overlap,
  caught 4 real layout bugs before any device test (lh `f89254de`).
- Stick math: clamping travel past the ring must divide by the post-clamp length or
  deflection falls off as 1/len. Extract a pure `StickVector()` and unit-test it
  (lh `95f3372d`).
- Gameplay integration: a control-scheme pass must only clear the C bits *it* asserted,
  not all of them (lh `7c871cc8`); a "Modern" camera scheme needs the touch C-diamond to
  become a genuine right stick writing axis values at gamepad calibration
  (lh `ed8ff88b`).
- ImGui on touch needs drag-to-scroll plus inertia, gated to touch-source input and
  mobile only (lus `454ed73b`, `84ad9e30`).

### 1.5 Lifecycle, launch, and store hygiene

- Install the lifecycle watch **before** the extractor runs, or its off-screen gate never
  fires. Memory warning: drop the texture cache and log — jetsam kills without a second
  notification. No quit control in the menu on iOS: `exit()` reads as a crash to the
  system (lh `5e9845aa`).
- Archive integrity: a zero-byte or truncated `.o2r` opens as valid-empty under
  `ZIP_CREATE` and boots assetless. Extract into a staging directory and move
  atomically; verify readability at boot and move bad archives aside instead of deleting
  the user's file (lh `5e9845aa`, later split as its own upstream PR).
- Launch: an empty `UILaunchScreen` is white; over a black Metal view that reads as a
  hang. Name a launch background color. Measure before optimizing: cold start was
  ~2.5 s, warm <1 s — most of the perceived slowness was the Debug scheme (70 MB vs
  14 MB binary; 1291 ms vs 205 ms pre-main page-in) (lh `ea6ac804`).
- App Store: SDL's HIDAPI links CoreBluetooth for one BLE controller nobody uses on iOS;
  it trips a usage-string requirement at submission. `SDL_HIDAPI OFF`
  (lus `89eceebc`). CMake's Xcode generator leaves `INSTALL_PATH` empty, which makes
  archives "generic" and unexportable — set `$(LOCAL_APPS_DIR)` (lh `ef175aec`).

**Phase gate:** boots on device, 60 fps sustained, audio through the speaker and through
an interruption, backgrounding and returning clean, extraction under two minutes,
touch controls playable, archive survives a kill mid-extraction.

---

## Part 2 — Android

**Goal:** the same game as an arm64-v8a APK on GLES 3.0, with the mobile layer shared
with iOS rather than duplicated.

### 2.1 The mobile split

Before adding the second platform, split the first platform's guards
(lh `87ae4fe6`): `__IOS__` becomes `LIGHTHOUSE_MOBILE` (lifecycle, menu scale, file
handling) and `LIGHTHOUSE_TOUCH_CONTROLS` (the pad); only genuinely-Apple code stays
behind `__IOS__`. Unit assumptions must be revisited at the same time: ImGui window units
are physical pixels on Android but points on iOS, so anything sized in points needs a
density conversion (lh `1c00ab54`).

Do this **before** the Android PR chain: shared code should arrive upstream under its
final name, so reviewers never review a rename of files they just read.

### 2.2 Build

- Gradle drives CMake; the game becomes `libmain.so` loaded by `SDLActivity`
  (lh `61afaf04`).
- **One SDL2 checkout must serve both** the Java shim and the native build
  (`FETCHCONTENT_SOURCE_DIR_SDL2`); a version mismatch between shim and native SDL is a
  silent runtime break. Stamp the tag inside the checkout so a tag change re-clones even
  through CI caches (lh `61afaf04`, `f8eb3fe2`).
- APK assets must be copied out before SDL starts, re-copied when their **bytes** change.
  A versionCode-based stamp broke the first time versionCode stayed at 1: stamp with
  size+CRC of each zip entry, read from the zip directory without inflating
  (lh `3724e15a`).
- `-fsigned-char` on the game target only: decompiled N64 code assumes signed char, and
  AArch64 Linux makes it unsigned. Keep the flag off other targets — it is a codegen
  change on platforms you do not test (lh `82da2b6a`, narrowed in `db5b504f`).
- Bionic ships `execinfo.h` but declares `backtrace()` only at API 33+, so
  `__has_include` guards pass and compilation fails (lh `2aee0498`).
- Namespace-scope globals that resolve app paths run before `main`; on Android that path
  goes through SDL's JNI bridge which does not exist yet — SIGSEGV in `dlopen` before
  anything prints. Make them function-local statics (lh `03da8885`).
- The NDK's `-Wformat-security-as-error` will find every runtime string passed as an
  ImGui format string. Fix them for all platforms (lh `4728431d`).

### 2.3 GLES correctness — the Mali chapter

**The recurring lesson: Mesa is lenient and hides every one of these on desktop.** A
desktop GLES twin build (`-DUSE_OPENGLES=ON`) proves *compilation* only; the Android
emulator renders through host Mesa and is the same trap. Correctness needs the device.

| Symptom on device | Root cause | Fix |
| --- | --- | --- |
| Shader compile failure at boot | ES 3.0 context was only requested under `__APPLE__`; the default ES 2 context rejects `#version 300 es` | Request the version explicitly wherever GLES is used (lus `5185c6dd`) |
| Textures keep a previous draw's values | Uniform upload of 2 elements into a 1-element linked array is `INVALID_OPERATION` on GLES — the whole call is discarded; Mesa accepts the overrun | Upload the linked count only (lus `e0a6d35d`) |
| Colored static instead of textures; first bytes spell a path prefix | Bionic top-byte-tags 64-bit heap pointers (ARM TBI); the "is this a resource path" pointer-range check rejected every tagged pointer, so the path *string* went to the RDP as texels | Mask the top byte, `__ANDROID__` only (lus `c7b9503b`). Measured: 0/89 texture uploads matched between runs before; 200/200 after |
| Wrong wrap mode, silently | `GL_MIRROR_CLAMP_TO_EDGE` is not core ES; missing extension makes the call `INVALID_ENUM`, keeping the old mode | Detect once at init, fall back to `GL_MIRRORED_REPEAT` (lus `97e5f92c`) |
| Filtering artifacts at texture edges | `mediump` is fp16 on Mali; the three-point filter's `fract(texCoord * texSize - 0.5)` loses whole texels at range | `highp` fragment shaders — ES 3.0 guarantees it (lus `98d6cd17`) |
| Black regions after framebuffer effects | GLES forbids scaled/flipped blits from MSAA sources; Mesa tolerates, Mali refuses with black | Resolve first with an equal, unflipped blit; flip after (lus `fc8544ba`) |
| Uninitialized depth reads | The GLES path compiled out the read but returned the uninitialized stack variable | Zero it (lus `eaf1ce46`) |

### 2.4 Android UX

- ROM import: Android 11 closed shared storage, so "copy the file in" is impossible. The
  system (SAF) picker becomes one backend of the port's existing `PickFile` seam — do
  not build a separate setup activity (built once, then deleted: lh `0a9b4616` →
  `bdc6de78`). No MIME filter (`.z64` has none); sniff the first four bytes and tell
  byte-swapped ROM holders what they have (lh `ba2347d3`).
- System bars: `SDLActivity` cancels the fullscreen theme; on SDK 35 bars draw over the
  app. Hide via `WindowInsetsController` posted to the looper (to run after SDL's own
  post). Feed cutout and bar insets into the touch layout as part of its cache key, and
  inset symmetrically so a mirrored layout cannot put a control under the cutout
  (lh `06d8faa1`).
- Process reuse: Android reuses the process and re-runs `SDL_main` with stale
  process-global state; decompiled code has far too many globals to survive that. End the
  process in `onDestroy`. Also: `onDestroy` sends `SDL_QUIT` and joins, but an
  off-screen-parked loop never services the RCP → deadlock until the 10 s destroy
  timeout. Release the loop as soon as the window stops running, and unblock all OS
  queues at shutdown (lh `ba22cdaa`).

**Phase gate:** boots from the launcher (not only from adb — adb starts are always clean
and hide relaunch bugs), extraction on device, textures verified against desktop
captures, controls clear of cutout and gesture zones, close-and-relaunch four times in a
row.

---

## Part 3 — Android XR bring-up (the flat window era)

**Goal:** the unchanged Android build presents into a headset as a flat window in
passthrough, before any stereo work.

Key architecture decision: the OpenXR backend **keeps the SDL window, GLES context,
audio, and controllers**, and replaces only presentation (`SwapWindow` → blit the default
framebuffer into an OpenXR swapchain, submit as a layer). No `NativeActivity` is needed:
OpenXR accepts the EGL display/context SDL already made, via `eglGetCurrent*`
(lus `3c20f03e`). Make the backend self-degrading: any bring-up failure clears an
`mActive` flag and falls back to the flat panel.

Bring-up traps, in the order they fire:

1. **The app must start in full space** (`PROPERTY_XR_ACTIVITY_START_MODE=FULL_SPACE_UNMANAGED`
   on Android XR; a VR intent category on Quest) or there is no head pose (lh `a173ca89`).
2. **The OpenXR loader needs a manifest opt-in**:
   `<uses-native-library android:name="libopenxr.google.so" android:required="false"/>`
   inside `<application>`. Without it the loader's dlopen is refused by the linker
   namespace and the failure reads as `XR_ERROR_RUNTIME_UNAVAILABLE` — a missing-runtime
   error that is actually a permissions line. The tell is an `E/linker` line naming a
   restricted namespace (lh `a173ca89`).
3. Any menu/name map keyed by backend id will `abort()` on the new id if it uses `.at()`
   (lh `a173ca89`, again on visionOS lh `9c19cbb8`). Grep for backend-name maps early.
4. **Blend mode and color space**: the default `OPAQUE` blend gives a black void —
   enumerate and prefer `ALPHA_BLEND` (Quest advertises it only after enabling
   `XR_FB_passthrough`, lus `e862fc8e`). Color: an sRGB-format swapchain plus an
   sRGB-encoding blit double-encodes (measured: mean diff 49/255). The correct pair is an
   `SRGB8_ALPHA8` swapchain **with** `GL_FRAMEBUFFER_SRGB` disabled during the blit —
   right bytes, right meaning (lus `b8dda13b`, `13e09a64`).
5. **Passthrough with stereo needs the alpha contract**: set
   `BLEND_TEXTURE_SOURCE_ALPHA_BIT` and force alpha opaque in the cleared region, or the
   runtime may treat the layer as covering everything (lus `fac29dea`).
6. **Input**: in full space the system stops hit-testing the panel; pinches produce zero
   SDL events (proved with a temporary event log — then delete the log). Do the hit test
   in-app: aim pose + select action, ray–plane intersect, push SDL finger/mouse events.
   Pushed mouse events need a window id or ImGui's SDL backend drops them
   (lus `b368f3f2`, `13e09a64`). The on-screen pad polls `SDL_GetTouchFinger`, which
   pushed events do not populate — a held pinch must join the finger list directly
   (lh `5fcb41d0`).
7. **Anchoring**: `LOCAL` space re-origins every few seconds on Android XR (measured: the
   origin moved while an anchor held still), which reads as "the window follows my
   head". Use `UNBOUNDED` when present and hang the window on a spatial anchor, located
   per frame. Head pose comes from located views — locating `VIEW` space directly
   reports invalid while worn (lus `db91c925`). The system recenter gesture re-origins
   `LOCAL` only for apps that are *locating* it — create and poll a throwaway LOCAL
   space every frame to receive the event (lus `68a38d37`).
8. **Refresh rate**: a 60 fps game on a 72 Hz panel beats. Ask for the fastest supported
   rate the logic rate divides (via `XR_FB_display_refresh_rate`); the rates list
   arrives only after the session starts, so a one-shot request spends itself on an
   empty list — retry until the list is real (lus `5d2f7f48`, lh `77422246`,
   `5dd58636`). Quest resets the rate after the launch transition; re-ask on focus
   (lus `faa33361`).
9. **Policy: no virtual controls in a headset.** The on-screen pad stays off
   (`PadActive()` false); gaze/pinch is one blind finger, useless for a two-thumb pad.
   Menu via the window's own menu button; gameplay via a paired controller
   (lh `5249ab37`).

**Phase gate:** game visible on a stable window in passthrough, correct colors verified
by byte-identical capture comparison, pinch drives the menu, a paired controller plays
the game, panel holds the granted refresh rate.

---

## Part 4 — The window camera model (stereo)

This is the core intellectual asset of the whole port. **The XR camera model is a
window** — a magic mirror / diorama. The game world sits behind a rectangular portal
anchored in the room. Head motion changes *only the projection* (a generalized
off-axis / Kooima frustum aimed at the portal corners). Never move or rotate the game
camera from the head pose: these games' cameras are fixed and game-driven; head-look
would fight them. Do not revisit this decision.

### 4.1 Mechanism

- Per-eye rendering: the frame loop asks the window backend for a view count and runs
  the display list once per view; flat backends answer 1 (lus `c04f8a7a`,
  lh `dcf1e1e4`). The sub-frame budget must measure both eyes so it drops a sub-frame
  before it drops an eye.
- `Interpreter::ApplyXrProjection` replaces a perspective projection **at load time**
  with an off-axis frustum from the eye to the window rectangle, reading near/far and
  the half-angle tangents back off the game's own matrix. Bit-exact at the nominal head
  position; `guOrtho` and offscreen passes untouched — which automatically pins HUD and
  menu to the glass (lus `c04f8a7a`).
- **Flat-projection bracket**: HUD elements that are real 3D models parked in front of
  the camera (signs, speech balloons, transition pieces) need a display-list command
  (`gSPXrFlatProjection`) pinning them to the window plane. The mark must travel *in the
  display list* because projection substitution happens on another thread
  (lus `fac29dea`, lh `716e81bd`, `9fd6a77a`).
- **Depth measurement**: each triangle reports its nearest visible depth — clipped
  against the four screen sides and the near plane, or off-screen corners of backdrop
  polygons pin the minimum at the clip plane. The glass moves to the nearest drawn
  thing: snaps in immediately, eases out over ~2 s (lus `e31caad3`). Give display lists
  an opt-out command (`gSPXrSceneDepth`) so particles do not yank the glass
  (lus `353dcab7`, lh `e06ebf32`); reset it at the top of every list so an abandoned
  list cannot leave it off.
- **The comfort model (Diorama Depth)**: eye strain came not from wrong math but from
  the *span* — glass at 0.5 m against ~1.3 m optical focus with world past 4 m. The
  settled model: eye offsets convert across the glass with gain
  `depth / (range + depth)`, which is `< 1` for every setting — **divergence is
  impossible by construction**, lean included. The farthest content sits a fixed
  "diorama depth" (meters) behind the glass; defaults keep the world within ~0.5
  diopter of the focal plane (lus `817dfbe1`, lh `823256dc`). This guarantee was broken
  silently once by a refactor (a gain that could exceed 1); guard it. All future depth
  tuning goes through this one gain.
- Stereo of screen-space captures: any `gDPCopyFB` snapshot (pause background, screen
  transitions) runs once per eye and the second pass overwrites the first — a flat
  photo in a deep world. Register a per-eye framebuffer pair and route the second eye's
  copy and bind to it (lus `76b92814`, lh `9fd6a77a`, `4741c3b9`).
- Inherited RDP state across eyes/frames: a model that "draws black on XR but fine
  before" is usually multiplying by a PRIM (or similar) that some other draw used to
  set every frame. Pin the state where the model draws (lh `9fd6a77a`).
- Edge treatment: at a side edge each eye sees a sliver the other cannot; no mask fixes
  that. Each eye gives up its own side edge by a fraction of the eye separation —
  crossed parallax reads as occlusion behind a near edge (lus `1484e572`).
- Window manipulation: one function (`PlaceWindow`) makes the pose from four numbers
  (direction, range, size, anchor). Resize moves the frustum apex, not the window.
  Range and size must be independent settings or one slider does nothing
  (lus `210abf8a`, `2d4083e7`). Hand manipulation and menu sliders set the same state:
  push a setting only when the menu moved it, else read the window back — last mover
  wins (lh `ae71cd5c`).

### 4.2 Verification style

Every claim above carries an on-device measurement in its commit body: parallax vs IPD
(43 mm vs 64 mm), glass overshoot before/after (77% → 0.32% of frames), passthrough
color diffs, recenter gesture angle thresholds. Reproduce that style: **measure, fix,
re-measure, and put the numbers in the commit message.** The emulator reports both eyes
at one pose — prove any stereo question on hardware, and md5 the eye pair before reading
any stereo measurement.

---

## Part 5 — Second OpenXR runtime (Meta Quest)

Porting to a second OpenXR runtime is cheap if the first backend was honest OpenXR. What
actually differed (lus `faa33361`, lh `e862fc8e`):

- **Manifest**: one manifest serves both runtimes — neither reads the other's entries.
  Quest blocks launch unless the app declares hand-tracking or controllers; the VR
  intent category replaces the full-space property; the `uses-native-library` rule
  applies to its runtime library too at targetSdk 35.
- **Controllers**: Horizon routes Touch controllers through OpenXR only — invisible to
  SDL. Build a full OpenXR action set and merge into the game pad. All bindings for one
  interaction profile must go in **one** suggestion call; a second call replaces the
  first.
- **Refresh**: Quest offers 120 Hz and resets the rate after launch; re-ask on focus and
  on unexpected change.
- **Menu legibility must be angular.** DPI-based scaling held on one headset and failed
  on the other (letters at a third of the angle on a 4128 px panel). Scale =
  panel-width / window-angle; desktop text reads at ~0.4° per capital letter, headsets
  need about twice that. Re-apply when the window resizes, and note the extractor's
  UI path never runs the per-frame scale hook unless told (lh `7c525fb3`, `87beb567`,
  `b796ac26`).
- **Render only the pixels the window can show.** The runtime hands a full-panel
  swapchain (4128 px) for a window covering ~1090 px: report the eye target as the
  window size, make presentation a 1:1 nearest copy, and let "Internal Resolution"
  be a true supersample. This also removes two non-integer bilinear resamples per eye
  that differ per eye and stop thin far objects fusing (lus `975ee14f`, `b36b97e2`,
  lh `e678f5b6`).
- Cosmetics the system reads: the *activity* label (not the application's) is what the
  system shows over the exit menu (lh `491c709f`).

---

## Part 6 — Performance (the XR cost structure)

**The measured cost model on a mobile headset: the frame is CPU-bound on the display
list walk, not GPU-bound on pixels.** Resolution sweep: ~9 ms fixed + ~4 ms/megapixel
per sub-frame — lowering internal resolution is nearly useless; cut list work or
sub-frames. The list runs once per eye × N sub-frames per game tick, so anything that
densifies the list costs 2N× per tick. Particles are the densest content.

Order of operations (each step's tool found the next step's problem):

1. **Build the measurement first.** A draw-call counter at the single flush funnel
   (lus `3d4638cd`); delivered-sub-frame reporting — the tick-rate report showed the
   *asked* count and hid a collapse from 120 to 45–65 sub-frames/s (lh `724e440c`,
   `5c88d0b4`); per-cause flush counters with a distinct-texture floor over a marked
   pass (lus `487e828b`); worst-sub-frame (not average) reporting — the average hid the
   burst (lh `85565f90`). All behind a debug-tools build flag, all stripped or silenced
   once the fault is fixed.
2. **Sub-frame pacing on delivered work** (lh `3d2cc35c`, `3c052baa`): cap the sub-frame
   count at what the last tick actually delivered, not what CVars ask. Pace on the
   delivered *count*, never on draw time — the present wait dominates a headset
   sub-frame and pacing on time chases its own tail. Require two consecutive short
   ticks before lowering; probe upward every ~30 ticks; bound at target+1; clamp a
   hitch's cost sample to the pass budget; measure against what the tick *asked* so
   cutscene VI-rate changes do not strand the learned count. Platform-neutral: this is
   an upstreamable superproject change.
3. **Texture-binding early-out** (lus `90ef840c`): N64 lists re-load the bound texture
   per object; the interpreter flushed the batch each time for zero work. Compare cache
   keys before flushing; only in the plain case (not masked/blended/framebuffer).
   Measured 1.8× fewer draws generally — and **no effect on particle bursts** (902→890),
   which is what forced step 4. Watch the interaction: skipping rebinds means the
   sampler-state setup must not be skipped for framebuffer textures that have no cache
   node (lus `e8515c3a`).
4. **Diagnose the burst, don't guess** (lus `487e828b`, plan doc): flush-cause counters
   said texture caused 41 886 of ~42 000 batch breaks, and the binding *alternated every
   draw* between a 31×32 and an 8×8 of the same sprite. **Sprite frames larger than one
   TMEM load are baked as one load + one triangle pair per chunk** — chunk alternation
   inside every particle defeats the early-out and any game-side draw ordering.
5. **`gSPTextureBatch` bracket** (lus `96d7c2c9`): inside a bracketed span, draws whose
   only state change is a plain, already-cached slot-0 rebind get parked in per-texture
   buckets, drained one draw per texture at the top of every flush and at bracket end.
   Cache hits only (the LRU must not evict a bucketed node); drain capped at the vertex
   buffer size. Game side: two lines per particle pass emit the bracket
   (lh `16e8ed55`). Measured on the worst burst (675 emitters, 13 500 particles):
   1668 draws → 4; worst sub-frame 96 ms → 7 ms; display holds 120.
6. Game-side emitter grouping (fork-only): emit runs of like emitters in sprite-frame
   order; fuzz the regrouping over 20 000 randomized emitter arrays asserting exact
   emission and balanced brackets (lh `85565f90`).

Every one of these was verified on device with before/after numbers in the commit body,
plus a scripted replay recipe for the worst-case scene (a file-triggered debug hook that
re-runs the burst on demand — temporary, never committed).

---

## Part 7 — visionOS

**Goal:** the game in a volumetric window in the Shared Space, in stereo, beside the other
apps, with no SDL video at all.

**Read this first.** This port was built twice. The first shell held a Compositor Services
immersive space and placed the screen itself. It worked, and it was the wrong shape: the
game is a window with depth, so the system must own move, resize and close. The second
shell is a volumetric `WindowGroup` holding a `RealityView`, and it is what ships. The
engine seam took that move with no change of contract, which is the one thing to copy from
this history: keep the shell behind a `void*` seam and either shell can drive it.

### 7.1 Architecture

- A native SwiftUI + ObjC++ shell owns a volumetric `WindowGroup`
  (`.windowStyle(.volumetric)`, `.defaultSize(..., in: .meters)`,
  `.volumeWorldAlignment(.gravityAligned)`) with one quad in a `RealityView`. SDL is built
  with `SDL_VIDEO=OFF`; the window backend derives directly from the abstract window
  backend, not from the SDL one (lus `1d7e790c`). `HandleEvents` still pumps SDL for
  controller add and remove.
- The engine renders **into an external Metal target**: `MetalInitExternal(device, queue,
  texture)` — no drawable, framebuffer zero points at the caller's texture, never presents.
  `nextDrawable` was also the implicit CPU/GPU pacing, so external mode must wait three deep
  on frame retirement (lus `53704500`). Everything runs on the shell's queue: command-buffer
  creation order is execution order, and that order is the only synchronization needed
  (lh `a8cfc534`).
- The picture reaches RealityKit through a `LowLevelTexture` on the quad's material.
  **Do not point the external target at the `LowLevelTexture`.** Fast3D spreads framebuffer
  zero over several command buffers and commits it last, which does not fit the one command
  buffer `replace(using:)` gives. Keep private Metal textures as the render targets, and
  blit into the `LowLevelTexture` in one command buffer when the frame closes
  (lh `87e1e0ff`). Double-buffer those textures so a copy never meets a draw
  (lus `ae58dfda`). `replace(using:)` is happy in the `SceneEvents.Update` handler.
- The `LowLevelTexture` is `bgra8Unorm_srgb`, so the sampler decodes once and Metal copies
  between it and a plain `bgra8Unorm` game texture. The immersive shell needed a decode in
  the screen shader instead (lh `98c42fdf`); a volume needs none.
- **ARKit answers nothing in the Shared Space.** `queryDeviceAnchor` returns failure and an
  identity pose while only a volume is open. An **empty mixed `ImmersiveSpace` open beside
  the volume** brings the head back. It draws nothing, and both the other apps and the
  volume's own window bar stay where they are (lh `2ca1f72e`). Two other routes stay dead:
  `AnchorEntity(.head)` never anchors, and `AnchorEntity(world:)` does anchor but reports a
  frame that is neither the quad's nor ARKit's. The route to the quad is
  `Entity.transformMatrix(relativeTo: .immersiveSpace)`.
- **Only one app on the device may hold an immersive space.** Hold it while the volume is in
  use and give it back at once (lh `9f20b37c`). An app that keeps it for the life of the
  process wedges the whole headset: nothing else can open content, the app cannot be torn
  down, and `devicectl device process launch` then hangs after the first launch. That last
  symptom reads as a tooling fault and is not one.
- The user can close that space with the Digital Crown while the volume keeps drawing.
  Watch the query status and fall back to a fixed head in front of the window. Leaving the
  eyes unreported is worse than a wrong head, because the camera model gives up without them
  and **that takes the stereo with it** (lh `0a82326c`).
- Shell and engine talk through `void*` seams so neither needs the other's headers.

Bring-up traps, all found under the first shell and all still true:

- **`SDL_SetMainReady` must be called by hand** — normally `SDL_UIKitRunApp` does it;
  without it `SDL_Init` refuses every subsystem and the symptom is "no controllers"
  (lh `9c19cbb8`).
- ImGui needs explicit bring-up (context, DisplaySize = game texture, scale 1) because only
  the SDL/DX11 paths did it before; the same silent size-mismatch drop from the iOS
  black-screen bug applies (lus `62b30c31`).
- Lifecycle: no SDL app events exist. `scenePhase` carries the same news, and it must be
  read even while the app is off screen, when no frame is opened at all. Route it into the
  same off-screen park iOS built (lus `ff93a054`, lh `57357774`).
- Keyboard: SDL's keyboard lives in the UIKit video driver, which is off. The GameController
  framework reports keys; its keycode is the HID usage, which *is* an SDL scancode. Trap:
  `GCDevice` handlers default to the main queue and can queue forever — give the keyboard
  its own serial handler queue (lus `e86c05b5`, lh `df18850d`).
- Nothing else empties the SDL event queue. The control deck polls the pad rather than
  taking its events, so every axis motion of a session stays in the queue and the two peeks
  the device handler makes each frame walk all of it, under the event lock. The desktop
  window backend has always dropped these; a backend with no window must too
  (lus `ad37113d`, lh `32e7a8ed`).
- Only the app's stderr reaches a `--console` session. Anything to read after the fact goes
  through spdlog to a file in the app container (lh `2ee15cb4`).

### 7.2 Input in a volume

The immersive shell used Compositor Services **tracking areas**: the app supplied rectangles
and the system drew the gaze highlight out of process and named the hit area in the event.
A volume has no such API. Do not look for one.

- Input is one RealityKit gesture on the quad:
  `DragGesture(minimumDistance: 0).targetedToEntity(quad)`, not `SpatialTapGesture`, because
  one gesture must carry tap, drag and release for the sliders. The quad needs an
  `InputTargetComponent` and a `CollisionComponent`. Quad-local x and y become a UV, then
  game texture pixels, then the existing pointer queue (lh `d4f9c668`).
- Keep a two-frame position-then-press step, with one rule: a press **that arrives at a new
  place** waits one frame, and a press already held moves at once. ImGui takes the item it
  hovers from the position it held at `NewFrame`, so a press delivered with its own first
  position lands on whatever was under the position before it. The naive version, which
  waits whenever the place moves, means a slider never sees the button go down
  (lus `36c8eec5`).
- **The gaze highlight is not lost, but it is rebuilt.** visionOS never tells an app where
  the wearer looks, so only the system can draw one. Feed ImGui's own item rectangles out
  through the test-engine `ItemAdd` hook (lus `41eb9160`), publish the finished set for
  another thread (lus `dc32901d`), and put one SwiftUI plate per rectangle over the picture
  (lh `cc23dec8`). Three things had to be measured, because nothing states them:
  - A volume puts a flat view at its **front face** and clips whatever stands in front of
    that. The picture hangs in the middle, so the plate has to be carried back to it.
  - **A plate at zero opacity is not hit-testable**, so it can never become active. A clear
    plate, and a plate that waits at zero for the gaze, both stay dark for ever. Use a black
    anchor at 0.02 alpha, which is twice the hit-test floor and adds no luminance, and a
    white flash that waits at zero, both in one `hoverEffectGroup()`.
  - The layers composite in **linear light**. A white trace of two percent on each window
    took the menu background from 10/255 to 95/255.
- **Order the rectangles the way ImGui hovers**, don't patch symptom by symptom: windows
  back-to-front, each first blanking its own rectangle; within a window, largest item first
  so the smallest wins a point. Order after the frame ends, when window order has settled
  (lus `4d5fb1fc`). Two special cases: near-display-size items are windows (lus `8e813525`);
  `EndChild` reports the whole child as one item *after* its rows and must be skipped, or
  every list press hits the same row (lus `cd543b3d`).
- A menu button drawn via the foreground draw list adds no ImGui item, so it gets no
  rectangle — give it an invisible button over the same place. If a press names something
  and nothing happens, look at **window order** first (lh `f607bfcf`).
- **The system keeps most of the game controller in the Shared Space, and this is not
  obvious.** `GCSupportsControllerUserInteraction` in the plist is needed and is not enough
  (lh `76970d3f`): a volumetric window still takes the thumbsticks for scrolling and the
  face buttons, the shoulders and the triggers for itself. What is left is the D pad, the
  two stick clicks, Menu and Options. Neither the window style nor the way the app reads the
  pad changes it. `.handlesGameControllerEvents(matching: .gamepad)` on the root view is
  what asks for the rest (lh `b04a68d1`). The shape of the fault misleads: the first Start
  press lands, everything after seems to fall away, and the sticks are worst, which reads as
  a leak or a queue filling up. It is a fixed list of buttons that never worked. An
  immersive space has no interface of its own to drive, which is why the first shell never
  showed it.

### 7.3 Stereo, cadence and window geometry

- The window camera model is backend-independent by design: widen the engine's "headset
  window" guards from the OpenXR define to a shared `ENABLE_XR_WINDOW`, then audit **every**
  caller — some answers flip per platform (lus `a6514d14`, lh `4d2f164f`). Keep "is a headset
  window" (rendering model) distinct from "headset controls active" (touchscreen and menu
  policy): visionOS answers yes to the first and no to the second.
- Per-eye textures: the external target moves between passes (choose the eye in the per-view
  hook); one shell frame spans both eyes. The display list runs once per eye, so **any
  per-frame state stepped inside it now steps twice** — step pointers and similar state on
  the first eye only (lus `b425713a`, `9bb6f2b2`).
- **RealityKit's only public per-eye path is a `ShaderGraphMaterial` with a Camera Index
  Switch node.** There is no per-eye entity visibility and no camera transform. Widen the
  texture to two eyes side by side and switch on `UV.x` (lh `0a8887f3`). It works inside a
  volume, which the documentation only ever discusses for immersive content. RealityKit
  loads a raw `.usda` copied into the bundle, with no `realitytool` step, so the build system
  needs one `target_sources` line with `MACOSX_PACKAGE_LOCATION Resources`. Two names are not
  guessable and are in the Xcode SDK, under
  `USDLib_FormatLoaderProxy_Xcode.framework/.../libraries/realitykit/`: the node is
  `ND_realitykit_geometry_switch_cameraindex_vector2` with ports `mono`, `left` and `right`,
  and `ND_RealityKitTexture2D` **flips the vertical coordinate** unless `no_flip_v` is `1`.
  Read them; do not guess. `mono` is the default output, which is what the simulator takes.
- Apple keeps the user's IPD private and there are no per-eye transforms, so make the eyes
  head plus and minus a nominal 63 mm along the device x axis. The diorama gain compresses
  disparity, so a few millimeters of error is a few percent of depth scale.
- **The volume owns its size, so the shell tells the backend** with a
  `SetVisionOSWindow(halfWidth, halfHeight, range)` and the getter becomes a read-back
  (lus `becaf36c`, `efe64716`). Letterbox the quad to the shape of the picture, which only
  the backend knows, and remake the quad when it moves (lh `11cb0010`). Report **zero**, not
  a fallback, before the game has a picture to measure: a shell that letterboxes to a
  self-derived 1.0 draws a square (lus `ef7fac32`, lh `9796d1f6`). The camera model needs no
  change for any of this, because it works in game units and never sees meters.
- **The range must be a latched reference, never the live head distance.** The eye offset
  reads `eyeZ - range`; if the range follows the head that term is always zero and all dolly
  parallax dies, which is most of what makes a diorama read (lus `983656b7`).
- Move, resize, placement and recenter all leave the app. The system window bar does them,
  so the matching menu sliders go under the OpenXR guard and only the depth control stays
  (lh `cbee98da`).
- Cadence: nothing on visionOS reports the panel rate and nothing can ask for one. Measure it
  from the frame times — the shortest gap over a window of about 120 frames — and follow it
  (lh `3d3998b3`, `ae6e2d92`, lus `f99b0987`). `SceneEvents.Update` is a true 90 Hz clock on
  the device. It is **not** one in the simulator, which reports 120 to 190 Hz.
- **A counting semaphore between shell and game needs back pressure.** The volume signalled
  once per update and the game took one signal per frame, so a game that fell behind left a
  signal behind every frame, the count grew without bound and the game never waited. The tick
  stretched to 20 Hz, which made the pacing ask for four sub-frames instead of three, which
  kept it stretched. Drain the queue each frame (lh `cccae954`). The measurement that finds
  this is two lines a second, one from the shell and one from the engine, because **30 is the
  logic tick rate**: a sub-frame count that collapses to one and a game that cannot draw look
  identical from outside.

Known open items in the reference implementation, so you do not re-diagnose them as new: the
first menu frame hitches on hardware; and the host-side `ExtractAssets` target writes an
archive the app rejects (`version` vs `portVersion` key mismatch — the in-app extractor
writes the right key).

---

## Part 8 — Cross-cutting engineering doctrine

These patterns repeated across every phase; treat them as rules.

1. **Silent failure is the default enemy.** Nil drawables via `objc_msgSend`, whole GUI
   frames dropped on a size mismatch, GLES errors that keep stale state, log calls
   that never reach any log. Whenever a fix closes a silent path, add a one-shot
   diagnostic at that exact point.
2. **Measure before fixing; re-measure after; numbers go in the commit message.** The
   commit body is the permanent engineering record — mechanism, measurement, and the
   debugging story. Comments in code stay at zero (superproject) or one line
   (engine); maintainers reject blocky comments.
3. **Build the measurement tool before the optimization.** Draw counters, flush-cause
   counters, delivered-frame reports, eye captures. Ship them dark behind a
   debug-tools flag; strip the ones whose fault is fixed.
4. **Automate the device loop as each input channel disappears.** The port removed
   human input channels one by one, and each removal got a host-driven substitute:
   scripted pad state from a file (no touchscreen in headset) → framebuffer capture to
   files (screencap is black in headsets) → host-driven pointer file (no pinch
   automation) → scripted taps via environment variable (no gaze automation). Build
   these *early*; they are what lets an agent test without a human wearing the device.
5. **Prove compilation on desktop, correctness on device.** The Linux desktop build
   (plus a GLES twin) catches compile breaks in minutes. It proves nothing about Mali,
   Bionic, drawable pools, or stereo.
6. **One new variable at a time on new hardware.** The eight-bug iOS chain and the
   five-fault visionOS pointer chain were each found by fixing one thing, re-running,
   and reading the next symptom. Resist batching speculative fixes.
7. **Old settings keys are live landmines.** A config file on a device keeps keys of
   removed settings; a renamed setting whose old key scales the other way must get a
   new key, never a reinterpretation.
8. **Check every "already settled" decision against its doc before reopening it.**
   Startup speed, the camera model, no-virtual-controls-in-XR — each has a written
   rationale. Re-litigating them burns sessions.

---

## Part 9 — Upstreaming

The port only counts when it lands upstream. The process that worked:

1. **Split from the start** (see 0.1): when planning any engine change, ask "does a
   platform that is not mine change?" — the review that question comes from re-cut
   three fixes (a texture-uniform fix that touched every GL platform became a count;
   an `__aarch64__` guard became `__ANDROID__ && __aarch64__`; a global `-fsigned-char`
   became Android-only).
2. **Target the right upstream branch.** The engine upstream integrates on a
   maintenance branch, not `main` (`main` had moved to SDL3 and renames the touched
   files). The superproject's active integration branch is not always `develop` —
   check where recent PRs merge.
3. **Cut PRs by dependency wave**: platform-independent fixes first (they need no new
   plumbing and build goodwill); then the platform build+render PR; then audio stacked
   on it; then the superproject platform PR (with the submodule bumped to an upstream
   commit containing its prerequisites); then follow-ups. A PR opened before its wave
   either cannot build or shows unrelated commits.
4. **PR hygiene**: both upstreams squash-merge, so the PR title is the permanent commit
   subject — write it as one. Bodies are honest about verification gaps ("this path
   has not been driven on device"). Strip AI trailers from upstream-bound commits and
   disclose AI assistance in the PR body instead; verify with
   `git log --format='%b' <base>..HEAD | grep -i co-authored`. No added comments in
   superproject PRs; one line max in engine PRs. Re-run the formatter after **every**
   rebase, not only after edits.
5. **Socialize firsts.** The first PR that says a new platform's name in its title goes
   to the maintainers' Discord before it opens.
6. Keep a single untracked planning doc (`UPSTREAM_SUBMISSION.md` style) holding the
   wave table, exact titles, full bodies, per-branch status, and device evidence.

---

## Part 10 — Test and debug infrastructure summary

| Target | Build proof | Run/see | Input automation | Hard limits |
| --- | --- | --- | --- | --- |
| Linux desktop | full build + GLES twin | run locally | keyboard scripts | Mesa forgives GLES violations |
| iOS device | SSH build to a Mac (compile+link only; codesign always fails headless — a human presses Run) | `devicectl` launch with `--console`; app stderr only | none — instrument the app | no system log, no screenshot, locked device refuses launch, the console session owns the process |
| Android phone | Gradle on Linux | adb install/launch/screencap; spdlog goes to a file in app storage, not logcat | `input keyevent`; file-driven debug pad | emulator renders via host Mesa |
| Android XR / Quest | same APK | per-eye raw captures via file-request hook (screencap is black); logcat with a huge ring buffer captured *before* the repro | debug-pad file; pointer file; `prox_close` fakes head presence for desk testing; emulator head via gRPC | headset presents nothing off-head; stereo only provable on device |
| visionOS | Mac build, simulator first | simulator screenshots work and show the room, so placement is checkable with no human; hardware via spdlog to the app container | mouse click in the simulator is a press and the mouse is the gaze; `simctl` ROM injection | simulator reports one view, so stereo needs hardware, and it is no display clock; posting mouse events over SSH needs Accessibility trust; Metal shader validation crashes the simulator compiler service |

Universal rules: never pipe an Xcode build through `tail`/`head` (the pipe status hides
`BUILD FAILED` — redirect to a file); kill local GUI runs with SIGKILL or self-exit
(SIGTERM triggers the crash-report modal); verify the binary you are testing is the
binary you built (`strings`/md5 against the installed artifact — shared build
directories and Gradle's build cache both served stale native builds during this port).

---

## Part 11 — Orchestrating this port with agents

This port took roughly 220 commits across two repositories. The structure that lets an
agent fleet do it with little handholding:

### 11.1 Roles

- **One orchestrator** holds the plan, the git history, all commit messages, and final
  judgment. It never delegates the history, the commit messages, or the decision of
  what is upstreamable.
- **Reader agents** fan out on anything spanning more than a few files (upstream diff
  archaeology, symbol searches, doc distillation). They return conclusions plus
  file:line, never dumps. **Verify every agent claim against the actual diff** — agent
  reports contain wrong claims.
- **Builder agents** run build/test loops (desktop build, GLES twin, format checks) and
  report pass/fail with the failing output.
- The orchestrator maintains two living untracked documents from day one: a debug-notes
  file (bug chain, environment limits, open items) and an upstream-submission file
  (wave table, PR bodies, status). Update them after every meaningful result — they are
  the hand-off state between sessions.

### 11.2 Phase gates and human touchpoints

Work strictly in the phase order of this playbook; each phase gate is a device
verification. The human is needed only at enumerable points — list them in the plan so
they can be batched:

1. Pressing Run in a GUI IDE when headless signing fails (every Apple install).
2. Wearing the headset for visual judgments (stereo fusion, comfort, legibility) and
   for anything the off-head/simulator route cannot show.
3. Plugging in / waking devices; unlocking them.
4. Store uploads (TestFlight etc.) — the human runs those, always.
5. Approving the upstream PR plan before anything opens, and all Discord contact.

Everything else — builds, installs, log capture, captures, scripted input, measurement
runs — should go through the automation channels of Part 10, which the agent builds for
itself as early as each phase allows.

### 11.3 The loop that works

For each phase: read this playbook's section and the referenced commits → write the
phase plan into the untracked doc, including the upstream/fork split → implement the
smallest slice that can show a frame or a number → prove compile on desktop → verify on
device via the automation channel → record the measurement in the commit body → update
the doc → next slice. On any anomaly: suspect a silent failure, add the one-shot
diagnostic, and fix one variable at a time.

---

## Part 12 — Kickoff prompt for a new port

Paste the following to start the same port on another Harbour Masters decompilation,
after filling the bracketed fields:

> Port [GAME/REPO] from desktop to iOS, then Android, then XR (OpenXR headsets, then
> visionOS), following `docs/PORTING_PLAYBOOK.md` from the Lighthouse repository —
> read that file completely first, then read the commits it references for any phase
> before working that phase.
>
> Environment: [host machine + repo path; Mac SSH host for Apple builds; device list
> with how each is reached; upstream repos and their integration branches; my signing
> team/bundle prefix location].
>
> Rules: work the playbook's phases in order, one branch per platform stage
> (`ios-support` → `android-support` → `xr-integration`), with the engine submodule
> forked and pinned in lockstep. Decide the upstream/fork split when you plan each
> change, not after. Keep two untracked planning docs updated after every result:
> debug notes (bug chain, environment limits, open items) and upstream submission
> (wave table, PR titles and bodies, status). Commit style: mechanism, measurement,
> and debugging story in the commit body; near-zero code comments; Simplified
> Technical English; no AI trailers on upstream-bound commits — disclose in PR
> bodies. Build the device-automation channels (scripted pad, framebuffer capture,
> host pointer, scripted taps) as early as each phase allows, and verify every claim
> on device with numbers in the commit message.
>
> Delegate reading and build loops to subagents; keep history, commit messages, and
> upstream judgment yourself; verify subagent claims against the diff.
>
> Ask me only at these points: GUI signing runs, headset-on visual checks, device
> unlock/plug-in, store uploads, and approval of the upstream PR plan. Batch device
> requests. Between those points, proceed.
>
> Start now: confirm the desktop build, then open the iOS phase per the playbook,
> and put the phase plan in the debug-notes doc before the first change.

The expensive lessons this prompt encodes: the phase order exists because each phase
reuses the previous one's plumbing; the planning docs are the only durable state
between sessions; the automation channels are what make "without handholding"
possible; and the commit bodies are the deliverable a future porter actually reads.

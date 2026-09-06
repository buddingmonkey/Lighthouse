# Agent instructions

Lighthouse is the Harbour Masters port of Banjo-Kazooie. This branch adds mobile
(iOS, Android) and XR (Android XR, Meta Quest, visionOS) targets on top of the
upstream port. The engine layer is the `libultraship` submodule; a change to
rendering, audio, input, or the window belongs there. A change to the game, the menu,
or the on-screen controls belongs in `src/port/`. Do not reformat the decompiled game
code under `src/`.

**Read `docs/PORTING_PLAYBOOK.md` before you work on any mobile or XR area.** It
records the full desktop → mobile → XR porting history: the bug chains with root
causes, the build traps, the XR window camera model, the performance work, the
upstreaming process, and the per-device test routes. The commit messages it
references are the primary record — read them before you re-investigate anything.

Decisions that are settled there; do not reopen them without new evidence:

- The XR camera model is a window (magic mirror / diorama). Head motion changes only
  the projection. Never move the game camera from the head pose.
- No on-screen touch controls in any XR target.
- iOS startup is not slow; it was measured.
- The stereo divergence bound (gain `depth / (range + depth)` < 1) must hold for
  every setting.

Working conventions:

- Commit messages carry the mechanism, the measurement, and the debugging story.
  Code comments: none in this repository, one line at most in `libultraship`.
- Write commit messages, PR titles, and PR bodies in ASD-STE100 Simplified Technical
  English, American spelling.
- No AI co-author trailers on upstream-bound commits; disclose AI assistance in the
  PR body instead.
- `src/port` is formatted with clang-format-14 (`./run-clang-format.sh`); re-run it
  after every rebase.
- Prove that a change compiles with the desktop build; prove that it is correct on
  the device it targets.

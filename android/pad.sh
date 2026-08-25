#!/usr/bin/env bash
# Drive the running Android build's controller from the host.
#
#   pad.sh A ms=150              tap A for 150 ms
#   pad.sh stick=0,60            hold the stick forward until told otherwise
#   pad.sh CLEFT ms=400          rotate the camera left for 400 ms
#   pad.sh cstick=60,0 ms=400    same through the right stick
#   pad.sh                       release everything
#
# Buttons: A B Z START L R CUP CDOWN CLEFT CRIGHT DUP DDOWN DLEFT DRIGHT
# Axes run -80..80. Without ms= the state is held until the next call.
set -euo pipefail

ADB="${ADB:-$HOME/Android/platform-tools/adb}"
PKG="${PKG:-com.harbormasters.lighthouse}"
FILE="/sdcard/Android/data/${PKG}/files/debug-pad"

"$ADB" shell "printf '%s' '$*' > $FILE"

#!/usr/bin/env bash
# Put the running Android build's pointer on its window from the host. Needs -PdebugTools=ON.
#
#   pointer.sh 0.5,0.55 click     hover, press and release at the middle of the picture
#   pointer.sh 0.5,0.55           hover there until told otherwise
#   pointer.sh 0.5,0.55 down      hold the button down there
#   pointer.sh                    lift the pointer off
#
# The coordinates are the picture's own, 0,0 at the top left and 1,1 at the bottom right.
# capture.sh writes capture-panel.png in the same coordinates, which is how to aim.
set -euo pipefail

ADB="${ADB:-$HOME/Android/platform-tools/adb}"
PKG="${PKG:-com.harbormasters.lighthouse}"
FILE="/sdcard/Android/data/${PKG}/files/debug-pointer"

put() { "$ADB" shell "printf '%s' '$1' > $FILE"; }

AT="${1:-}"
ACTION="${2:-}"

if [ -z "$AT" ]; then
    put ""
    exit 0
fi

if [ "$ACTION" = "click" ]; then
    # A press needs a frame of hover before it, the same as a hand arriving on the window does,
    # and the release has to land in the same place for the widget under it to fire.
    put "$AT"
    sleep 0.3
    put "$AT down"
    sleep 0.3
    put ""
    exit 0
fi

put "$AT ${ACTION}"

#!/usr/bin/env bash
# Ask the running Android build for its frames and convert them to PNG.
#   capture.sh [outdir]        -> outdir/capture-<label>.png for every label the app wrote
set -euo pipefail

ADB="${ADB:-$HOME/Android/platform-tools/adb}"
PKG="${PKG:-com.harbormasters.lighthouse}"
FILES="/sdcard/Android/data/${PKG}/files"
OUT="${1:-$(dirname "$0")/../build-android/captures}"

mkdir -p "$OUT"
"$ADB" shell "rm -f $FILES/capture-*.raw; touch $FILES/capture-request"

for _ in $(seq 30); do
    "$ADB" shell "test ! -e $FILES/capture-request" 2>/dev/null && break
    "$ADB" shell sleep 0.2 >/dev/null 2>&1 || true
done

if "$ADB" shell "test -e $FILES/capture-request" 2>/dev/null; then
    echo "The app did not answer the capture request. Is it running and rendering?" >&2
    exit 1
fi

mapfile -t RAWS < <("$ADB" shell "ls $FILES/capture-*.raw 2>/dev/null" | tr -d '\r')
if [ "${#RAWS[@]}" -eq 0 ]; then
    echo "The request was consumed but no capture-*.raw was written." >&2
    exit 1
fi

for raw in "${RAWS[@]}"; do
    base="$(basename "$raw" .raw)"
    "$ADB" pull "$raw" "$OUT/$base.raw" >/dev/null
    python3 - "$OUT/$base.raw" "$OUT/$base.png" <<'PY'
import struct, sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
width, height = struct.unpack('<II', data[:8])
image = Image.frombytes('RGBA', (width, height), data[8:8 + width * height * 4])
image.transpose(Image.FLIP_TOP_BOTTOM).convert('RGB').save(dst)
print(f'{dst}  {width}x{height}')
PY
    rm -f "$OUT/$base.raw"
done

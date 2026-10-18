#!/usr/bin/env bash
set -euo pipefail

# Runs the native analysis driver in full (0) and no-pixels (1) mode on the
# pinned fixtures and compares the record dumps byte-for-byte.
#
# Usage: run-native-analysis.sh [fixture ...]   (defaults to the three codecs)
# Fixtures come from VOIDPLAYER_WEB/fixtures/video (override FIXTURES_DIR).

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WEB="${VOIDPLAYER_WEB:-$HOME/Documents/yorune/VoidPlayer-Web}"
FIXTURES="${FIXTURES_DIR:-$WEB/fixtures/video}"
OUT="${RESULT_DIR:-$ROOT/.build-native-analysis/results}"
DRIVER="$ROOT/dist-native-analysis/vpa_native"

FILES=("$@")
if [ "${#FILES[@]}" -eq 0 ]; then
    FILES=(h264_9s_1920x1080.mp4 mhw_hevc_fullrange_bt709_3s.mp4 h266_10s_1920x1080.mp4)
fi

[ -x "$DRIVER" ] || { echo "missing $DRIVER; run build-native-analysis.sh first" >&2; exit 1; }
mkdir -p "$OUT"
failed=0

for name in "${FILES[@]}"; do
    file="$FIXTURES/$name"
    [ -f "$file" ] || { echo "missing fixture $file" >&2; exit 1; }
    full_json=$("$DRIVER" 0 "$file" "$OUT/$name.mode0.bin") || { echo "$name: full mode failed"; failed=1; continue; }
    fast_json=$("$DRIVER" 1 "$file" "$OUT/$name.mode1.bin") || { echo "$name: no-pixels mode failed"; failed=1; continue; }
    if cmp -s "$OUT/$name.mode0.bin" "$OUT/$name.mode1.bin"; then
        equal=yes
    else
        equal=NO
        failed=1
    fi
    echo "== $name (records equal: $equal)"
    echo "   full:      $full_json"
    echo "   no-pixels: $fast_json"
done

exit $failed

#!/usr/bin/env bash
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
install_dir=${FFMPEG_LIVE_INSTALL_DIR:?set FFMPEG_LIVE_INSTALL_DIR}
export PKG_CONFIG_PATH="$install_dir/lib/pkgconfig"
suffix=
sanitize=()
if [[ -n ${FFMPEG_LIVE_SANITIZE:-} ]]; then
    sanitize=("-fsanitize=$FFMPEG_LIVE_SANITIZE" -fno-omit-frame-pointer)
fi
case "$(uname -s)" in *NT*) suffix=.exe;; esac
for probe in latency_probe async_probe; do
    gcc -std=c11 -O2 -Wall -Wextra "${sanitize[@]}" -I"$install_dir/include" \
        "$source_root/tools/live-async/$probe.c" \
        -o "$install_dir/bin/$probe$suffix" \
        $(pkg-config --libs libavformat libavcodec libavutil)
done

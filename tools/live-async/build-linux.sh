#!/usr/bin/env bash
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${FFMPEG_LIVE_BUILD_DIR:-"${XDG_CACHE_HOME:-$HOME/.cache}/ffmpeg-live/build-linux-baseline"}
install_dir=${FFMPEG_LIVE_INSTALL_DIR:-"$source_root/../install-linux-baseline"}
mkdir -p -- "$build_dir" "$install_dir"
build_dir=$(cd -- "$build_dir" && pwd)
install_dir=$(cd -- "$install_dir" && pwd)
cd -- "$build_dir"

"$source_root/configure" \
    --prefix="$install_dir" \
    --enable-shared --disable-static \
    --disable-autodetect --disable-everything \
    --disable-network --disable-hwaccels --disable-doc --disable-ffplay \
    --disable-avdevice --disable-postproc --disable-swresample --disable-swscale \
    --enable-decoder=h264,hevc \
    --enable-parser=h264,hevc \
    --enable-demuxer=h264,hevc,mov \
    --enable-protocol=file \
    --enable-muxer=framehash,null \
    --enable-encoder=rawvideo,wrapped_avframe \
    --enable-filter=null \
    --enable-debug=3 --assert-level=2

make -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
make install
export PKG_CONFIG_PATH="$install_dir/lib/pkgconfig"
gcc -std=c11 -O2 -Wall -Wextra \
    "$source_root/tools/live-async/latency_probe.c" \
    -o "$install_dir/bin/latency_probe" \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil)
export LD_LIBRARY_PATH="$install_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$install_dir/bin/ffmpeg" -hide_banner -version

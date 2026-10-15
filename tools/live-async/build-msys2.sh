#!/usr/bin/env bash
# Run from MSYS2 UCRT64, or with C:/msys64/usr/bin/bash.exe.
set -euo pipefail

export PATH="/ucrt64/bin:/usr/bin:$PATH"
export MSYSTEM=UCRT64
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${FFMPEG_LIVE_BUILD_DIR:-"$source_root/../build-msys2-baseline"}
install_dir=${FFMPEG_LIVE_INSTALL_DIR:-"$source_root/../install-msys2-baseline"}

mkdir -p -- "$build_dir" "$install_dir"
build_dir=$(cd -- "$build_dir" && pwd)
install_dir=$(cd -- "$install_dir" && pwd)
cd -- "$build_dir"

"$source_root/configure" \
    --prefix="$install_dir" \
    --target-os=mingw32 --arch=x86_64 \
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
# Word splitting here is intentional: pkg-config emits compiler arguments.
gcc -std=c11 -O2 -Wall -Wextra \
    "$source_root/tools/live-async/latency_probe.c" \
    -o "$install_dir/bin/latency_probe.exe" \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil)

export PATH="$install_dir/bin:$PATH"
"$install_dir/bin/ffmpeg.exe" -hide_banner -version

#!/usr/bin/env bash
# Run from MSYS2 UCRT64, or with C:/msys64/usr/bin/bash.exe.
set -euo pipefail

export PATH="/ucrt64/bin:/usr/bin:$PATH"
export MSYSTEM=UCRT64
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${FFMPEG_LIVE_BUILD_DIR:-"$source_root/../build-msys2-auto"}
install_dir=${FFMPEG_LIVE_INSTALL_DIR:-"$source_root/../install-msys2-auto"}

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

export FFMPEG_LIVE_INSTALL_DIR="$install_dir"
bash "$source_root/tools/live-async/compile-probes.sh"

export PATH="$install_dir/bin:$PATH"
"$install_dir/bin/ffmpeg.exe" -hide_banner -version

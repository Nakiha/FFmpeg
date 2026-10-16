#!/usr/bin/env bash
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
flavor=${FFMPEG_LIVE_SANITIZE:-auto}
build_dir=${FFMPEG_LIVE_BUILD_DIR:-"${XDG_CACHE_HOME:-$HOME/.cache}/ffmpeg-live/build-linux-$flavor"}
install_dir=${FFMPEG_LIVE_INSTALL_DIR:-"$source_root/../install-linux-$flavor"}
mkdir -p -- "$build_dir" "$install_dir"
build_dir=$(cd -- "$build_dir" && pwd)
install_dir=$(cd -- "$install_dir" && pwd)
cd -- "$build_dir"
sanitize=()
if [[ -n ${FFMPEG_LIVE_SANITIZE:-} ]]; then
    sanitize=("--extra-cflags=-fsanitize=$FFMPEG_LIVE_SANITIZE -fno-omit-frame-pointer"
              "--extra-ldflags=-fsanitize=$FFMPEG_LIVE_SANITIZE" --disable-stripping
              --disable-asm)
fi

"$source_root/configure" \
    --prefix="$install_dir" \
    --enable-shared --disable-static \
    --disable-autodetect --disable-everything \
    --disable-network --disable-hwaccels --disable-doc --disable-ffplay \
    --disable-avdevice --disable-swresample --disable-swscale \
    --enable-decoder=h264,hevc \
    --enable-parser=h264,hevc \
    --enable-demuxer=h264,hevc,mov \
    --enable-protocol=file \
    --enable-muxer=framehash,null \
    --enable-encoder=rawvideo,wrapped_avframe \
    --enable-filter=null \
    --enable-debug=3 --assert-level=2 "${sanitize[@]}"

make -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
make install
export FFMPEG_LIVE_INSTALL_DIR="$install_dir"
bash "$source_root/tools/live-async/compile-probes.sh"
export LD_LIBRARY_PATH="$install_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$install_dir/bin/ffmpeg" -hide_banner -version

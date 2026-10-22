#!/usr/bin/env bash
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:?use a fresh build name; apply experimental patch explicitly for candidate}
if [ -n "${FFMPEG_TEST_SANITIZE:-}" ]; then
 set -- --disable-asm "--extra-cflags=-fsanitize=$FFMPEG_TEST_SANITIZE -fno-omit-frame-pointer" "--extra-ldflags=-fsanitize=$FFMPEG_TEST_SANITIZE"
else
 set --
fi
mkdir -p "$root/out/build/$mode" "$root/out/install/$mode"
cd "$root/out/build/$mode"
"$root/FFmpeg/configure" --prefix="$root/out/install/$mode" \
 --enable-shared --disable-static --disable-autodetect --disable-everything \
 --disable-network --disable-hwaccels --disable-doc --disable-ffplay \
 --disable-avdevice --disable-swresample --disable-swscale \
 --enable-decoder=h264,hevc --enable-parser=h264,hevc --enable-demuxer=h264,hevc,mov \
 --enable-protocol=file --enable-muxer=framehash,null \
 --enable-encoder=rawvideo,wrapped_avframe --enable-filter=null --enable-debug=3 --assert-level=2 "$@"
make -j6
make install

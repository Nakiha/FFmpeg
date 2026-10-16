#!/usr/bin/env bash
# Offline reference builder: use an ORIGINAL or API-1 source tree, not this
# checkout, when producing install-linux-baseline / install-linux-async.
set -euo pipefail
if [[ $# != 3 ]]; then
    echo "Usage: $0 SOURCE INSTALL BUILD" >&2
    exit 2
fi
reference_source=$(cd -- "$1" && pwd)
mkdir -p -- "$2" "$3"
reference_install=$(cd -- "$2" && pwd)
reference_build=$(cd -- "$3" && pwd)
cd -- "$reference_build"
"$reference_source/configure" \
    --prefix="$reference_install" --enable-shared --disable-static \
    --disable-autodetect --disable-everything --disable-network --disable-hwaccels \
    --disable-doc --disable-ffplay --disable-avdevice \
    --disable-swresample --disable-swscale \
    --enable-decoder=h264,hevc --enable-parser=h264,hevc \
    --enable-demuxer=h264,hevc,mov --enable-protocol=file \
    --enable-muxer=framehash,null --enable-encoder=rawvideo,wrapped_avframe \
    --enable-filter=null --enable-debug=3 --assert-level=2
make -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
make install

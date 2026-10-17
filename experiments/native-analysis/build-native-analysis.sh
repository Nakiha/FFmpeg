#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# Native analysis-core experiment build (instrumented fork + Apple Clang).
#
# Mirrors the FFmpeg-Build analysis configuration (VP_ANALYSIS_ONLY=1, HEVC and
# VVC no-pixels defaults, H264 loop-filter skip) but adds libavformat with
# mov/matroska demuxers so a native driver can feed packets without the JS
# layer. NOT a release input.
#
# Usage: ANALYSIS_STREAM_DIR=<FFmpeg-Build worktree with analysis/stream.c> $0
# ---------------------------------------------------------------------------

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STREAM_DIR="${ANALYSIS_STREAM_DIR:-}"
BUILD="${ANALYSIS_NATIVE_BUILD:-$ROOT/.build-native-analysis}"
DIST="${ANALYSIS_NATIVE_DIST:-$ROOT/dist-native-analysis}"

[ -n "$STREAM_DIR" ] && [ -f "$STREAM_DIR/analysis/stream.c" ] || {
    echo "Set ANALYSIS_STREAM_DIR to an FFmpeg-Build worktree containing analysis/stream.c" >&2
    exit 1
}

FFMPEG_BUILD="$BUILD/work/ffmpeg-native-analysis"
FFMPEG_INSTALL="$BUILD/work/install"
ANALYSIS_CFLAGS="-O3 -DVP_ANALYSIS_ONLY=1 -DVP_HEVC_NO_PIXELS=1 -DVP_VVC_NO_PIXELS=1 -DVP_ANALYSIS_H264_SKIP_LOOP_FILTER=1"

mkdir -p "$FFMPEG_BUILD" "$FFMPEG_INSTALL" "$DIST"

if [ ! -f "$FFMPEG_BUILD/config.h" ]; then
    echo "==> configure (native analysis)"
    ( cd "$FFMPEG_BUILD" && "$ROOT/configure" \
        --prefix="$FFMPEG_INSTALL" \
        --cc=clang \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --disable-avdevice \
        --disable-avfilter \
        --disable-swresample \
        --disable-swscale \
        --disable-network \
        --disable-everything \
        --disable-autodetect \
        --disable-pthreads \
        --disable-w32threads \
        --disable-os2threads \
        --enable-avcodec \
        --enable-avformat \
        --enable-avutil \
        --enable-protocol=file \
        --enable-demuxer=mov,matroska \
        --enable-decoder=h264,hevc,vvc \
        --enable-parser=h264,hevc,vvc \
        --optflags=-O3 \
        "--extra-cflags=$ANALYSIS_CFLAGS" )
fi

echo "==> make"
make -C "$FFMPEG_BUILD" -j"$(sysctl -n hw.ncpu)"

echo "==> link driver"
clang -O2 \
    -I"$FFMPEG_BUILD" \
    -I"$ROOT" \
    -I"$STREAM_DIR/analysis" \
    "$STREAM_DIR/analysis/stream.c" \
    "$ROOT/experiments/native-analysis/vpa_native.c" \
    "$FFMPEG_BUILD/libavformat/libavformat.a" \
    "$FFMPEG_BUILD/libavcodec/libavcodec.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" \
    -lm \
    -o "$DIST/vpa_native"

echo "Built: $DIST/vpa_native"

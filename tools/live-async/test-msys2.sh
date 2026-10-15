#!/usr/bin/env bash
# Generate synthetic fixtures with an external full FFmpeg, test our 7.0.3 build.
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
install_dir=${FFMPEG_LIVE_INSTALL_DIR:-"$source_root/../install-msys2-baseline"}
result_dir=${FFMPEG_LIVE_RESULT_DIR:-"$source_root/../results-msys2-baseline"}
generator=${FFMPEG_LIVE_GENERATOR:-/c/program1/ffmpeg.exe}
export PATH="$install_dir/bin:$PATH"
mkdir -p -- "$result_dir"

"$generator" -hide_banner -version > "$result_dir/generator-version.txt"
"$install_dir/bin/ffmpeg.exe" -hide_banner -version > "$result_dir/tested-version.txt"

for codec in h264 hevc; do
    for reorder in 0 3; do
        fixture="$result_dir/${codec}-b${reorder}.mp4"
        if [[ $codec == h264 ]]; then
            "$generator" -hide_banner -loglevel error -nostdin -y \
                -f lavfi -i testsrc2=size=960x540:rate=25 -frames:v 50 \
                -c:v libx264 -preset ultrafast -pix_fmt yuv420p \
                -x264-params "threads=2:bframes=$reorder:keyint=25:scenecut=0" \
                -an "$fixture"
        else
            "$generator" -hide_banner -loglevel error -nostdin -y \
                -f lavfi -i testsrc2=size=960x540:rate=25 -frames:v 50 \
                -c:v libx265 -preset ultrafast -pix_fmt yuv420p \
                -x265-params "pools=2:frame-threads=2:bframes=$reorder:keyint=25:scenecut=0:log-level=error" \
                -an "$fixture"
        fi
        for threads in 2 12; do
            "$install_dir/bin/ffmpeg.exe" -hide_banner -loglevel error -nostdin -y \
                -threads "$threads" -thread_type frame -i "$fixture" \
                -map 0:v:0 -an -c:v rawvideo -fps_mode passthrough \
                -f framehash "$result_dir/${codec}-b${reorder}-t${threads}.sha256"
        done
        cmp "$result_dir/${codec}-b${reorder}-t2.sha256" \
            "$result_dir/${codec}-b${reorder}-t12.sha256"
    done
    for threads in 2 4 8 12; do
        "$install_dir/bin/latency_probe.exe" "$result_dir/${codec}-b0.mp4" \
            "$threads" 25 frame > "$result_dir/${codec}-t${threads}.csv"
    done
done

python3 "$source_root/tools/live-async/summarize_probe.py" "$result_dir"

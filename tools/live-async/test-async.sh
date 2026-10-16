#!/usr/bin/env bash
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
install_dir=${FFMPEG_LIVE_INSTALL_DIR:?set FFMPEG_LIVE_INSTALL_DIR}
fixture_dir=${FFMPEG_LIVE_FIXTURE_DIR:-"$source_root/../results-msys2-baseline"}
result_dir=${FFMPEG_LIVE_RESULT_DIR:?set FFMPEG_LIVE_RESULT_DIR}
export PATH="$install_dir/bin:/ucrt64/bin:/usr/bin:$PATH"
export LD_LIBRARY_PATH="$install_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p -- "$result_dir"
probe="$install_dir/bin/async_probe"
baseline_dir="$source_root/../results-linux-baseline"
case "$(uname -s)" in *NT*) probe="$probe.exe"; baseline_dir="$source_root/../results-msys2-baseline";; esac
baseline_dir=${FFMPEG_LIVE_BASELINE_RESULT_DIR:-$baseline_dir}
export FFMPEG_LIVE_INSTALL_DIR="$install_dir"
bash "$source_root/tools/live-async/compile-probes.sh"
run_probe() { timeout --kill-after=5s 45s "$probe" "$@"; }

for codec in h264 hevc; do
    for reorder in 0 3; do
        for threads in 2 12; do
            stem="$result_dir/${codec}-b${reorder}-t${threads}"
            for mode in legacy async; do
                run_probe "$fixture_dir/${codec}-b${reorder}.mp4" "$threads" 0 \
                    "$mode" "$stem-$mode"
            done
            cmp "$stem-legacy.sha256" "$stem-async.sha256"
        done
    done
    for threads in 2 4 8 12; do
        run_probe "$fixture_dir/${codec}-b0.mp4" "$threads" 25 legacy \
            "$result_dir/${codec}-paced-legacy-t${threads}"
        run_probe "$fixture_dir/${codec}-b0.mp4" "$threads" 25 async \
            "$result_dir/${codec}-paced-t${threads}"
    done
    # Capacity one forces queue-full retries; three drain/flush/replay cycles.
    run_probe "$fixture_dir/${codec}-b3.mp4" 12 0 async \
        "$result_dir/${codec}-flush" 3 0 1
    # A deliberately slow get_buffer2 executes on codec workers, never the caller.
    run_probe "$fixture_dir/${codec}-b0.mp4" 12 0 async \
        "$result_dir/${codec}-slow" 1 20000 1
    run_probe "$fixture_dir/${codec}-b0.mp4" 12 0 pause \
        "$result_dir/${codec}-pause"
    run_probe "$fixture_dir/${codec}-b0.mp4" 12 0 error \
        "$result_dir/${codec}-error"
    run_probe "$fixture_dir/${codec}-b0.mp4" 12 0 gate \
        "$result_dir/${codec}-gate" 1 0 1
    run_probe "$fixture_dir/${codec}-b0.mp4" 12 0 flush \
        "$result_dir/${codec}-flush-inflight" 1 20000 1
    run_probe "$fixture_dir/${codec}-b0.mp4" 12 0 api \
        "$result_dir/${codec}-api"
    for iteration in 1 2 3 4 5; do
        run_probe "$fixture_dir/${codec}-b0.mp4" 12 0 close \
            "$result_dir/${codec}-close" 1 20000
    done
done
for codec in h264 hevc; do
    for variant in resize 10bit; do
        extension=mp4
        [[ $variant == resize ]] && extension=$codec
        fixture="$fixture_dir/${codec}-${variant}.${extension}"
        if [[ ! -f $fixture ]]; then
            echo "Missing $fixture; run generate-extra-fixtures.ps1 first" >&2
            exit 1
        fi
        for mode in legacy async; do
            run_probe "$fixture" 12 0 "$mode" "$result_dir/${codec}-${variant}-$mode"
        done
        cmp "$result_dir/${codec}-${variant}-legacy.sha256" \
            "$result_dir/${codec}-${variant}-async.sha256"
    done
done
run_probe "$fixture_dir/h264-intra.mp4" 12 0 legacy "$result_dir/intra-legacy"
run_probe "$fixture_dir/h264-intra.mp4" 12 0 async "$result_dir/intra-async" 1 20000
cmp "$result_dir/intra-legacy.sha256" "$result_dir/intra-async.sha256"
python3 "$source_root/tools/live-async/validate_async.py" "$result_dir" "$baseline_dir"

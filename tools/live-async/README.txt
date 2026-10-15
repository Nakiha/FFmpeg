Windows and Linux baseline tools
================================

These tools compile the original FFmpeg 7.0.3 decoder implementation and measure
its behavior before the planned asynchronous early-output changes. They are not
an implementation of the asynchronous extension described in
doc/live-async-decoding.txt.

Build (PowerShell, assuming MSYS2 is installed at C:\msys64):

    & C:\msys64\usr\bin\bash.exe -lc 'bash /d/Code/ffmpeg_live/ffmpeg-7.0.3/tools/live-async/build-msys2.sh'

The script selects the existing UCRT64 GCC/NASM/Make toolchain and explicitly
targets mingw32 x86_64. The resulting programs and DLLs are native Windows
artifacts. MSYS2 is used for building; this is not a WSL build.

The build enables software H.264/HEVC decoding, file/MP4 input, null filtering and
frame-hash output. It uses optimized code, debug symbols and assertion level 2.
It is a small testing build, not a feature-complete production transcode package.

Default output directories, beside the source checkout:

    build-msys2-baseline/      build objects and generated configuration
    install-msys2-baseline/    installed DLLs, headers, executables, latency probe
    results-msys2-baseline/    generated test streams, hashes and timing CSVs

Override with FFMPEG_LIVE_BUILD_DIR, FFMPEG_LIVE_INSTALL_DIR and
FFMPEG_LIVE_RESULT_DIR (MSYS2-style paths). Build concurrency can be selected
with FFMPEG_LIVE_BUILD_JOBS; the default is 8.

Test:

    & C:\msys64\usr\bin\bash.exe -lc 'bash /d/Code/ffmpeg_live/ffmpeg-7.0.3/tools/live-async/test-msys2.sh'

Fixture generation uses /c/program1/ffmpeg.exe by default, overridable with
FFMPEG_LIVE_GENERATOR. This external executable is used only to encode synthetic
960x540, 25-fps fixtures. Decoding and timing use the new 7.0.3 DLLs/programs.
Both generator and tested binary version output are saved with the results.

The suite checks frame hashes/PTS at 2 and 12 frame threads for H.264/HEVC, with
0 or 3 B-frames configured at encoding. It then feeds the B=0 streams at real
time into 2/4/8/12-thread decoders using the service's send-then-receive-until-
EAGAIN pattern. Each run must return all 50 frames in order, with the submit
timestamp propagated through AV_CODEC_FLAG_COPY_OPAQUE, including EOF drain.

summary.csv reports first output time, mean submit-to-output latency, and maximum
send/receive call duration. Mean latency includes the faster EOF-drain tail;
per-frame CSVs should be used to inspect steady state. Timing is descriptive,
not a pass/fail threshold or a production benchmark. Short, simple fixtures
cannot establish worst-case data-call duration or sufficient parallel throughput
for large streams. Such tests belong to the next implementation stage.

Windows testing can validate the common frame-thread scheduler, decoded-frame
correctness and the absence of deliberate pipeline filling after a change.
Linux scheduling, synchronization backends and deployed-server performance must
still be validated on Linux before deployment. BIOS virtualization is not a
prerequisite for this Windows workflow.

Linux/WSL baseline
------------------

Dependencies: GCC, GNU Make, NASM, pkg-config and Python 3. In WSL, use a clean
non-login shell to avoid unrelated toolchain setup in shell startup scripts:

    wsl -d Ubuntu -- /bin/bash --noprofile --norc -c 'bash /mnt/d/Code/ffmpeg_live/ffmpeg-7.0.3/tools/live-async/build-linux.sh'
    wsl -d Ubuntu -- /bin/bash --noprofile --norc -c 'bash /mnt/d/Code/ffmpeg_live/ffmpeg-7.0.3/tools/live-async/test-linux.sh'

Linux objects are built in $XDG_CACHE_HOME/ffmpeg-live/build-linux-baseline, or
$HOME/.cache/ffmpeg-live/build-linux-baseline if XDG_CACHE_HOME is unset. Keeping
objects on WSL's Linux filesystem avoids Windows filesystem metadata overhead.
The source remains in the shared checkout. Installation and results are saved
beside it in install-linux-baseline/ and results-linux-baseline/.

The Linux test reuses the exact four Windows-generated fixtures; select their
directory with FFMPEG_LIVE_FIXTURE_DIR. It checks both Linux 2/12-thread output
equality and the decoded frame hashes/PTS against Windows. It runs the same
paced-input probe without timing pass/fail gates. Data-call behavior and frame
latency are measured separately on each platform.

When running the installed Linux binaries directly, set LD_LIBRARY_PATH to the
installation's lib directory, as the test script does. The Windows and Linux
builds and their shared libraries live in separate directories.

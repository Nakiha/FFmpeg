# VoidPlayer FFmpeg Analyzer

`tools/void_ffmpeg_analyzer.c` is the codec-side analyzer entry point used by
VoidPlayer.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File voidplayer\build_windows_msvc.ps1
```

The script configures FFmpeg with MSVC, uses NASM for x86 assembly, builds
vendored zstd as a static library, builds the tool target, and copies the
executable to:

```text
bin/windows-x64/void_ffmpeg_analyzer.exe
```

## VBS4

The tool writes a VBS4 file:

```text
void_ffmpeg_analyzer.exe --codec hevc --input input.mp4 --vbs4 output.vbs4
void_ffmpeg_analyzer.exe --codec h264 --input input.mp4 --vbs4 output.vbs4
```

Implemented sections:

- `FSUM`: frame summaries collected from decoder internals, including reference
  POC lists for the reference pyramid.
- `FIDX`: frame-to-block index rows.
- `BIDX`: compressed payload block index rows.
- `CPAY`: H.265 `HEVCCU1` or H.264 `H264MB1` column-stream blocks.

Production builds compress VBS4 blocks with statically linked zstd. Set
`VOIDPLAYER_VBS4_NO_COMPRESSION=1` for debug files with `compression=0`.

H.265 records are emitted from `hevcdec.c` after each coding unit is decoded.
H.264 records are emitted from `h264_mb.c` after each macroblock is decoded.
VP9, MPEG-2, and AV1 VBS4 generation are intentionally disabled in this branch
until we add real codec-specific payload profiles for them.

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

## VACHUNK

The tool writes a VACHUNK file:

```text
void_ffmpeg_analyzer.exe --codec hevc --input input.mp4 --vachunk output.vck --start-frame 0 --end-frame 63 --base-revision 1 --generator-revision 1
void_ffmpeg_analyzer.exe --codec h264 --input input.mp4 --vachunk output.vck --start-frame 0 --end-frame 63 --base-revision 1 --generator-revision 1
```

Implemented sections:

- `FSUM`: frame summaries collected from decoder internals, including reference
  POC lists for the reference pyramid.
- `FIDX`: frame-to-CU range index rows.
- `CU4R`: fixed-size overlay CU records for the requested frame range.

Production builds compress VACHUNK blocks with statically linked zstd. Set
`VOIDPLAYER_VACHUNK_NO_COMPRESSION=1` for debug files with `compression=0`.

H.265 records are emitted from `hevcdec.c` after each coding unit is decoded.
H.264 records are emitted from `h264_mb.c` after each macroblock is decoded.
VP9, MPEG-2, and AV1 VACHUNK generation are intentionally disabled in this branch
until we add real codec-specific payload profiles for them.

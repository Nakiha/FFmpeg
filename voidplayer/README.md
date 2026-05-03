# VoidPlayer FFmpeg Analyzer

`tools/void_ffmpeg_analyzer.c` is the codec-side analyzer entry point used by
VoidPlayer.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File voidplayer\build_windows_msvc.ps1
```

The script configures FFmpeg with MSVC, uses NASM for x86 assembly, builds the
tool target, and copies the executable to:

```text
bin/windows-x64/void_ffmpeg_analyzer.exe
```

## VBS3

The tool writes a VBS3 file:

```text
void_ffmpeg_analyzer.exe --codec hevc --input input.mp4 --vbs3 output.vbs3
```

Implemented sections:

- `FSUM`: frame summaries collected from decoder internals for H.265 and
  H.264, including reference POC lists for the reference pyramid.
- `CUID`: one CU index row per frame.
- `CUBL`: real H.265 CU records and H.264 macroblock records.

VP9 and MPEG-2 still use decoder-output frame summaries with an empty CU
payload. They are kept as lightweight frame-view support only.

AV1 VBS3 generation is intentionally disabled for now. This FFmpeg branch's
native AV1 decoder currently requires hardware acceleration on this machine;
when we decide to support AV1, add a real software path such as `libdav1d` or
`libaom` instead of emitting packet-summary stand-ins.

H.265 records are emitted from `hevcdec.c` after each coding unit is decoded.
H.264 records are emitted from `h264_mb.c` after each macroblock is decoded.

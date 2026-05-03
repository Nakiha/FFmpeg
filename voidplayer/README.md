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

The current tool writes a structurally valid VBS3 file:

```text
void_ffmpeg_analyzer.exe --codec hevc --input input.mp4 --vbs3 output.vbs3
```

Implemented sections:

- `FSUM`: frame summaries collected from decoder output for H.265, H.264, VP9,
  and MPEG-2.
- `CUID`: one empty CU index row per frame.
- `CUBL`: present but empty.

AV1 VBS3 generation is intentionally disabled for now. This FFmpeg branch's
native AV1 decoder currently requires hardware acceleration on this machine;
when we decide to support AV1, add a real software path such as `libdav1d` or
`libaom` instead of emitting packet-summary stand-ins.

The empty CU payload is intentional. Real codec block statistics should be
added by instrumenting codec decoder internals and filling `CUBL` with VBS CU
records, rather than synthesizing fake blocks in this command-line layer.

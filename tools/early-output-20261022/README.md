# Early-output research archive, 2026-10-22

> **实验版，最终验收未完成，不建议生产启用**

Read [the publication report](../../doc/early-output-20261022/REPORT.md) first.
This archive preserves phase1 rejection, phase2 pre-hardening patches/results,
and phase3 hardened source evidence. The active FFmpeg source is phase3;
`research/phase2/ffmpeg-early-output.patch` is a different, rejected version.
Historical reports have warning banners; scripts and patches are byte-for-byte
copies. Consult the report's representative-test prefix-collision finding before
accepting the old review's blanket raw-comparison claims.

These scripts were written for a separate lab layout:

```
LAB/
  FFmpeg/                 # matching experimental source, or exact baseline
  NakiLiveTranscode/      # independent application source
  research/              # contents of this archive's research/ directory
  out/                   # private, existing/generated artifacts; never committed
```

They are archived entry points, not an automatic test or install hook. Running
them directly inside this archive does not supply that layout. No script was
executed during publication. Many historical scripts use fixed result names and
would overwrite evidence; use a separate fresh lab and unique result directories
if future testing is separately authorized. In particular the representative
runner currently collides on identical input basenames across directories;
the report marks the affected existing evidence unknown. The scripts are
preserved without repairing or rerunning them in this documentation-only task.

`build-macos.sh` builds a restricted codec harness, not the Linux service. The
standalone probe, performance probe, lifecycle/gate/flush/error/API drivers,
header-contract verifier, mutation generator and FIFO-failure interposer are
research tools. Generator/downloader scripts are included as source only; no
generated fixtures, FATE media, download cache, raw logs or binaries are published.
The exact original and published file hashes are in
`doc/early-output-20261022/ARCHIVE_MANIFEST.json` at the repository root.

Do not apply historical patches over the current source, run source-mutating
helpers, infer a verified-even contract from an encoder name, or reuse phase2
timings as phase3 measurements. The application SDK pin and production defaults
have not been changed by publication.

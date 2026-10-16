API 2 / libavcodec 61.5.100: fixed async 12 versus adaptive dispatch.
All 86 native timing processes exited successfully. Pixel tests are separate.
A collector initially checked a too-early window in the short low-load case;
all raw measurements were retained and independently revalidated with
--validate-performance-only against unchanged core/library/probe fingerprints.
See validate-auto-performance.log, provenance.json and explicit report windows.
ASan/Leak passed a separate no-assembly build; this is not a race-freedom claim.
Windows validates anonymous/process-local automatic budgets; named groups are Linux-only.
Full media, per-frame pixels/metadata and control traces are in the offline bundle.

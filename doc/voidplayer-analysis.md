# Main8/Main10 analysis execution extension

Independent analysis builds may compile `VP_ANALYSIS_ONLY=1`. Ordinary and
playback builds leave it zero; their HEVC pixel path is unchanged. Private HEVC
option `vp_analysis_mode=1` requests no-pixels execution; its default is zero.
`vp_analysis_path` reports full=0/no-pixels=1; `vp_analysis_reason` gives provenance.
`VP_HEVC_NO_PIXELS=0` provides a reproducible full-path control build.

| Parsed configuration | Selection before decoding |
| --- | --- |
| Main profile, 8-bit 4:2:0, single-layer progressive software, one thread | Eligible |
| Main10 profile, 10-bit 4:2:0, same software/layer/progressive limits | Eligible |
| Other profile/depth combinations, chroma, fields, multilayer | Full |
| PCM, transquant bypass, SPS/PPS extensions | Full |
| Ordinary transform skip, tiles/WPP/dependent slices/TMVP/weighted prediction | Eligible within the bounds above |
| Missing complete PPS, hardware/threaded decode, CRC verification requested | Full |
| Film grain or decoded-picture-hash already known | Full |

Admission reuses FFmpeg's existing parameter parser, without a second CABAC
scan. Coefficient/sign/Rice/context consumption, QP, CU/TU traversal, intra modes,
merge/MVP/temporal motion tables, references, DPB, allocation, reordering and
picture completion stay active. Only filter pixels, prediction/MC and residual
materialization/dequant/inverse transform/add-back are bypassed.

After pruning starts, successfully parsed parameter replacements (including
repeated in-band sets), new extradata, film grain or any decoded-picture-hash SEI
return `VP_ANALYSIS_RETRY_FULL`. Malformed parameter sets keep their parse error.
This tag is distinct from ENOMEM, corrupt data and missing references. The caller
must destroy this decoder/DPB and replay from a proven anchor with required
preroll and pruning disabled; it must never resume pixel reconstruction in the
no-pixels DPB. The caller permits at most one such retry per request.

Speculative results remain private until the requested transaction succeeds.
Web reserves ownership before payload allocation under its existing in-flight
budget, and commits through existing per-frame backpressure. Dependent/open or
unsealed work, summary, viewport, range and legacy requests use full execution.
Only explicit single-picture blocks requests opt into the fast path. Cancellation/resource
failures do not automatically retry. Exact-picture publication includes its own
packet and suffix SEI processing, and makes no promise about unseen future AUs.
Summary publication remains incremental without speculative GOP staging.

Hash presence selects full execution without adding a hash-validation promise:
the existing full decoder `err_recognition` policy still controls MD5 checking.
Main10 QP/invalid-output contracts stay unchanged. The legacy CU callback carries
an unsigned, clipped QP; consumers must retain unsupported/null Main10 QP and must
not infer accurate signed QP from those bytes. Main10 pruning only accelerates
the existing block/mode/reference result, without adding a QP capability or ABI.
H264/VVC receive no pruning.

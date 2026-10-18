# Plain RTMP packet read origin (API 1)

Opt in with protocol option `rtmp_read_origin=1` before opening plain RTMP
input. TCP records the completion time of the recv that returns the first
basic-header byte of each RTMP chunk. Packet assembly retains the first
chunk's origin across partial and interleaved channels. This is app read time,
not wire arrival, readiness wait, kernel queue age or remote display time.

A bounded 256-entry ledger maps absolute synthesized FLV tag positions to
immutable first-read and RTMP assembly completion times. FLV attaches
`AVReadOrigin` in AVPacket.opaque_ref before probe buffering and parsing.
`read_origin.h` exposes validation/access; consumers must copy provenance
before replacing opaque_ref. Missing positions/overflow fail explicitly.
No network_observer callbacks are used. Disabled input does not read clocks
at TCP or change demux packet opaque_ref. Aggregates, TLS/tunnels, seeks and
fake duration insertion are explicitly unsupported in this opt-in version.

The libvvenc option `live-max-future=1..4` is restricted to CQF live PTS GOP,
no RC lookahead, no frame parallelism or future MCTF. A separate bounded CTS
ledger counts submitted display pictures newer than each released AU and
fails rather than emitting when the configured bound is exceeded. Packet
strings metadata `live_future_frames` reports that conservative release-time
upper bound. It is not a CPU time limit or total pending-frame count.

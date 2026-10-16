/* Fork-only bounded live muxing. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef AVFORMAT_LIVE_MUX_H
#define AVFORMAT_LIVE_MUX_H
#include "avformat.h"
#define AVFORMAT_LIVE_MUX_VERSION 1

/* Demux owner only, after initial probing. Limits subsequent raw codec probes
 * while packets continue arriving, without disabling packet buffering. Blocking
 * transport reads still need the caller's ordinary I/O interrupt deadline. */
int avformat_live_input_probe_limit(AVFormatContext *s, int64_t wait_us);

typedef struct AVLiveMuxStats {
    unsigned queued_packets, high_water_packets;
    uint64_t queued_bytes, high_water_bytes, timeout_releases, pressure_releases, forced_releases;
} AVLiveMuxStats;

/* Call after write_header, on the sole mux owner thread. FLV/MPEG-TS only.
 * Use send/receive followed exclusively by av_write_frame, not the generic
 * av_interleaved_write_frame queue. No internal thread/timer is created.
 * Poll receive at a bounded interval even when no input packet arrives.
 * wait_us is a monotonic wall-clock bound (1..1000000); capacities are hard.
 * The bound excludes muxer computation, transport I/O and caller scheduling. */
int avformat_live_mux_init(AVFormatContext *s, int64_t wait_us,
                          unsigned packets, uint64_t bytes);
/* Copies parameters. Only fully described AAC/MP3 audio may be appended.
 * Registration performs no I/O, changes no existing PID/track/PCR state,
 * and rolls back on failure. Sequence header/PSI is sent before media.
 * No stream removal or parameter replacement. Maximum 32 total streams. */
int avformat_live_add_audio(AVFormatContext *s, const AVCodecParameters *par,
                           AVRational time_base, AVStream **out);
/* send takes packet references on success; EAGAIN leaves input unchanged. */
int avformat_live_mux_send(AVFormatContext *s, AVPacket *pkt);
/* 1 = moved packet to empty dst; 0 = not ready; <0 = error.
 * force drains EOF/capacity pressure. Stream DTS must be monotonic, but late
 * packets can be globally older after a deadline; application admission policy
 * must handle such packets. No packets are silently dropped by this API. */
int avformat_live_mux_receive(AVFormatContext *s, AVPacket *dst, int force);
int avformat_live_mux_stats(AVFormatContext *s, AVLiveMuxStats *stats);
#endif

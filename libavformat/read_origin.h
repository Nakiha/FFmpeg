/* Opt-in plain RTMP receive provenance, LGPL-2.1-or-later. */
#ifndef AVFORMAT_READ_ORIGIN_H
#define AVFORMAT_READ_ORIGIN_H
#include <stdint.h>
#include "libavcodec/packet.h"
#define AVFORMAT_READ_ORIGIN_VERSION 1
#define AV_READ_ORIGIN_MAGIC UINT64_C(0x4e414b4952544d50)
typedef struct AVReadOrigin {
    uint64_t magic;
    int64_t first_read_us; /* first RTMP chunk's first byte, after recv returns */
    int64_t complete_us;   /* complete RTMP media message assembled */
    int64_t tag_position;  /* absolute synthesized FLV tag offset */
} AVReadOrigin;
/* rtmp_read_origin=1 is restricted to plain RTMP input, consumes opaque_ref
 * on demux packets, rejects aggregate messages/seeks, and fails on ledger loss.
 * No socket observer correlation, wire timestamp or peer/display measurement.
 * Probe/parser packet refs preserve this immutable origin. Copy it before an
 * application replaces opaque_ref. All times are av_gettime_relative(). */
static inline const AVReadOrigin *avformat_packet_read_origin(const AVPacket *p)
{
    const AVReadOrigin *o = p && p->opaque_ref && p->opaque_ref->size == sizeof(AVReadOrigin)
        ? (const AVReadOrigin *)p->opaque_ref->data : NULL;
    return o && o->magic == AV_READ_ORIGIN_MAGIC && o->first_read_us > 0 && o->complete_us >= o->first_read_us ? o : NULL;
}
#endif

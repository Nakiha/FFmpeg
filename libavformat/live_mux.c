/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "config_components.h"
#include "live_mux.h"
#include "avformat_internal.h"
#include "mux.h"
#include "mpegts.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"

typedef struct LiveEntry { AVPacket *pkt; int64_t entered; } LiveEntry;
int avformat_live_input_probe_limit(AVFormatContext *s, int64_t wait_us)
{
    if (!s || !s->iformat || wait_us < 1 || wait_us > 1000000) return AVERROR(EINVAL);
    ff_fc_internal(s)->live_input_probe_us = wait_us;
    return 0;
}
int avformat_live_input_probe_poll(AVFormatContext *s, int64_t wait_us)
{
    if (!s || !s->iformat || !s->pb || !s->nb_streams || wait_us < 1 || wait_us > 1000000)
        return AVERROR(EINVAL);
#if CONFIG_MPEGTS_DEMUXER
    if (strcmp(s->iformat->name, "mpegts"))
        return AVERROR(ENOSYS);
    ff_mpegts_enable_live_poll(s);
    ff_fc_internal(s)->live_input_probe_us = wait_us;
    ff_fc_internal(s)->live_input_poll = 1;
    return 0;
#else
    return AVERROR(ENOSYS);
#endif
}
typedef struct LiveMuxContext {
    LiveEntry queue[256];
    unsigned capacity;
    uint64_t byte_capacity;
    int64_t wait_us;
    AVLiveMuxStats stats;
} LiveMuxContext;

void ff_live_mux_free(AVFormatContext *s)
{
    LiveMuxContext *q = ff_fc_internal(s)->live_mux;
    if (!q) return;
    for (unsigned i = 0; i < q->stats.queued_packets; ++i)
        av_packet_free(&q->queue[i].pkt);
    av_freep(&ff_fc_internal(s)->live_mux);
}

int avformat_live_mux_init(AVFormatContext *s, int64_t wait_us, unsigned packets, uint64_t bytes)
{
    FormatContextInternal *fci;
    LiveMuxContext *q;
    if (!s || !s->oformat || !ffofmt(s->oformat)->live_add_stream ||
        !ff_fc_internal(s)->live_header_written || !ff_fc_internal(s)->initialized ||
        ff_fc_internal(s)->fc.packet_buffer.head || s->nb_streams > 32)
        return AVERROR(EINVAL);
    fci = ff_fc_internal(s);
    if (fci->live_mux) return AVERROR(EALREADY);
    if (wait_us < 1 || wait_us > 1000000 || !packets || packets > 256 || !bytes || bytes > INT_MAX)
        return AVERROR(EINVAL);
    q = av_mallocz(sizeof(*q));
    if (!q) return AVERROR(ENOMEM);
    q->wait_us = wait_us; q->capacity = packets; q->byte_capacity = bytes;
    fci->live_mux = q;
    return 0;
}

int avformat_live_add_audio(AVFormatContext *s, const AVCodecParameters *par,
                           AVRational tb, AVStream **out)
{
    AVStream *st;
    FFStream *sti;
    int ret;
    if (out) *out = NULL;
    if (!s || !out || !par || !ff_fc_internal(s)->live_mux || !ff_fc_internal(s)->live_header_written || s->nb_streams >= 32 ||
        tb.num <= 0 || tb.den <= 0 || par->codec_type != AVMEDIA_TYPE_AUDIO ||
        (par->codec_id != AV_CODEC_ID_AAC && par->codec_id != AV_CODEC_ID_MP3) ||
        par->sample_rate <= 0 || par->sample_rate > 384000 || par->ch_layout.nb_channels <= 0 ||
        par->ch_layout.nb_channels > 8 || par->extradata_size < 0 || par->extradata_size > 65536 ||
        (par->codec_id == AV_CODEC_ID_AAC && (!par->extradata || par->extradata_size < 2)))
        return AVERROR(EINVAL);
    st = avformat_new_stream(s, NULL);
    if (!st) return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_copy(st->codecpar, par)) < 0) goto fail;
    st->codecpar->codec_tag = av_codec_get_tag(s->oformat->codec_tag, par->codec_id);
    st->time_base = tb;
    if ((ret = ffofmt(s->oformat)->live_add_stream(s, st)) < 0) goto fail;
    sti = ffstream(st);
    sti->is_intra_only = 1;
    sti->priv_pts.val = sti->priv_pts.num = 0;
    sti->priv_pts.den = (int64_t)st->time_base.num * par->sample_rate;
    ff_fc_internal(s)->nb_interleaved_streams++;
    *out = st;
    return 0;
fail:
    ff_remove_stream(s, st);
    return ret;
}

int avformat_live_mux_send(AVFormatContext *s, AVPacket *pkt)
{
    LiveMuxContext *q = ff_fc_internal(s)->live_mux;
    AVPacket *copy;
    unsigned pos, count;
    if (!q || !ff_fc_internal(s)->live_header_written || !pkt || pkt->stream_index < 0 || pkt->stream_index >= s->nb_streams ||
        pkt->size <= 0 || pkt->dts == AV_NOPTS_VALUE) return AVERROR(EINVAL);
    if (pkt->size > q->byte_capacity) return AVERROR(EMSGSIZE);
    count = q->stats.queued_packets;
    if (count == q->capacity || q->stats.queued_bytes + pkt->size > q->byte_capacity) return AVERROR(EAGAIN);
    copy = av_packet_alloc();
    if (!copy) return AVERROR(ENOMEM);
    av_packet_move_ref(copy, pkt);
    for (pos = count; pos; --pos) {
        AVPacket *prev = q->queue[pos-1].pkt;
        if (av_compare_ts(prev->dts, s->streams[prev->stream_index]->time_base,
                          copy->dts, s->streams[copy->stream_index]->time_base) <= 0) break;
        q->queue[pos] = q->queue[pos-1];
    }
    q->queue[pos] = (LiveEntry){copy, av_gettime_relative()};
    q->stats.queued_packets++;
    q->stats.queued_bytes += copy->size;
    q->stats.high_water_packets = FFMAX(q->stats.high_water_packets, q->stats.queued_packets);
    q->stats.high_water_bytes = FFMAX(q->stats.high_water_bytes, q->stats.queued_bytes);
    return 0;
}

int avformat_live_mux_receive(AVFormatContext *s, AVPacket *dst, int force)
{
    LiveMuxContext *q = ff_fc_internal(s)->live_mux;
    unsigned count, present = 0;
    uint32_t streams = 0;
    int64_t oldest = INT64_MAX;
    int timed, pressure;
    if (!q || !dst || dst->buf || dst->data || dst->side_data_elems) return AVERROR(EINVAL);
    if (!(count = q->stats.queued_packets)) return 0;
    for (unsigned i = 0; i < count; ++i) {
        streams |= UINT32_C(1) << q->queue[i].pkt->stream_index;
        oldest = FFMIN(oldest, q->queue[i].entered);
    }
    for (unsigned i = 0; i < s->nb_streams; ++i) {
        enum AVMediaType type = s->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO) {
            if (!(streams & (UINT32_C(1) << i))) break;
        }
        present++;
    }
    timed = av_gettime_relative() - oldest >= q->wait_us;
    pressure = count == q->capacity || q->stats.queued_bytes >= q->byte_capacity;
    if (!force && !timed && !pressure && present != s->nb_streams) return 0;
    if (timed) q->stats.timeout_releases++;
    if (pressure) q->stats.pressure_releases++;
    if (force) q->stats.forced_releases++;
    q->stats.queued_bytes -= q->queue[0].pkt->size;
    q->stats.queued_packets--;
    av_packet_move_ref(dst, q->queue[0].pkt);
    av_packet_free(&q->queue[0].pkt);
    memmove(q->queue, q->queue+1, (count-1)*sizeof(q->queue[0]));
    return 1;
}

int avformat_live_mux_stats(AVFormatContext *s, AVLiveMuxStats *stats)
{
    LiveMuxContext *q = ff_fc_internal(s)->live_mux;
    if (!q || !stats) return AVERROR(EINVAL);
    *stats = q->stats;
    return 0;
}

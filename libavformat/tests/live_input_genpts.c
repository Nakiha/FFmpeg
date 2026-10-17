/* Fork-only GENPTS error/EOF regression. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "config.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavformat/avformat_internal.h"
#include "libavformat/demux.h"

typedef struct Fault {
    int error, calls, cancelled, released;
} Fault;

static int interrupted(void *opaque)
{
    return ((Fault *)opaque)->cancelled;
}

static int fault_packet(AVFormatContext *s, AVPacket *pkt)
{
    Fault *f = s->opaque;
    (void)pkt;
    f->calls++;
    if (f->error == AVERROR_EXIT)
        f->cancelled = 1; /* Cancellation begins inside read, after entry check. */
    return f->error;
}

static const FFInputFormat fault_demuxer = {
    .p.name = "genpts-fault-test",
    .p.flags = AVFMT_NOFILE,
    .read_packet = fault_packet,
};

static void released(void *opaque, uint8_t *data)
{
    ((Fault *)opaque)->released++;
    av_free(data);
}

static void check(const char *name, int error, int poll, int fixed)
{
    Fault f = { .error = error };
    AVFormatContext *s = avformat_alloc_context();
    AVPacket *out = av_packet_alloc();
    AVStream *st;
    PacketListEntry *entry = av_mallocz(sizeof(*entry));
    uint8_t *data = av_mallocz(1 + AV_INPUT_BUFFER_PADDING_SIZE);
    int ret, cached, expected;
    av_assert0(s && out && entry && data);
    s->iformat = &fault_demuxer.p;
    s->opaque = &f;
    s->flags |= AVFMT_FLAG_GENPTS;
    s->interrupt_callback = (AVIOInterruptCB){ interrupted, &f };
    ff_fc_internal(s)->live_input_poll = poll;
    st = avformat_new_stream(s, NULL);
    av_assert0(st);
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    st->time_base = (AVRational){ 1, 1000 };
    st->pts_wrap_bits = 33;

    /* A reference packet already buffered by GENPTS, awaiting a later DTS.
     * No timing race or real network is needed to reach this exact branch. */
    entry->pkt.buf = av_buffer_create(data, 1 + AV_INPUT_BUFFER_PADDING_SIZE,
                                      released, &f, 0);
    av_assert0(entry->pkt.buf);
    data[0] = 0x5a;
    entry->pkt.data = data;
    entry->pkt.size = 1;
    entry->pkt.stream_index = st->index;
    entry->pkt.pts = AV_NOPTS_VALUE;
    entry->pkt.dts = 100;
    entry->pkt.duration = 40;
    entry->pkt.pos = -1;
    ffformatcontext(s)->packet_buffer = (PacketList){ entry, entry };

    ret = av_read_frame(s, out);
    cached = ffformatcontext(s)->packet_buffer.head != NULL;
    expected = error == AVERROR_EOF ? 0 :
               error == AVERROR(EAGAIN) ? error : poll && fixed ? error : 0;
    printf("{\"case\":\"%s\",\"poll\":%d,\"read_calls\":%d,\"result\":%d,\"cached\":%d,\"pts\":%lld}\n",
           name, poll, f.calls, ret, cached, (long long)out->pts);
    fflush(stdout);
    av_assert0(f.calls == 1 && ret == expected);
    if (ret < 0) {
        av_assert0(cached && entry->pkt.pts == AV_NOPTS_VALUE &&
                   !out->buf && !out->size && !f.released);
        /* Hard errors and EAGAIN must leave ownership/PTS intact. A subsequent
         * deliberate true EOF may still synthesize and drain that same item. */
        f.error = AVERROR_EOF;
        f.cancelled = 0;
        av_assert0(!av_read_frame(s, out));
    } else {
        av_assert0(!cached);
    }
    av_assert0(out->pts == 140 && out->dts == 100 && out->duration == 40 &&
               out->size == 1 && out->data[0] == 0x5a);
    av_packet_unref(out);
    f.error = AVERROR_EOF;
    f.cancelled = 0;
    av_assert0(f.released == 1 && av_read_frame(s, out) == AVERROR_EOF);
    av_packet_free(&out);
    avformat_close_input(&s);
    av_assert0(f.released == 1);
}

int main(int argc, char **argv)
{
    int fixed = argc == 1;
    av_assert0(fixed || (argc == 2 && !strcmp(argv[1], "--expect-before-fix")));
    check("EIO", AVERROR(EIO), 1, fixed);
    check("read-time-cancel", AVERROR_EXIT, 1, fixed);
    check("true-EOF", AVERROR_EOF, 1, fixed);
    check("EAGAIN", AVERROR(EAGAIN), 1, fixed);
    check("legacy-EIO", AVERROR(EIO), 0, fixed);
    return 0;
}

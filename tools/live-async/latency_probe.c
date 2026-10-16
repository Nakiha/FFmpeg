/*
 * Paced-input baseline probe for software frame-thread decoding.
 * Usage: latency_probe INPUT THREADS FPS [frame|slice]
 * This measures time after demux/open; it does not measure network or filtering.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/buffer.h"
#include "libavutil/error.h"
#include "libavutil/time.h"

typedef struct PacketStamp {
    int64_t sequence;
    int64_t submitted_us;
} PacketStamp;

typedef struct ProbeStats {
    int64_t started_us;
    int64_t frames;
    int64_t stamped_frames;
    int64_t total_latency_us;
    int64_t first_frame_us;
    int64_t max_send_us;
    int64_t max_receive_us;
} ProbeStats;

static int receive_available(AVCodecContext *ctx, AVFrame *frame,
                             ProbeStats *stats)
{
    for (;;) {
        int64_t began = av_gettime_relative();
        int ret = avcodec_receive_frame(ctx, frame);
        int64_t now = av_gettime_relative();
        int64_t duration = now - began;
        if (duration > stats->max_receive_us)
            stats->max_receive_us = duration;
        if (ret < 0)
            return ret;
        if (!stats->frames)
            stats->first_frame_us = now - stats->started_us;
        stats->frames++;
        if (frame->opaque_ref &&
            frame->opaque_ref->size == sizeof(PacketStamp)) {
            const PacketStamp *stamp = (const PacketStamp *)frame->opaque_ref->data;
            int64_t latency = now - stamp->submitted_us;
            stats->stamped_frames++;
            stats->total_latency_us += latency;
            printf("frame,%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                   stats->frames, stamp->sequence, latency);
        }
        av_frame_unref(frame);
    }
}

int main(int argc, char **argv)
{
    AVFormatContext *format = NULL;
    AVCodecContext *ctx = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    const AVCodec *codec;
    ProbeStats stats = { 0 };
    int stream, ret = 0, threads, fps, requested_type;
    int64_t submitted = 0;

    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s INPUT THREADS FPS [frame|slice]\n", argv[0]);
        return 2;
    }
    threads = atoi(argv[2]);
    fps = atoi(argv[3]);
    if (threads < 1 || fps < 1 || fps > 1000 ||
        (argc == 5 && strcmp(argv[4], "frame") && strcmp(argv[4], "slice")))
        return 2;
    requested_type = argc == 5 && !strcmp(argv[4], "slice")
                   ? FF_THREAD_SLICE : FF_THREAD_FRAME;
    av_log_set_level(AV_LOG_ERROR);
    if ((ret = avformat_open_input(&format, argv[1], NULL, NULL)) < 0 ||
        (ret = avformat_find_stream_info(format, NULL)) < 0)
        goto end;
    stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0) {
        ret = stream;
        goto end;
    }
    ctx = avcodec_alloc_context3(codec);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!ctx || !packet || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = avcodec_parameters_to_context(ctx, format->streams[stream]->codecpar)) < 0)
        goto end;
    ctx->pkt_timebase = format->streams[stream]->time_base;
    ctx->thread_count = threads;
    ctx->thread_type = requested_type;
    ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    if ((ret = avcodec_open2(ctx, codec, NULL)) < 0)
        goto end;

    printf("# codec=%s threads=%d active_thread_type=%d delay=%d fps=%d\n",
           codec->name, ctx->thread_count, ctx->active_thread_type, ctx->delay, fps);
    printf("# frame,output_index,input_sequence,submit_to_output_us\n");
    stats.started_us = av_gettime_relative();
    while ((ret = av_read_frame(format, packet)) >= 0) {
        int64_t due, now, began, duration;
        PacketStamp *stamp;
        if (packet->stream_index != stream) {
            av_packet_unref(packet);
            continue;
        }
        due = stats.started_us + submitted * 1000000 / fps;
        while ((now = av_gettime_relative()) < due)
            av_usleep((unsigned)(due - now));
        av_buffer_unref(&packet->opaque_ref);
        packet->opaque_ref = av_buffer_alloc(sizeof(PacketStamp));
        if (!packet->opaque_ref) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        stamp = (PacketStamp *)packet->opaque_ref->data;
        stamp->sequence = submitted;
        stamp->submitted_us = av_gettime_relative();
        began = av_gettime_relative();
        ret = avcodec_send_packet(ctx, packet);
        duration = av_gettime_relative() - began;
        if (duration > stats.max_send_us)
            stats.max_send_us = duration;
        /* Every accepted packet is followed by a complete receive loop. */
        if (ret < 0)
            goto end;
        submitted++;
        av_packet_unref(packet);
        ret = receive_available(ctx, frame, &stats);
        if (ret != AVERROR(EAGAIN))
            goto end;
    }
    if (ret != AVERROR_EOF)
        goto end;
    if ((ret = avcodec_send_packet(ctx, NULL)) < 0)
        goto end;
    ret = receive_available(ctx, frame, &stats);
    if (ret != AVERROR_EOF)
        goto end;
    if (!stats.frames || stats.stamped_frames != stats.frames) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    printf("summary,codec=%s,threads=%d,packets=%" PRId64 ",frames=%" PRId64
           ",stamped_frames=%" PRId64 ",first_frame_us=%" PRId64
           ",mean_latency_us=%" PRId64 ",max_send_us=%" PRId64
           ",max_receive_us=%" PRId64 "\n",
           codec->name, ctx->thread_count, submitted, stats.frames,
           stats.stamped_frames, stats.first_frame_us,
           stats.total_latency_us / stats.stamped_frames,
           stats.max_send_us, stats.max_receive_us);
    ret = 0;
end:
    if (ret < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, message, sizeof(message));
        fprintf(stderr, "Probe failed: %s\n", message);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    avformat_close_input(&format);
    return ret < 0 ? 1 : 0;
}

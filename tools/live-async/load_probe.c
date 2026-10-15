/* Linux-only benchmark; compile the same source against the frozen original
 * libraries and the fork. Hashing is a separate, explicitly untimed workload.
 * No demux, file output, packet allocation or pixel copy in performance timing.
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#include "libavcodec/avcodec.h"
#ifdef LIVE_ASYNC
#include "libavcodec/async.h"
#endif
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libavutil/sha.h"
#include "libavutil/time.h"

typedef struct Stamp { int sequence; int64_t submitted; } Stamp;
typedef struct Row {
    int sequence, width, height, format;
    int64_t pts, best, dts, duration, received, submitted;
} Row;

static int64_t cpu_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * INT64_C(1000000) + ts.tv_nsec / 1000;
}

#ifdef LIVE_ASYNC
static void notify(void *opaque)
{
    uint64_t one = 1;
    ssize_t ignored = write(*(int *)opaque, &one, sizeof(one));
    (void)ignored;
}
#endif

static void wait_progress(int fd, int64_t us)
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    int ms = (int)((us + 999) / 1000);
    if (fd < 0) { if (us > 0) av_usleep((unsigned)us); return; }
    if (poll(&pfd, 1, ms) > 0) {
        uint64_t value;
        ssize_t ignored = read(fd, &value, sizeof(value));
        (void)ignored;
    }
}

static int hash_frame(FILE *out, const AVFrame *frame)
{
    int size = av_image_get_buffer_size(frame->format, frame->width, frame->height, 1);
    struct AVSHA *sha = av_sha_alloc();
    uint8_t *pixels = size > 0 ? av_malloc(size) : NULL, hash[32];
    int ret;
    if (!sha || !pixels) { av_free(sha); av_free(pixels); return AVERROR(ENOMEM); }
    ret = av_image_copy_to_buffer(pixels, size, (const uint8_t *const *)frame->data,
                                 frame->linesize, frame->format, frame->width, frame->height, 1);
    if (ret >= 0) {
        av_sha_init(sha, 256); av_sha_update(sha, pixels, size); av_sha_final(sha, hash);
        fprintf(out, "%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%d,%d,%d,%d,%d,%d,%d,%d,",
                frame->pts, frame->best_effort_timestamp, frame->pkt_dts, frame->duration,
                frame->width, frame->height, frame->format,
                frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den,
                frame->color_range, frame->color_primaries, frame->color_trc, frame->colorspace);
        for (int i = 0; i < 32; i++) fprintf(out, "%02x", hash[i]);
        fputc('\n', out);
    }
    av_free(sha); av_free(pixels);
    return ret < 0 ? ret : 0;
}

int main(int argc, char **argv)
{
    AVFormatContext *format = NULL;
    AVCodecContext *ctx = NULL;
    const AVCodec *codec = NULL;
    AVPacket *packet = NULL, **packets = NULL;
    AVFrame *frame = NULL;
#ifdef LIVE_ASYNC
    AVCodecAsyncContext *owner = NULL;
    AVCodecAsyncStats stats = { 0 };
#endif
    Row *rows = NULL;
    FILE *hashes = NULL, *csv = NULL, *summary = NULL;
    char path[2048];
    int ret = 0, count = 0, stream, threads, fps, asynchronous, verify;
    int next = 0, frames = 0, closed = 0, done = 0, fd = -1;
    int peak_inflight = 0, peak_workers = 0;
    int64_t began, cpu_began, elapsed, cpu_elapsed, eof_at = -1;
    int64_t max_send = 0, max_receive = 0, eagain = 0;
    AVRational tb;
    if (argc != 8) {
        fprintf(stderr, "Usage: %s INPUT THREADS FPS legacy|async perf|verify CAPACITY PREFIX\n", argv[0]);
        return 2;
    }
    threads = atoi(argv[2]); fps = atoi(argv[3]);
    asynchronous = !strcmp(argv[4], "async"); verify = !strcmp(argv[5], "verify");
    if (threads < 2 || threads > 12 || fps < 0 || fps > 1000) return 2;
#ifndef LIVE_ASYNC
    if (asynchronous) return 2;
#endif
    av_log_set_level(AV_LOG_ERROR);
    if ((ret = avformat_open_input(&format, argv[1], NULL, NULL)) < 0 ||
        (ret = avformat_find_stream_info(format, NULL)) < 0) goto end;
    stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0) { ret = stream; goto end; }
    tb = format->streams[stream]->time_base;
    packet = av_packet_alloc(); frame = av_frame_alloc();
    if (!packet || !frame) { ret = AVERROR(ENOMEM); goto end; }
    while ((ret = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == stream) {
            AVPacket **grown = av_realloc_array(packets, count + 1, sizeof(*packets));
            if (!grown) { ret = AVERROR(ENOMEM); goto end; }
            packets = grown;
            packets[count] = av_packet_clone(packet);
            if (!packets[count]) { ret = AVERROR(ENOMEM); goto end; }
            av_buffer_unref(&packets[count]->opaque_ref);
            packets[count]->opaque_ref = av_buffer_allocz(sizeof(Stamp));
            if (!packets[count]->opaque_ref) { count++; ret = AVERROR(ENOMEM); goto end; }
            ((Stamp *)packets[count]->opaque_ref->data)->sequence = count;
            count++;
        }
        av_packet_unref(packet);
    }
    if (ret != AVERROR_EOF || count < 96) { ret = AVERROR_INVALIDDATA; goto end; }
    rows = av_calloc(count, sizeof(*rows)); ctx = avcodec_alloc_context3(codec);
    if (!rows || !ctx) { ret = AVERROR(ENOMEM); goto end; }
    if ((ret = avcodec_parameters_to_context(ctx, format->streams[stream]->codecpar)) < 0) goto end;
    ctx->pkt_timebase = tb; ctx->thread_count = threads; ctx->thread_type = FF_THREAD_FRAME;
    ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    if ((ret = avcodec_open2(ctx, codec, NULL)) < 0) goto end;
    if (!(ctx->active_thread_type & FF_THREAD_FRAME)) { ret = AVERROR(EINVAL); goto end; }
#ifdef LIVE_ASYNC
    if (asynchronous) {
        fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) { ret = AVERROR(errno); goto end; }
        if ((ret = avcodec_async_create(&owner, &ctx, atoi(argv[6]), notify, &fd)) < 0) goto end;
    }
#endif
    if (verify) {
        snprintf(path, sizeof(path), "%s.sha256", argv[7]); hashes = fopen(path, "w");
        if (!hashes) { ret = AVERROR(EIO); goto end; }
    }
    cpu_began = cpu_us(); began = av_gettime_relative();
    while (!done) {
        int progressed = 0, blocked = 0;
        int64_t now = av_gettime_relative();
        if (now - began > 120000000) { ret = AVERROR(ETIMEDOUT); goto end; }
        while (next < count && (!fps || now >= began + (int64_t)next * 1000000 / fps)) {
            int64_t call, cost;
            ((Stamp *)packets[next]->opaque_ref->data)->submitted = av_gettime_relative();
            call = av_gettime_relative();
#ifdef LIVE_ASYNC
            if (asynchronous) ret = avcodec_async_send_packet(owner, packets[next]); else
#endif
            ret = avcodec_send_packet(ctx, packets[next]);
            cost = av_gettime_relative() - call;
            if (cost > max_send) max_send = cost;
            if (ret == AVERROR(EAGAIN)) { eagain++; blocked = 1; break; }
            if (ret < 0) goto end;
            next++; progressed = 1;
            if (!asynchronous) break;
            now = av_gettime_relative();
        }
        if (next == count && !closed) {
#ifdef LIVE_ASYNC
            if (asynchronous) ret = avcodec_async_send_packet(owner, NULL); else
#endif
            ret = avcodec_send_packet(ctx, NULL);
            if (ret == 0) { closed = 1; eof_at = av_gettime_relative() - began; progressed = 1; }
            else if (ret != AVERROR(EAGAIN)) goto end;
        }
        for (;;) {
            int64_t call = av_gettime_relative(), cost;
            const Stamp *stamp;
#ifdef LIVE_ASYNC
            if (asynchronous) ret = avcodec_async_receive_frame(owner, frame); else
#endif
            ret = avcodec_receive_frame(ctx, frame);
            now = av_gettime_relative(); cost = now - call;
            if (cost > max_receive) max_receive = cost;
            if (ret == AVERROR_EOF) { done = 1; break; }
            if (ret == AVERROR(EAGAIN)) break;
            if (ret < 0) goto end;
            if (frames >= count || !frame->opaque_ref || frame->opaque_ref->size != sizeof(Stamp)) {
                ret = AVERROR_INVALIDDATA; goto end;
            }
            stamp = (const Stamp *)frame->opaque_ref->data;
            rows[frames++] = (Row){ stamp->sequence, frame->width, frame->height, frame->format,
                frame->pts, frame->best_effort_timestamp, frame->pkt_dts, frame->duration,
                now - began, stamp->submitted - began };
            if (verify && (ret = hash_frame(hashes, frame)) < 0) goto end;
            av_frame_unref(frame); progressed = 1;
        }
        if (!done && !progressed) {
            int64_t wait = 100000;
            if (fps && next < count && !blocked) {
                wait = began + (int64_t)next * 1000000 / fps - av_gettime_relative();
                if (wait < 0) wait = 0;
            }
            wait_progress(fd, wait);
        }
    }
    elapsed = av_gettime_relative() - began; cpu_elapsed = cpu_us() - cpu_began;
    if (frames != count) { ret = AVERROR_INVALIDDATA; goto end; }
#ifdef LIVE_ASYNC
    if (asynchronous) {
        avcodec_async_get_stats(owner, &stats);
        peak_inflight = stats.peak_inflight; peak_workers = stats.peak_active_workers;
    }
#endif
    snprintf(path, sizeof(path), "%s.csv", argv[7]); csv = fopen(path, "w");
    snprintf(path, sizeof(path), "%s.json", argv[7]); summary = fopen(path, "w");
    if (!csv || !summary) { ret = AVERROR(EIO); goto end; }
    fprintf(csv, "output,sequence,pts,best_effort,dts,duration,width,height,format,received_us,submitted_us,scheduled_us,packet_latency_us,arrival_latency_us\n");
    for (int i = 0; i < frames; i++) {
        Row *r = &rows[i];
        int64_t due = fps ? (int64_t)r->sequence * 1000000 / fps : 0;
        fprintf(csv, "%d,%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%d,%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                i, r->sequence, r->pts, r->best, r->dts, r->duration,
                r->width, r->height, r->format, r->received, r->submitted, due,
                r->received - r->submitted, r->received - due);
    }
    fprintf(summary, "{\"codec\":\"%s\",\"lavc\":%u,\"threads\":%d,\"fps\":%d,\"async\":%d,\"verify\":%d,\"capacity\":%d,\"frames\":%d,\"time_base_num\":%d,\"time_base_den\":%d,\"elapsed_us\":%" PRId64 ",\"cpu_us\":%" PRId64 ",\"first_us\":%" PRId64 ",\"eof_at_us\":%" PRId64 ",\"max_send_us\":%" PRId64 ",\"max_receive_us\":%" PRId64 ",\"send_eagain\":%" PRId64 ",\"peak_inflight\":%d,\"peak_active_workers\":%d}\n",
            codec->name, avcodec_version(), threads, fps, asynchronous, verify, atoi(argv[6]),
            frames, tb.num, tb.den, elapsed, cpu_elapsed, rows[0].received, eof_at,
            max_send, max_receive, eagain, peak_inflight, peak_workers);
    ret = 0;
end:
    if (ret < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret, error, sizeof(error));
        fprintf(stderr, "load_probe: %s\n", error);
    }
#ifdef LIVE_ASYNC
    avcodec_async_free(&owner);
#endif
    avcodec_free_context(&ctx); avformat_close_input(&format);
    for (int i = 0; i < count; i++) av_packet_free(&packets[i]);
    av_free(packets); av_free(rows); av_packet_free(&packet); av_frame_free(&frame);
    if (fd >= 0) close(fd);
    if (csv) fclose(csv);
    if (summary) fclose(summary);
    if (hashes) fclose(hashes);
    return ret < 0 ? 1 : 0;
}

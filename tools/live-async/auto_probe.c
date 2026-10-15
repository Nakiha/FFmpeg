/* Linux multi-owner / multi-process / adaptive-pressure probe. LGPL-2.1-or-later. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#include "libavcodec/async.h"
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libavutil/sha.h"
#include "libavutil/time.h"

typedef struct Stamp { int sequence; int64_t submitted; } Stamp;
typedef struct Output {
    int phase, sequence, width, height, format;
    int64_t pts, best, dts, duration, received, submitted, scheduled;
} Output;
typedef struct Pass { int fps, frames; int64_t began, first, elapsed; } Pass;
typedef struct Owner {
    AVCodecAsyncContext *codec;
    AVPacket **packets;
    AVFrame *frame;
    Output *rows;
    Pass *passes;
    FILE *hash;
    int phase, next, frames, total, closed, done;
} Owner;
typedef struct Trace { int64_t time; int stream, phase; AVCodecAsyncAutoStats stats; } Trace;
static atomic_int gate_entered;

static int64_t cpu_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * INT64_C(1000000) + ts.tv_nsec / 1000;
}
static void notify(void *opaque)
{
    uint64_t one = 1;
    ssize_t ignored = write(*(int *)opaque, &one, sizeof(one));
    (void)ignored;
}
static int gated_buffer(AVCodecContext *ctx, AVFrame *frame, int flags)
{
    unsigned delay = *(unsigned *)ctx->opaque;
    if (!atomic_exchange(&gate_entered, 1)) {
        puts("GATE_ENTERED"); fflush(stdout);
        av_usleep(delay);
    }
    return avcodec_default_get_buffer2(ctx, frame, flags);
}
static int digest(FILE *out, const AVFrame *frame)
{
    int size = av_image_get_buffer_size(frame->format, frame->width, frame->height, 1), ret;
    uint8_t *data = size > 0 ? av_malloc(size) : NULL, hash[32];
    struct AVSHA *sha = av_sha_alloc();
    if (!data || !sha) { av_free(data); av_free(sha); return AVERROR(ENOMEM); }
    ret = av_image_copy_to_buffer(data, size, (const uint8_t *const *)frame->data,
                                 frame->linesize, frame->format, frame->width, frame->height, 1);
    if (ret >= 0) {
        av_sha_init(sha, 256); av_sha_update(sha, data, size); av_sha_final(sha, hash);
        fprintf(out, "%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%d,%d,%d,%d,%d,%d,%d,%d,",
            frame->pts, frame->best_effort_timestamp, frame->pkt_dts, frame->duration,
            frame->width, frame->height, frame->format, frame->sample_aspect_ratio.num,
            frame->sample_aspect_ratio.den, frame->color_range, frame->color_primaries,
            frame->color_trc, frame->colorspace);
        for (int i = 0; i < 32; i++) fprintf(out, "%02x", hash[i]);
        fputc('\n', out);
    }
    av_free(data); av_free(sha);
    return ret < 0 ? ret : 0;
}

int main(int argc, char **argv)
{
    AVFormatContext *format = NULL;
    const AVCodec *decoder = NULL;
    AVPacket *read_packet = NULL, **input = NULL;
    AVCodecAsyncBudget *budget = NULL;
    Owner *owners = NULL;
    Trace *trace = NULL;
    int rates[64], phases = 0, count = 0, stream, ret = 0, fd = -1;
    int threads, minimum, slots, automatic, streams, verify, traced = 0;
    unsigned delay;
    int64_t began, cpu_began, elapsed, cpu_elapsed, next_trace, max_send = 0, max_receive = 0;
    int pause_ms = getenv("PROBE_HOLD_READ_MS") ? atoi(getenv("PROBE_HOLD_READ_MS")) : 0;
    char path[2048], *rate_copy = NULL, *token;
    if (argc < 10 || argc > 12) {
        fprintf(stderr, "Usage: %s INPUT THREADS MIN SLOTS GROUP|-|default auto|fixed RATES STREAMS PREFIX [VERIFY] [GATE_US]\n", argv[0]);
        return 2;
    }
    threads = atoi(argv[2]); minimum = atoi(argv[3]); slots = atoi(argv[4]);
    automatic = !strcmp(argv[6], "auto"); streams = atoi(argv[8]);
    verify = argc > 10 ? atoi(argv[10]) : 0; delay = argc > 11 ? (unsigned)atoi(argv[11]) : 0;
    if (threads < 2 || threads > 12 || streams < 1 || streams > 8) return 2;
    rate_copy = av_strdup(argv[7]);
    if (!rate_copy) return 1;
    token = strtok(rate_copy, ",");
    while (token && phases < 64) { rates[phases++] = atoi(token); token = strtok(NULL, ","); }
    if (!phases || token) { ret = AVERROR(EINVAL); goto end; }
    for (int i = 0; i < phases; i++) if (rates[i] < 0 || rates[i] > 1000) { ret = AVERROR(EINVAL); goto end; }
    av_log_set_level(AV_LOG_ERROR);
    if ((ret = avformat_open_input(&format, argv[1], NULL, NULL)) < 0 ||
        (ret = avformat_find_stream_info(format, NULL)) < 0) goto end;
    stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0) { ret = stream; goto end; }
    read_packet = av_packet_alloc();
    if (!read_packet) { ret = AVERROR(ENOMEM); goto end; }
    while ((ret = av_read_frame(format, read_packet)) >= 0) {
        if (read_packet->stream_index == stream) {
            AVPacket **grown = av_realloc_array(input, count + 1, sizeof(*input));
            if (!grown) { ret = AVERROR(ENOMEM); goto end; }
            input = grown; input[count] = av_packet_clone(read_packet);
            if (!input[count]) { ret = AVERROR(ENOMEM); goto end; }
            count++;
        }
        av_packet_unref(read_packet);
    }
    if (ret != AVERROR_EOF || !count) { ret = AVERROR_INVALIDDATA; goto end; }
    fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) { ret = AVERROR(errno); goto end; }
    if (strcmp(argv[5], "default") &&
        (ret = avcodec_async_budget_create(&budget, slots, !strcmp(argv[5], "-") ? NULL : argv[5])) < 0) goto end;
    owners = av_calloc(streams, sizeof(*owners));
    trace = av_calloc(32768, sizeof(*trace));
    if (!owners || !trace) { ret = AVERROR(ENOMEM); goto end; }
    for (int i = 0; i < streams; i++) {
        Owner *s = &owners[i];
        AVCodecContext *ctx = avcodec_alloc_context3(decoder);
        if (!ctx) { ret = AVERROR(ENOMEM); goto end; }
        ret = avcodec_parameters_to_context(ctx, format->streams[stream]->codecpar);
        if (ret >= 0) {
            ctx->pkt_timebase = format->streams[stream]->time_base;
            ctx->thread_count = threads; ctx->thread_type = FF_THREAD_FRAME;
            ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
            if (delay) { ctx->get_buffer2 = gated_buffer; ctx->opaque = &delay; }
            ret = avcodec_open2(ctx, decoder, NULL);
        }
        if (ret >= 0) ret = avcodec_async_create(&s->codec, &ctx, 12, notify, &fd);
        avcodec_free_context(&ctx);
        if (ret < 0) goto end;
        if (automatic) {
            AVCodecAsyncBudget *separate = NULL, *chosen = budget;
            if (getenv("PROBE_SEPARATE_HANDLES")) {
                ret = avcodec_async_budget_create(&separate, slots, argv[5]);
                if (ret < 0) goto end;
                chosen = separate;
            }
            if (getenv("PROBE_CHECK_API") &&
                avcodec_async_enable_auto(s->codec, threads + 1, chosen) != AVERROR(EINVAL)) {
                avcodec_async_budget_free(&separate); ret = AVERROR_INVALIDDATA; goto end;
            }
            ret = avcodec_async_enable_auto(s->codec, minimum, chosen);
            avcodec_async_budget_free(&separate);
            if (ret < 0) goto end;
            if (getenv("PROBE_CHECK_API") &&
                avcodec_async_enable_auto(s->codec, minimum, budget) != AVERROR(EINVAL)) {
                ret = AVERROR_INVALIDDATA; goto end;
            }
        }
        s->frame = av_frame_alloc(); s->packets = av_calloc(count * phases, sizeof(*s->packets));
        s->rows = av_calloc(count * phases, sizeof(*s->rows)); s->passes = av_calloc(phases, sizeof(*s->passes));
        if (!s->frame || !s->packets || !s->rows || !s->passes) { ret = AVERROR(ENOMEM); goto end; }
        for (int j = 0; j < count * phases; j++) {
            s->packets[j] = av_packet_clone(input[j % count]);
            if (!s->packets[j]) { ret = AVERROR(ENOMEM); goto end; }
            av_buffer_unref(&s->packets[j]->opaque_ref);
            s->packets[j]->opaque_ref = av_buffer_allocz(sizeof(Stamp));
            if (!s->packets[j]->opaque_ref) { ret = AVERROR(ENOMEM); goto end; }
            ((Stamp *)s->packets[j]->opaque_ref->data)->sequence = j;
        }
        if (verify) {
            snprintf(path, sizeof(path), "%s-s%d.sha256", argv[9], i); s->hash = fopen(path, "w");
            if (!s->hash) { ret = AVERROR(EIO); goto end; }
        }
    }
    /* Owners must retain the budget after the application's handle is freed. */
    avcodec_async_budget_free(&budget);
    cpu_began = cpu_us(); began = next_trace = av_gettime_relative();
    for (int i = 0; i < streams; i++) owners[i].passes[0].began = began;
    for (;;) {
        int progressed = 0, finished = 0;
        int64_t now = av_gettime_relative(), wait = 20000;
        if (now - began > 180000000) { ret = AVERROR(ETIMEDOUT); goto end; }
        for (int i = 0; i < streams; i++) {
            Owner *s = &owners[i];
            Pass *pass;
            if (s->done) { finished++; continue; }
            pass = &s->passes[s->phase]; pass->fps = rates[s->phase];
            if (s->next < count) {
                int64_t due = pass->began + (pass->fps ? (int64_t)s->next * 1000000 / pass->fps : 0);
                if (now >= due) {
                    AVPacket *packet = s->packets[s->phase * count + s->next];
                    int64_t call, duration;
                    ((Stamp *)packet->opaque_ref->data)->submitted = av_gettime_relative();
                    call = av_gettime_relative(); ret = avcodec_async_send_packet(s->codec, packet);
                    duration = av_gettime_relative() - call;
                    if (duration > max_send) max_send = duration;
                    if (!ret) { s->next++; progressed = 1; }
                    else if (ret != AVERROR(EAGAIN)) goto end;
                } else if (due - now < wait) wait = due - now;
            }
            if (s->next == count && !s->closed) {
                ret = avcodec_async_send_packet(s->codec, NULL);
                if (!ret) { s->closed = 1; progressed = 1; }
                else if (ret != AVERROR(EAGAIN)) goto end;
            }
            if (now - began < (int64_t)pause_ms * 1000) continue;
            for (;;) {
                int64_t call = av_gettime_relative(), duration;
                const Stamp *stamp;
                ret = avcodec_async_receive_frame(s->codec, s->frame);
                now = av_gettime_relative(); duration = now - call;
                if (duration > max_receive) max_receive = duration;
                if (ret == AVERROR(EAGAIN)) break;
                if (ret == AVERROR_EOF) {
                    progressed = 1;
                    if (s->frames != count) { ret = AVERROR_INVALIDDATA; goto end; }
                    pass->frames = s->frames; pass->elapsed = now - pass->began;
                    if (++s->phase == phases) { s->done = 1; break; }
                    if ((ret = avcodec_async_flush(s->codec)) < 0) goto end;
                    s->passes[s->phase].began = av_gettime_relative();
                    s->next = s->frames = s->closed = 0; progressed = 1;
                    break;
                }
                if (ret < 0) goto end;
                if (!s->frame->opaque_ref || s->frame->opaque_ref->size != sizeof(Stamp) || s->frames >= count) {
                    ret = AVERROR_INVALIDDATA; goto end;
                }
                stamp = (const Stamp *)s->frame->opaque_ref->data;
                if (stamp->sequence / count != s->phase) { ret = AVERROR_INVALIDDATA; goto end; }
                if (!s->frames) pass->first = now - pass->began;
                s->rows[s->total++] = (Output){ s->phase, stamp->sequence, s->frame->width,
                    s->frame->height, s->frame->format, s->frame->pts, s->frame->best_effort_timestamp,
                    s->frame->pkt_dts, s->frame->duration, now - began, stamp->submitted - began,
                    pass->began - began + (pass->fps ? (int64_t)(stamp->sequence % count) * 1000000 / pass->fps : 0) };
                if (verify && (ret = digest(s->hash, s->frame)) < 0) goto end;
                av_frame_unref(s->frame); s->frames++; progressed = 1;
            }
        }
        now = av_gettime_relative();
        if (now >= next_trace) {
            for (int i = 0; i < streams; i++) {
                if (traced >= 32768) { ret = AVERROR(ENOSPC); goto end; }
                Trace *t = &trace[traced++]; t->time = now - began; t->stream = i; t->phase = owners[i].phase;
                avcodec_async_get_auto_stats(owners[i].codec, &t->stats);
            }
            next_trace = now + 50000;
        }
        if (finished == streams) break;
        if (!progressed) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            if (poll(&pfd, 1, (int)((wait + 999) / 1000)) > 0) {
                uint64_t value; ssize_t ignored = read(fd, &value, sizeof(value)); (void)ignored;
            }
        }
    }
    elapsed = av_gettime_relative() - began; cpu_elapsed = cpu_us() - cpu_began;
    snprintf(path, sizeof(path), "%s.control.csv", argv[9]);
    FILE *control = fopen(path, "w");
    if (!control) { ret = AVERROR(EIO); goto end; }
    fprintf(control, "time_us,stream,phase,target,peak_target,busy,leased,input,output,increases,decreases,rollbacks,budget_waits,completed\n");
    for (int i = 0; i < traced; i++) {
        Trace *t = &trace[i]; AVCodecAsyncAutoStats *a = &t->stats;
        fprintf(control, "%" PRId64 ",%d,%d,%d,%d,%d,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
            t->time, t->stream, t->phase, a->target, a->peak_target, a->busy, a->leased,
            a->input_queue, a->output_queue, a->increases, a->decreases, a->probe_rollbacks, a->budget_waits, a->completed);
    }
    fclose(control);
    for (int i = 0; i < streams; i++) {
        Owner *s = &owners[i];
        snprintf(path, sizeof(path), "%s-s%d.csv", argv[9], i); FILE *out = fopen(path, "w");
        if (!out) { ret = AVERROR(EIO); goto end; }
        fprintf(out, "phase,sequence,pts,best_effort,dts,duration,width,height,format,received_us,submitted_us,scheduled_us,packet_latency_us,arrival_latency_us\n");
        for (int j = 0; j < s->total; j++) {
            Output *r = &s->rows[j];
            fprintf(out, "%d,%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%d,%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                r->phase, r->sequence, r->pts, r->best, r->dts, r->duration, r->width, r->height, r->format,
                r->received, r->submitted, r->scheduled, r->received - r->submitted, r->received - r->scheduled);
        }
        fclose(out);
        snprintf(path, sizeof(path), "%s-s%d.passes.csv", argv[9], i); out = fopen(path, "w");
        if (!out) { ret = AVERROR(EIO); goto end; }
        fprintf(out, "phase,fps,frames,first_us,elapsed_us\n");
        for (int j = 0; j < phases; j++) {
            Pass *p = &s->passes[j];
            fprintf(out, "%d,%d,%d,%" PRId64 ",%" PRId64 "\n", j, p->fps, p->frames, p->first, p->elapsed);
        }
        fclose(out);
    }
    snprintf(path, sizeof(path), "%s.json", argv[9]); FILE *summary = fopen(path, "w");
    if (!summary) { ret = AVERROR(EIO); goto end; }
    fprintf(summary, "{\"frames\":%d,\"streams\":%d,\"phases\":%d,\"verify\":%d,\"auto\":%d,\"slots\":%d,\"elapsed_us\":%" PRId64 ",\"cpu_us\":%" PRId64 ",\"max_send_us\":%" PRId64 ",\"max_receive_us\":%" PRId64 "}\n",
        streams * phases * count, streams, phases, verify, automatic, slots, elapsed, cpu_elapsed, max_send, max_receive);
    fclose(summary); ret = 0;
end:
    if (ret < 0) { char error[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret, error, sizeof(error)); fprintf(stderr, "auto_probe: %s\n", error); }
    if (owners) for (int i = 0; i < streams; i++) {
        Owner *s = &owners[i]; avcodec_async_free(&s->codec); av_frame_free(&s->frame);
        if (s->packets) for (int j = 0; j < count * phases; j++) av_packet_free(&s->packets[j]);
        if (s->hash) fclose(s->hash);
        av_free(s->packets); av_free(s->rows); av_free(s->passes);
    }
    avcodec_async_budget_free(&budget);
    for (int i = 0; i < count; i++) av_packet_free(&input[i]);
    av_free(input); av_free(owners); av_free(trace); av_free(rate_copy);
    av_packet_free(&read_packet); avformat_close_input(&format); if (fd >= 0) close(fd);
    return ret < 0 ? 1 : 0;
}

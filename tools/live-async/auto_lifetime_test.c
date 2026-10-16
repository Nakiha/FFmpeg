/* Concurrent default-budget creation / active-close stress. Linux test only. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "libavcodec/async.h"
#include "libavformat/avformat.h"
#include "libavutil/time.h"

typedef struct Input { const AVCodec *decoder; const AVCodecParameters *parameters; const AVPacket *packet; } Input;
static atomic_int failed;
static atomic_int gate_closed, gate_entered;

static int gated_buffer(AVCodecContext *ctx, AVFrame *frame, int flags)
{
    atomic_store(&gate_entered, 1);
    while (atomic_load(&gate_closed)) av_usleep(1000);
    return avcodec_default_get_buffer2(ctx, frame, flags);
}

/* Force an owner to prefetch a packet but fail budget acquisition. Flushing
 * that owner must discard the prefetched packet as well as its public queues. */
static int flush_budget_waiter(const Input *input)
{
    AVCodecAsyncBudget *budget = NULL;
    AVCodecAsyncContext *owners[2] = { NULL, NULL };
    AVCodecContext *ctx = NULL;
    AVFrame *frame = av_frame_alloc();
    AVCodecAsyncAutoStats stats;
    int ret = AVERROR_BUG;
    int64_t deadline;
    atomic_store(&gate_closed, 1);
    atomic_store(&gate_entered, 0);
    if (!frame) { ret = AVERROR(ENOMEM); goto end; }
    if ((ret = avcodec_async_budget_create(&budget, 1, NULL)) < 0) goto end;
    for (int i = 0; i < 2; i++) {
        ctx = avcodec_alloc_context3(input->decoder);
        if (!ctx) { ret = AVERROR(ENOMEM); goto end; }
        if ((ret = avcodec_parameters_to_context(ctx, input->parameters)) < 0) goto end;
        ctx->thread_count = 2; ctx->thread_type = FF_THREAD_FRAME;
        if (!i) ctx->get_buffer2 = gated_buffer;
        if ((ret = avcodec_open2(ctx, input->decoder, NULL)) < 0 ||
            (ret = avcodec_async_create(&owners[i], &ctx, 1, NULL, NULL)) < 0 ||
            (ret = avcodec_async_enable_auto(owners[i], 1, budget)) < 0) goto end;
    }
    if ((ret = avcodec_async_send_packet(owners[0], input->packet)) < 0) goto end;
    deadline = av_gettime_relative() + 3000000;
    while (!atomic_load(&gate_entered) && av_gettime_relative() < deadline) av_usleep(1000);
    if (!atomic_load(&gate_entered)) { ret = AVERROR_BUG; goto end; }
    if ((ret = avcodec_async_send_packet(owners[1], input->packet)) < 0) goto end;
    deadline = av_gettime_relative() + 3000000;
    do {
        avcodec_async_get_auto_stats(owners[1], &stats);
        if (stats.budget_waits) break;
        av_usleep(1000);
    } while (av_gettime_relative() < deadline);
    if (!stats.budget_waits) { ret = AVERROR_BUG; goto end; }
    if ((ret = avcodec_async_flush(owners[1])) < 0) goto end;
    atomic_store(&gate_closed, 0);
    avcodec_async_free(&owners[0]);
    if ((ret = avcodec_async_send_packet(owners[1], NULL)) < 0) goto end;
    deadline = av_gettime_relative() + 3000000;
    do {
        ret = avcodec_async_receive_frame(owners[1], frame);
        if (ret != AVERROR(EAGAIN)) break;
        av_usleep(1000);
    } while (av_gettime_relative() < deadline);
    if (ret != AVERROR_EOF)
        fprintf(stderr, "Budget waiter drain after flush: %d (expected EOF, zero means a stale frame)\n", ret);
    ret = ret == AVERROR_EOF ? 0 : AVERROR_BUG;
end:
    atomic_store(&gate_closed, 0);
    avcodec_async_free(&owners[0]); avcodec_async_free(&owners[1]);
    avcodec_async_budget_free(&budget); avcodec_free_context(&ctx);
    av_frame_free(&frame);
    if (ret >= 0) puts("PASS shared-budget waiter flush discards prefetched packet before empty drain");
    return ret;
}

static void *run(void *opaque)
{
    const Input *input = opaque;
    for (int i = 0; i < 20; i++) {
        AVCodecContext *ctx = avcodec_alloc_context3(input->decoder);
        AVCodecAsyncContext *owner = NULL;
        int ret = ctx ? avcodec_parameters_to_context(ctx, input->parameters) : AVERROR(ENOMEM);
        if (ret >= 0) {
            ctx->thread_count = 2; ctx->thread_type = FF_THREAD_FRAME;
            ret = avcodec_open2(ctx, input->decoder, NULL);
        }
        if (ret >= 0) ret = avcodec_async_create(&owner, &ctx, 1, NULL, NULL);
        if (ret >= 0) ret = avcodec_async_enable_auto(owner, 1, NULL);
        if (ret >= 0) ret = avcodec_async_send_packet(owner, input->packet);
        if (i % 2) av_usleep(1000);
        avcodec_async_free(&owner); avcodec_free_context(&ctx);
        if (ret < 0) { atomic_store(&failed, 1); break; }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    AVFormatContext *format = NULL;
    AVPacket *packet = av_packet_alloc();
    Input input = { 0 };
    pthread_t threads[8];
    int ret, stream, started = 0;
    if (argc != 2 || !packet) return 2;
    av_log_set_level(AV_LOG_ERROR);
    if ((ret = avformat_open_input(&format, argv[1], NULL, NULL)) < 0 ||
        (ret = avformat_find_stream_info(format, NULL)) < 0) goto end;
    stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &input.decoder, 0);
    if (stream < 0) { ret = stream; goto end; }
    while ((ret = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == stream) break;
        av_packet_unref(packet);
    }
    if (ret < 0) goto end;
    input.parameters = format->streams[stream]->codecpar; input.packet = packet;
    if ((ret = flush_budget_waiter(&input)) < 0) goto end;
    for (; started < 8; started++) {
        ret = pthread_create(&threads[started], NULL, run, &input);
        if (ret) { atomic_store(&failed, 1); break; }
    }
    for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
    ret = atomic_load(&failed) ? AVERROR_UNKNOWN : 0;
    if (ret >= 0) puts("PASS 8 concurrent callers, 160 default-budget owner create/active-close lifetimes");
end:
    av_packet_free(&packet); avformat_close_input(&format);
    return ret < 0 ? 1 : 0;
}

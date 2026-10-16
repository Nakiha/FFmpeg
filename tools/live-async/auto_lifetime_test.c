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

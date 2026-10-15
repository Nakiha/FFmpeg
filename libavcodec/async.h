/* Live asynchronous software decoding extension, LGPL-2.1-or-later. */
#ifndef AVCODEC_ASYNC_H
#define AVCODEC_ASYNC_H

#include <stdint.h>
#include "avcodec.h"

#define AVCODEC_ASYNC_API_VERSION 1

/** Opaque owner of a decoder and a background coordinator thread. */
typedef struct AVCodecAsyncContext AVCodecAsyncContext;

typedef struct AVCodecAsyncStats {
    uint64_t packets_accepted;
    uint64_t frames_received;
    int peak_inflight;       /**< Submitted, uncollected frame tasks. */
    int peak_active_workers; /**< Overlapping decoder callbacks, including waits. */
} AVCodecAsyncStats;

/**
 * Transfer a freshly opened native software h264/hevc frame-threaded decoder
 * into an asynchronous owner. On success *decoder becomes NULL. On failure
 * ownership is retained. Requires thread_count > 1 and default get_format.
 * queue_capacity (1..1024) bounds input packets AND output frames separately;
 * Codec tasks, codec reference buffers and coordinator staging are additional.
 * notify may be NULL (polling); otherwise it runs on the coordinator thread,
 * outside queue locks, for progress/output/terminal state. It must only post a
 * wakeup, must not block or re-enter any API, and is quiesced before free returns.
 * Do not access the transferred decoder. Serialize all frontend API calls.
 * Data calls do not wait for decoding: they take short queue locks and acquire
 * references. This is not a hard real-time allocation/scheduling guarantee.
 * This fork-specific API is not part of upstream FFmpeg 7.0.3.
 */
int avcodec_async_create(AVCodecAsyncContext **out, AVCodecContext **decoder,
                         int queue_capacity, void (*notify)(void *), void *opaque);

/**
 * Accept a complete positive-size reference-counted packet (packet->buf != NULL)
 * by reference, or NULL to close input. Non-reference-counted packets are rejected
 * to avoid copying their payload on the calling thread.
 * 0: accepted; EAGAIN: queue full, NOT accepted, retain and retry on notification.
 * EOF: input already closed. Other errors persist until flush.
 */
int avcodec_async_send_packet(AVCodecAsyncContext *ctx, const AVPacket *packet);

/**
 * Unrefs frame, then returns 0 and moves an ordered result into it, EAGAIN if
 * no output is available NOW, or EOF after complete drain. EAGAIN can occur
 * during drain and on both send/receive: wait for progress, do not spin.
 * EOF and errors are reported after already queued output has been consumed.
 */
int avcodec_async_receive_frame(AVCodecAsyncContext *ctx, AVFrame *frame);

/** Discard queued/inflight work and reset the decoder; this lifecycle call waits. */
int avcodec_async_flush(AVCodecAsyncContext *ctx);
/** Lifetime counters; flush does not reset them or the peak measurements. */
int avcodec_async_get_stats(AVCodecAsyncContext *ctx, AVCodecAsyncStats *stats);
/** Join coordinator and decoder workers, destroy decoder, and clear *ctx; waits. */
void avcodec_async_free(AVCodecAsyncContext **ctx);

#endif

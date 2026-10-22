/* Live asynchronous software decoding extension, LGPL-2.1-or-later. */
#ifndef AVCODEC_ASYNC_H
#define AVCODEC_ASYNC_H

#include <stdint.h>
#include "avcodec.h"
#include "early_output.h"

#define AVCODEC_ASYNC_API_VERSION 2

/** Opaque owner of a decoder and a background coordinator thread. */
typedef struct AVCodecAsyncContext AVCodecAsyncContext;
typedef struct AVCodecAsyncBudget AVCodecAsyncBudget;

typedef struct AVCodecAsyncStats {
    uint64_t packets_accepted;
    uint64_t frames_received;
    int peak_inflight;       /**< Submitted, uncollected frame tasks. */
    int peak_active_workers; /**< Overlapping decoder callbacks, including waits. */
} AVCodecAsyncStats;

typedef struct AVCodecAsyncAutoStats {
    int enabled, minimum, maximum, target, peak_target, budget_slots;
    int inflight, busy, leased, input_queue, output_queue;
    uint64_t increases, decreases, probe_rollbacks, budget_waits, completed;
} AVCodecAsyncAutoStats;

/**
 * Allocate a cooperative unfinished-task budget, with 1..256 slots (0 chooses
 * about 3/4 of av_cpu_count(), at least one). NULL name is process-local; share
 * this handle among owners. Named groups are Linux-only, same UID and /dev/shm
 * namespace, name [A-Za-z0-9_-]{1,64}. All joiners must use the same capacity.
 * Named slot locks are released by the kernel on process death. Their files
 * persist: never unlink a live group's files, or fork and retain live handles.
 * Capacity covers participating decoders, not filter/encode or other programs.
 * Lifecycle call: may allocate/open/lock metadata. Not a frontend data call.
 */
int avcodec_async_budget_create(AVCodecAsyncBudget **out, int slots, const char *name);
void avcodec_async_budget_free(AVCodecAsyncBudget **budget);

/**
 * Opt into experimental pressure-based dispatch control BEFORE accepting input,
 * once per owner. minimum=0 defaults to 2; otherwise 1..allocated thread_count.
 * Allocation never changes. Target bounds are clamped to the budget capacity.
 * NULL budget uses
 * one process-wide default; explicit budgets are retained by reference.
 * Budget waits occur only on the coordinator. Growth uses queue pressure and
 * completion-rate trials; low demand shrinks slowly. No hard latency, optimal
 * throughput or cross-process fairness guarantee; queues/EAGAIN remain bounded.
 */
int avcodec_async_enable_auto(AVCodecAsyncContext *ctx, int minimum,
                              AVCodecAsyncBudget *budget);
/** Approximate coordinator snapshots; busy includes setup/reference waits. */
int avcodec_async_get_auto_stats(AVCodecAsyncContext *ctx, AVCodecAsyncAutoStats *stats);

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
int avcodec_async_get_early_output_stats(AVCodecAsyncContext *ctx, AVCodecEarlyOutputStats *stats);
int avcodec_async_get_stats(AVCodecAsyncContext *ctx, AVCodecAsyncStats *stats);
/** Join coordinator and decoder workers, destroy decoder, and clear *ctx; waits. */
void avcodec_async_free(AVCodecAsyncContext **ctx);

#endif

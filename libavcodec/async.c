/* Live asynchronous software decoding extension, LGPL-2.1-or-later. */
#include "config.h"
#include "live_trace_internal.h"
#include <string.h>
#include <time.h>
#include "async.h"
#include "async_budget.h"
#include "decode.h"
#include "internal.h"
#include "thread.h"
#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#if HAVE_THREADS
struct AVCodecAsyncContext {
    AVCodecContext *decoder;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    AVPacket **input;
    AVFrame **output;
    AVPacket *work_packet;
    AVFrame *work_frame;
    int capacity, input_head, input_count, output_head, output_count;
    int stop, flush_requested, input_closed, terminal;
    uint64_t generation;
    uint64_t packets_accepted, frames_received;
    void (*notify)(void *);
    void *opaque;
    int threads, auto_pending, minimum, maximum, target, step, peak_window;
    int probe_previous, probe_constrained, probe_continuous, budget_waiting;
    int *leases, lease_count;
    int64_t next_tick, window_start, idle_since, cooldown, probe_start;
    uint64_t window_completed, probe_completed, budget_waits;
    double baseline_rate;
    AVCodecAsyncBudget *budget;
    AVCodecAsyncAutoStats auto_stats;
};

static void reconcile_budget(AVCodecAsyncContext *s, int busy)
{
    /* The coordinator is the only submitter. Worker states can only move toward
     * READY during the scan, so retaining busy slots cannot undercount work. */
    av_assert0(busy <= s->lease_count);
    while (s->lease_count > busy)
        ff_async_budget_release(s->budget, s->leases[--s->lease_count]);
}

static int acquire_budget(void *opaque)
{
    AVCodecAsyncContext *s = opaque;
    FFThreadAsyncStatus status;
    int ticket;
    ff_thread_async_status(s->decoder, &status);
    reconcile_budget(s, status.busy);
    ticket = ff_async_budget_take(s->budget);
    if (ticket == AVERROR(EAGAIN)) {
        s->budget_waits++;
        s->budget_waiting = s->probe_constrained = 1;
        return 0;
    }
    if (ticket < 0)
        return ticket;
    av_assert0(s->lease_count < s->threads);
    s->leases[s->lease_count++] = ticket;
    s->budget_waiting = 0;
    return 1;
}

static void auto_tick(AVCodecAsyncContext *s, int have_packet)
{
    FFThreadAsyncStatus status;
    int pressure, space, changed = 0, old_target;
    int64_t now = av_gettime_relative();
    ff_thread_async_status(s->decoder, &status);
    reconcile_budget(s, status.busy);
    s->peak_window = FFMAX(s->peak_window, status.pending);
    pthread_mutex_lock(&s->mutex);
    space = s->output_count < s->capacity;
    /* A single newly arrived packet is normal paced input, not sustained
     * saturation. Otherwise every periodic arrival resets the shrink timer. */
    pressure = space && !s->terminal &&
               (s->input_count >= 2 ||
                ((s->input_count || have_packet) && status.pending >= s->target));
    if (!pressure) s->probe_continuous = 0;
    s->auto_stats.inflight = status.pending;
    s->auto_stats.busy = status.busy;
    s->auto_stats.leased = s->lease_count;
    s->auto_stats.completed = status.completed;
    s->auto_stats.budget_waits = s->budget_waits;
    if (now < s->next_tick) { pthread_mutex_unlock(&s->mutex); return; }
    old_target = s->target;
    if (s->probe_start && now - s->probe_start >= 250000) {
        double rate = (status.completed - s->probe_completed) * 1000000.0 / (now - s->probe_start);
        if (s->probe_continuous && !s->probe_constrained &&
            s->baseline_rate > 0 && rate < s->baseline_rate * 1.03) {
            s->target = s->probe_previous;
            s->auto_stats.probe_rollbacks++;
            s->auto_stats.decreases++;
            s->step = FFMIN(s->maximum, s->step + 2);
            s->cooldown = now + 600000;
        } else {
            s->step = 2;
            s->cooldown = now + 200000;
        }
        s->probe_start = 0;
    } else if (!s->probe_start && pressure && !s->budget_waiting &&
               s->peak_window >= s->target && s->target < s->maximum && now >= s->cooldown) {
        double rate = (status.completed - s->window_completed) * 1000000.0 / (now - s->window_start);
        if (rate > 0) {
            s->baseline_rate = rate;
            s->probe_previous = s->target;
            s->probe_start = now;
            s->probe_completed = status.completed;
            s->probe_continuous = 1;
            s->probe_constrained = 0;
            s->target = FFMIN(s->maximum, s->target + s->step);
            s->auto_stats.increases++;
        }
    }
    if (!pressure && space && s->peak_window <= FFMAX(s->minimum, s->target / 2)) {
        if (!s->idle_since) s->idle_since = now;
        if (!s->probe_start && s->target > s->minimum && now - s->idle_since >= 1000000) {
            s->target = FFMAX(s->minimum, s->target - 2);
            s->auto_stats.decreases++;
            s->idle_since = now;
            s->step = 2;
            s->cooldown = now + 200000;
        }
    } else s->idle_since = 0;
    changed = s->target != old_target;
    s->auto_stats.target = s->target;
    s->auto_stats.peak_target = FFMAX(s->auto_stats.peak_target, s->target);
    s->window_start = now;
    s->window_completed = status.completed;
    s->next_tick = now + 100000;
    s->peak_window = 0;
    pthread_mutex_unlock(&s->mutex);
    if (changed)
        ff_thread_async_control(s->decoder, s->target, acquire_budget);
}

static void wake_locked(AVCodecAsyncContext *s)
{
    s->generation++;
    pthread_cond_signal(&s->cond);
}

static void codec_progress(void *opaque)
{
    AVCodecAsyncContext *s = opaque;
    pthread_mutex_lock(&s->mutex);
    wake_locked(s);
    pthread_mutex_unlock(&s->mutex);
}

static void notify_frontend(AVCodecAsyncContext *s)
{
    if (s->notify)
        s->notify(s->opaque);
}

static void clear_queues(AVCodecAsyncContext *s)
{
    int i;
    for (i = 0; i < s->capacity; i++) {
        av_packet_unref(s->input[i]);
        av_frame_unref(s->output[i]);
    }
    s->input_head = s->input_count = s->output_head = s->output_count = 0;
}

static void set_terminal(AVCodecAsyncContext *s, int error)
{
    pthread_mutex_lock(&s->mutex);
    s->terminal = error;
    pthread_mutex_unlock(&s->mutex);
    notify_frontend(s);
}

static void *async_coordinator(void *opaque)
{
    AVCodecAsyncContext *s = opaque;
    AVPacket *packet = s->work_packet;
    AVFrame *frame = s->work_frame;
    int have_packet = 0;

    for (;;) {
        uint64_t observed;
        int space, terminal, popped = 0, progressed = 0, ret;
        pthread_mutex_lock(&s->mutex);
        observed = s->generation;
        if (s->stop) {
            pthread_mutex_unlock(&s->mutex);
            break;
        }
        if (s->auto_pending) {
            int64_t now = av_gettime_relative();
            s->auto_pending = 0;
            pthread_mutex_unlock(&s->mutex);
            ff_thread_async_control(s->decoder, s->target, acquire_budget);
            s->window_start = now;
            s->next_tick = now + 100000;
            continue;
        }
        if (s->budget) {
            pthread_mutex_unlock(&s->mutex);
            auto_tick(s, have_packet);
            pthread_mutex_lock(&s->mutex);
            observed = s->generation;
            if (s->stop) { pthread_mutex_unlock(&s->mutex); break; }
        }
        if (s->flush_requested) {
            pthread_mutex_unlock(&s->mutex);
            av_packet_unref(packet);
            av_frame_unref(frame);
            have_packet = 0;
            avcodec_flush_buffers(s->decoder);
            s->budget_waiting = 0;
            pthread_mutex_lock(&s->mutex);
            clear_queues(s);
            s->input_closed = s->terminal = s->flush_requested = 0;
            wake_locked(s);
            pthread_cond_broadcast(&s->cond);
            pthread_mutex_unlock(&s->mutex);
            notify_frontend(s);
            continue;
        }
        terminal = s->terminal;
        space = s->output_count < s->capacity;
        if (!terminal && !have_packet && s->input_count && space) {
            av_packet_move_ref(packet, s->input[s->input_head]);
            s->input_head = (s->input_head + 1) % s->capacity;
            s->input_count--;
            have_packet = popped = 1;
        }
        pthread_mutex_unlock(&s->mutex);
        if (popped)
            notify_frontend(s); /* Input capacity became available. */
        if (!terminal && space) {
            if (have_packet) {
                ret = avcodec_send_packet(s->decoder, packet->data ? packet : NULL);
                if (ret == 0) {
                    av_packet_unref(packet);
                    have_packet = 0;
                    progressed = 1;
                } else if (ret != AVERROR(EAGAIN)) {
                    set_terminal(s, ret);
                    continue;
                }
            }
            ret = avcodec_receive_frame(s->decoder, frame);
            if (ret == 0) {
                AVLiveDecodeTrace *tr=ff_live_trace(frame);
                if(tr)FF_LIVE_SET(tr,async_queued_us,av_gettime_relative());
                pthread_mutex_lock(&s->mutex);
                av_frame_move_ref(s->output[(s->output_head + s->output_count) % s->capacity], frame);
                s->output_count++;
                pthread_mutex_unlock(&s->mutex);
                notify_frontend(s);
                progressed = 1;
            } else if (ret != AVERROR(EAGAIN)) {
                set_terminal(s, ret);
                continue;
            }
        }
        if (progressed)
            continue;
        pthread_mutex_lock(&s->mutex);
        /* Progress can arrive while codec calls run outside the queue lock. */
        while (s->generation == observed && !s->stop && !s->flush_requested) {
            if (s->budget && !s->terminal &&
                (s->budget_waiting || s->target > s->minimum || s->input_count || have_packet)) {
                struct timespec until;
                int64_t us = av_gettime() + (s->budget_waiting ? 2000 : 50000);
                until.tv_sec = us / 1000000;
                until.tv_nsec = (us % 1000000) * 1000;
                if (pthread_cond_timedwait(&s->cond, &s->mutex, &until) == ETIMEDOUT)
                    break;
            } else
                pthread_cond_wait(&s->cond, &s->mutex);
        }
        pthread_mutex_unlock(&s->mutex);
    }
    return NULL;
}

int avcodec_async_create(AVCodecAsyncContext **out, AVCodecContext **decoder,
                         int capacity, void (*notify)(void *), void *opaque)
{
    AVCodecAsyncContext *s;
    AVCodecContext *ctx;
    int ret, i;
    if (!out || *out || !decoder || !(ctx = *decoder) || !avcodec_is_open(ctx) ||
        capacity < 1 || capacity > 1024 ||
        !(ctx->active_thread_type & FF_THREAD_FRAME) || ctx->thread_count < 2 ||
        ctx->hw_device_ctx || ctx->hw_frames_ctx || ctx->hwaccel ||
        ctx->get_format != avcodec_default_get_format ||
        (strcmp(ctx->codec->name, "h264") && strcmp(ctx->codec->name, "hevc")) ||
        !ff_decode_async_is_fresh(ctx))
        return AVERROR(EINVAL);
    s = av_mallocz(sizeof(*s));
    if (!s)
        return AVERROR(ENOMEM);
    s->capacity = capacity;
    s->threads = ctx->thread_count;
    s->auto_stats.maximum = s->auto_stats.target = s->auto_stats.peak_target = ctx->thread_count;
    s->decoder = ctx;
    s->notify = notify;
    s->opaque = opaque;
    s->input = av_calloc(capacity, sizeof(*s->input));
    s->output = av_calloc(capacity, sizeof(*s->output));
    s->work_packet = av_packet_alloc();
    s->work_frame = av_frame_alloc();
    if (!s->input || !s->output || !s->work_packet || !s->work_frame) {
        ret = AVERROR(ENOMEM);
        goto fail_alloc;
    }
    for (i = 0; i < capacity; i++) {
        s->input[i] = av_packet_alloc();
        s->output[i] = av_frame_alloc();
        if (!s->input[i] || !s->output[i]) {
            ret = AVERROR(ENOMEM);
            goto fail_alloc;
        }
    }
    ret = pthread_mutex_init(&s->mutex, NULL);
    if (ret) {
        ret = AVERROR(ret);
        goto fail_alloc;
    }
    ret = pthread_cond_init(&s->cond, NULL);
    if (ret) {
        ret = AVERROR(ret);
        goto fail_mutex;
    }
    ret = ff_thread_async_enable(ctx, codec_progress, s);
    if (ret < 0)
        goto fail_cond;
    ret = pthread_create(&s->thread, NULL, async_coordinator, s);
    if (ret) {
        ff_thread_async_enable(ctx, NULL, NULL);
        ret = AVERROR(ret);
        goto fail_cond;
    }
    *decoder = NULL;
    *out = s;
    return 0;
fail_cond:
    pthread_cond_destroy(&s->cond);
fail_mutex:
    pthread_mutex_destroy(&s->mutex);
fail_alloc:
    for (i = 0; i < capacity; i++) {
        if (s->input)
            av_packet_free(&s->input[i]);
        if (s->output)
            av_frame_free(&s->output[i]);
    }
    av_free(s->input);
    av_free(s->output);
    av_packet_free(&s->work_packet);
    av_frame_free(&s->work_frame);
    av_free(s);
    return ret;
}

int avcodec_async_send_packet(AVCodecAsyncContext *s, const AVPacket *packet)
{
    int ret = 0;
    if (!s || (packet && (!packet->buf || !packet->data || packet->size <= 0)))
        return AVERROR(EINVAL);
    pthread_mutex_lock(&s->mutex);
    if (s->terminal)
        ret = s->terminal;
    else if (s->input_closed)
        ret = AVERROR_EOF;
    else if (s->input_count == s->capacity)
        ret = AVERROR(EAGAIN);
    else {
        if (packet)
            ret = av_packet_ref(s->input[(s->input_head + s->input_count) % s->capacity], packet);
        if (ret == 0) {
            s->input_count++;
            if (!packet)
                s->input_closed = 1;
            else
                s->packets_accepted++;
            wake_locked(s);
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return ret;
}

int avcodec_async_receive_frame(AVCodecAsyncContext *s, AVFrame *frame)
{
    int ret = 0;
    if (!s || !frame)
        return AVERROR(EINVAL);
    av_frame_unref(frame);
    pthread_mutex_lock(&s->mutex);
    if (s->output_count) {
        av_frame_move_ref(frame, s->output[s->output_head]);
        AVLiveDecodeTrace *tr=ff_live_trace(frame);
        if(tr)FF_LIVE_SET(tr,frontend_us,av_gettime_relative());
        s->output_head = (s->output_head + 1) % s->capacity;
        s->output_count--;
        s->frames_received++;
        wake_locked(s);
    } else
        ret = s->terminal ? s->terminal : AVERROR(EAGAIN);
    pthread_mutex_unlock(&s->mutex);
    return ret;
}

int avcodec_async_flush(AVCodecAsyncContext *s)
{
    if (!s)
        return AVERROR(EINVAL);
    pthread_mutex_lock(&s->mutex);
    s->flush_requested = 1;
    wake_locked(s);
    while (s->flush_requested)
        pthread_cond_wait(&s->cond, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

int avcodec_async_get_early_output_stats(AVCodecAsyncContext *s, AVCodecEarlyOutputStats *stats)
{
    return s ? avcodec_get_early_output_stats(s->decoder, stats) : AVERROR(EINVAL);
}

int avcodec_async_get_stats(AVCodecAsyncContext *s, AVCodecAsyncStats *stats)
{
    if (!s || !stats)
        return AVERROR(EINVAL);
    pthread_mutex_lock(&s->mutex);
    stats->packets_accepted = s->packets_accepted;
    stats->frames_received = s->frames_received;
    pthread_mutex_unlock(&s->mutex);
    ff_thread_async_stats(s->decoder, &stats->peak_inflight, &stats->peak_active_workers);
    return 0;
}

int avcodec_async_enable_auto(AVCodecAsyncContext *s, int minimum, AVCodecAsyncBudget *budget)
{
    AVCodecAsyncBudget *retained = NULL;
    int *leases, ret = 0;
    if (!s || minimum < 0 || minimum > s->threads)
        return AVERROR(EINVAL);
    if (!minimum) minimum = FFMIN(2, s->threads);
    leases = av_malloc_array(s->threads, sizeof(*leases));
    if (!leases) return AVERROR(ENOMEM);
    if (budget) retained = ff_async_budget_ref(budget);
    else ret = ff_async_budget_default(&retained);
    if (ret < 0) { av_free(leases); return ret; }
    pthread_mutex_lock(&s->mutex);
    if (s->budget || s->packets_accepted || s->input_closed || s->terminal) {
        ret = AVERROR(EINVAL);
    } else {
        s->budget = retained;
        s->leases = leases;
        s->maximum = FFMIN(s->threads, ff_async_budget_slots(retained));
        minimum = FFMIN(minimum, s->maximum);
        s->minimum = s->target = minimum;
        s->step = 2;
        s->auto_pending = 1;
        s->auto_stats = (AVCodecAsyncAutoStats){ .enabled = 1, .minimum = minimum,
            .maximum = s->maximum, .target = minimum, .peak_target = minimum,
            .budget_slots = ff_async_budget_slots(retained) };
        wake_locked(s);
    }
    pthread_mutex_unlock(&s->mutex);
    if (ret < 0) { av_free(leases); avcodec_async_budget_free(&retained); }
    return ret;
}

int avcodec_async_get_auto_stats(AVCodecAsyncContext *s, AVCodecAsyncAutoStats *stats)
{
    if (!s || !stats) return AVERROR(EINVAL);
    pthread_mutex_lock(&s->mutex);
    *stats = s->auto_stats;
    stats->input_queue = s->input_count;
    stats->output_queue = s->output_count;
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

void avcodec_async_free(AVCodecAsyncContext **ps)
{
    AVCodecAsyncContext *s;
    int i;
    if (!ps || !(s = *ps))
        return;
    *ps = NULL;
    pthread_mutex_lock(&s->mutex);
    s->stop = 1;
    wake_locked(s);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread, NULL);
    /* Worker progress callbacks still reference s until all codec workers join. */
    avcodec_free_context(&s->decoder);
    if (s->budget) {
        reconcile_budget(s, 0);
        avcodec_async_budget_free(&s->budget);
        av_free(s->leases);
    }
    for (i = 0; i < s->capacity; i++) {
        av_packet_free(&s->input[i]);
        av_frame_free(&s->output[i]);
    }
    av_free(s->input);
    av_free(s->output);
    av_packet_free(&s->work_packet);
    av_frame_free(&s->work_frame);
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->mutex);
    av_free(s);
}
#else
int avcodec_async_create(AVCodecAsyncContext **out, AVCodecContext **decoder,
                         int capacity, void (*notify)(void *), void *opaque)
{ return AVERROR(ENOSYS); }
int avcodec_async_send_packet(AVCodecAsyncContext *s, const AVPacket *packet)
{ return AVERROR(ENOSYS); }
int avcodec_async_receive_frame(AVCodecAsyncContext *s, AVFrame *frame)
{ return AVERROR(ENOSYS); }
int avcodec_async_flush(AVCodecAsyncContext *s)
{ return AVERROR(ENOSYS); }
int avcodec_async_get_early_output_stats(AVCodecAsyncContext *s, AVCodecEarlyOutputStats *stats)
{
    return AVERROR(ENOSYS);
}

int avcodec_async_get_stats(AVCodecAsyncContext *s, AVCodecAsyncStats *stats)
{ return AVERROR(ENOSYS); }
void avcodec_async_free(AVCodecAsyncContext **s) {}
int avcodec_async_enable_auto(AVCodecAsyncContext *s, int minimum, AVCodecAsyncBudget *budget)
{ return AVERROR(ENOSYS); }
int avcodec_async_get_auto_stats(AVCodecAsyncContext *s, AVCodecAsyncAutoStats *stats)
{ return AVERROR(ENOSYS); }
#endif

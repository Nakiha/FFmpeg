/* Integration/latency probe. See test-async.sh and the migration guide. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#include "libavcodec/async.h"
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/sha.h"
#include "libavutil/time.h"

typedef struct Stamp { int64_t sequence, submitted_us; } Stamp;
static atomic_int buffer_gate, gate_entered;
typedef struct Event {
#ifdef _WIN32
    HANDLE handle;
#else
    int fd, write_fd;
#endif
} Event;

static void notify(void *opaque)
{
    Event *event = opaque;
#ifdef _WIN32
    SetEvent(event->handle);
#else
    uint64_t one = 1;
    ssize_t ignored = write(event->write_fd, &one, sizeof(one));
    (void)ignored; /* EAGAIN means the eventfd is already signaled. */
#endif
}

static void wait_event(Event *event, int timeout_ms)
{
#ifdef _WIN32
    WaitForSingleObject(event->handle, timeout_ms);
#else
    struct pollfd pfd = { event->fd, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_ms) > 0) {
        uint64_t value;
        ssize_t ignored = read(event->fd, &value, sizeof(value));
        (void)ignored;
    }
#endif
}

static int slow_buffer(AVCodecContext *ctx, AVFrame *frame, int flags)
{
    unsigned delay = *(unsigned *)ctx->opaque;
    while (atomic_load(&buffer_gate)) {
        atomic_store(&gate_entered, 1);
        av_usleep(1000);
    }
    if (delay)
        av_usleep(delay);
    return avcodec_default_get_buffer2(ctx, frame, flags);
}

static void slow_band(AVCodecContext *ctx, const AVFrame *src,
                      int offset[AV_NUM_DATA_POINTERS], int y, int type, int height)
{
    unsigned delay = *(unsigned *)ctx->opaque;
    const Stamp *stamp = src->opaque_ref && src->opaque_ref->size == sizeof(Stamp)
                       ? (const Stamp *)src->opaque_ref->data : NULL;
    (void)offset; (void)type; (void)height;
    if (y == 0 && delay && stamp && stamp->sequence % 3 == 0)
        av_usleep(delay);
}

static int digest_frame(FILE *output, AVFrame *frame, AVRational time_base)
{
    int size = av_image_get_buffer_size(frame->format, frame->width, frame->height, 1);
    uint8_t hash[32], *buffer;
    struct AVSHA *sha;
    int ret, i;
    if (size < 0)
        return size;
    buffer = av_malloc(size);
    sha = av_sha_alloc();
    if (!buffer || !sha) {
        av_free(buffer); av_free(sha);
        return AVERROR(ENOMEM);
    }
    ret = av_image_copy_to_buffer(buffer, size, (const uint8_t *const *)frame->data,
                                  frame->linesize, frame->format,
                                  frame->width, frame->height, 1);
    if (ret >= 0) {
        av_sha_init(sha, 256);
        av_sha_update(sha, buffer, size);
        av_sha_final(sha, hash);
        fprintf(output, "%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%d,%d,%d,%d,%d,%d,%d,%d,",
                av_rescale_q(frame->pts, time_base, AV_TIME_BASE_Q),
                frame->best_effort_timestamp, frame->pkt_dts,
                av_rescale_q(frame->duration, time_base, AV_TIME_BASE_Q),
                frame->width, frame->height, frame->format,
                frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den,
                frame->color_range, frame->color_primaries, frame->color_trc,
                frame->colorspace);
        for (i = 0; i < 32; i++)
            fprintf(output, "%02x", hash[i]);
        const Stamp *st=frame->opaque_ref ? (const Stamp *)frame->opaque_ref->data : NULL;
        fprintf(output,",%d,%d,%d,%d,%" PRId64 "\n",frame->flags,frame->repeat_pict,frame->decode_error_flags,frame->pict_type,st?st->sequence:-1);
    }
    av_free(buffer); av_free(sha);
    return ret < 0 ? ret : 0;
}

static int verify_packet_certificate(const char *path, const AVCodecParameters *par,
                                     AVPacket **packets, int count)
{
    char expected[128], actual[65]; uint8_t digest[32], size[8];
    struct AVSHA *sha=av_sha_alloc();
    FILE *file=fopen(path,"r");
    if (!file || !sha) { if(file)fclose(file);av_free(sha);return 0; }
    if (!fgets(expected,sizeof(expected),file)) expected[0]=0;
    fclose(file); av_sha_init(sha,256);
    AV_WB64(size,par->extradata_size);av_sha_update(sha,size,8);
    av_sha_update(sha,par->extradata,par->extradata_size);
    for (int i=0;i<count;i++) {
        AV_WB64(size,packets[i]->size);av_sha_update(sha,size,8);
        av_sha_update(sha,packets[i]->data,packets[i]->size);
        AV_WB64(size,packets[i]->pts);av_sha_update(sha,size,8);
        AV_WB64(size,packets[i]->dts);av_sha_update(sha,size,8);
        AV_WB64(size,packets[i]->duration);av_sha_update(sha,size,8);
    }
    av_sha_final(sha,digest);av_free(sha);
    for(int i=0;i<32;i++)snprintf(actual+i*2,3,"%02x",digest[i]);
    char certificate[128];snprintf(certificate,sizeof(certificate),"progressive-even-poc0-v1:%s\n",actual);
    return !strcmp(certificate,expected);
}

int main(int argc, char **argv)
{
    AVFormatContext *format = NULL;
    AVCodecContext *ctx = NULL;
    AVCodecAsyncContext *async = NULL;
    const AVCodec *codec = NULL;
    AVPacket **packets = NULL, *read_packet = NULL;
    AVFrame *frame = NULL;
    AVCodecAsyncStats stats = { 0 };
    AVRational time_base;
    Event event;
    FILE *csv = NULL, *digest = NULL;
    char path[2048];
    int ret = 0, stream, count = 0, i, cycle, threads, fps, legacy, cycles, capacity;
    unsigned delay;
    int64_t total_frames = 0, total_latency = 0, first_output = -1;
    int64_t max_send = 0, max_receive = 0, send_eagain = 0;

    if (argc < 6 || argc > 9) {
        fprintf(stderr, "Usage: %s INPUT THREADS FPS MODE PREFIX [CYCLES] [DELAY_US] [CAPACITY]\n", argv[0]);
        return 2;
    }
    threads = atoi(argv[2]); fps = atoi(argv[3]);
    legacy = !strcmp(argv[4], "legacy");
    cycles = argc > 6 ? atoi(argv[6]) : 1;
    delay = argc > 7 ? (unsigned)atoi(argv[7]) : 0;
    capacity = argc > 8 ? atoi(argv[8]) : threads;
    if (threads < 1 || fps < 0 || fps > 1000 || cycles < 1 || capacity < 1)
        return 2;
    av_log_set_level(AV_LOG_ERROR);
#ifdef _WIN32
    event.handle = CreateEvent(NULL, FALSE, FALSE, NULL);
#else
    int pipefd[2];
    if (pipe(pipefd)) return 2;
    event.fd=pipefd[0]; event.write_fd=pipefd[1];
    fcntl(event.fd,F_SETFL,O_NONBLOCK); fcntl(event.write_fd,F_SETFL,O_NONBLOCK);
#endif
    snprintf(path, sizeof(path), "%s.csv", argv[5]);
    csv = fopen(path, "w");
    snprintf(path, sizeof(path), "%s.sha256", argv[5]);
    digest = fopen(path, "w");
    if (!csv || !digest) { ret = AVERROR(EIO); goto end; }
    if ((ret = avformat_open_input(&format, argv[1], NULL, NULL)) < 0 ||
        (ret = avformat_find_stream_info(format, NULL)) < 0)
        goto end;
    stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0) { ret = stream; goto end; }
    time_base = format->streams[stream]->time_base;
    read_packet = av_packet_alloc(); frame = av_frame_alloc();
    if (!read_packet || !frame) { ret = AVERROR(ENOMEM); goto end; }
    if (getenv("SEEK_US")) {
        int64_t target=av_rescale_q(atoll(getenv("SEEK_US")),AV_TIME_BASE_Q,time_base);
        if ((ret=av_seek_frame(format,stream,target,AVSEEK_FLAG_BACKWARD))<0) goto end;
    }
    while ((ret = av_read_frame(format, read_packet)) >= 0) {
        if (read_packet->stream_index == stream) {
            AVPacket **grown = av_realloc_array(packets, count + 1, sizeof(*packets));
            if (!grown) { ret = AVERROR(ENOMEM); goto end; }
            packets = grown;
            if (getenv("SYNTHETIC_PTS")) {
                read_packet->pts = read_packet->dts = av_rescale_q(count,(AVRational){1,25},format->streams[stream]->time_base);
                read_packet->duration = av_rescale_q(1,(AVRational){1,25},format->streams[stream]->time_base);
            }
            if (getenv("TEST_FIRST_TWO_REORDER")) {
                int display = count==0 ? 1 : count==1 ? 0 : count;
                read_packet->pts = av_rescale_q(display,(AVRational){1,25},format->streams[stream]->time_base);
                read_packet->dts = av_rescale_q(count-2,(AVRational){1,25},format->streams[stream]->time_base);
            }
            packets[count] = av_packet_clone(read_packet);
            if (!packets[count]) { ret = AVERROR(ENOMEM); goto end; }
            count++;
        }
        av_packet_unref(read_packet);
    }
    if (ret != AVERROR_EOF || !count) { ret = AVERROR_INVALIDDATA; goto end; }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) { ret = AVERROR(ENOMEM); goto end; }
    if ((ret = avcodec_parameters_to_context(ctx, format->streams[stream]->codecpar)) < 0)
        goto end;
    ctx->pkt_timebase = time_base;
    ctx->thread_count = threads;
    ctx->thread_type = FF_THREAD_FRAME;
    ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    ctx->opaque = &delay;
    ctx->get_buffer2 = slow_buffer;
    ctx->draw_horiz_band = slow_band;
    if (codec->id == AV_CODEC_ID_HEVC && getenv("FFMPEG_LIVE_IDR_EARLY") &&
        (ret = av_opt_set_int(ctx->priv_data, "live_idr_early_output", 1, 0)) < 0)
        goto end;
    if (codec->id == AV_CODEC_ID_HEVC && getenv("FFMPEG_LIVE_POC_EARLY") &&
        (ret = av_opt_set_int(ctx->priv_data, "live_poc_early_output", 1, 0)) < 0)
        goto end;
    if (getenv("EARLY_OUTPUT")) {
        int early=atoi(getenv("EARLY_OUTPUT"));
        if ((ret=av_opt_set_int(ctx->priv_data,"live_poc_early_output",early,0))<0) goto end;
        if (codec->id==AV_CODEC_ID_HEVC && (ret=av_opt_set_int(ctx->priv_data,"live_idr_early_output",early,0))<0) goto end;
    }
    if (codec->id==AV_CODEC_ID_H264 && getenv("H264_CERTIFICATE")) {
        int64_t began=av_gettime_relative();
        int verified=verify_packet_certificate(getenv("H264_CERTIFICATE"),format->streams[stream]->codecpar,packets,count);
        fprintf(stderr,"certificate_verified=%d certificate_hash_us=%lld\n",verified,(long long)(av_gettime_relative()-began));
        if ((ret=av_opt_set_int(ctx->priv_data,"live_poc_verified_even",verified,0))<0) goto end;
    }
    if (getenv("STRICT_ERRORS")) ctx->err_recognition |= AV_EF_EXPLODE;
    if (!strcmp(argv[4], "checksum"))
        ctx->err_recognition |= AV_EF_CRCCHECK | AV_EF_EXPLODE;
    if ((ret = avcodec_open2(ctx, codec, NULL)) < 0)
        goto end;
    if (!strcmp(argv[4], "api")) {
        AVCodecAsyncContext *invalid = NULL;
        AVCodecContext *used = avcodec_alloc_context3(codec);
        ret = avcodec_async_create(&invalid, &ctx, 0, notify, &event);
        if (ret != AVERROR(EINVAL) || invalid || !ctx) { ret = AVERROR_INVALIDDATA; goto end; }
        if (!used) { ret = AVERROR(ENOMEM); goto end; }
        avcodec_parameters_to_context(used, format->streams[stream]->codecpar);
        used->thread_count = 2;
        used->thread_type = FF_THREAD_FRAME;
        ret = avcodec_open2(used, codec, NULL);
        if (ret >= 0) ret = avcodec_send_packet(used, NULL);
        if (ret >= 0) ret = avcodec_async_create(&invalid, &used, 1, notify, &event);
        if (ret != AVERROR(EINVAL) || invalid || !used) {
            avcodec_async_free(&invalid); avcodec_free_context(&used);
            ret = AVERROR_INVALIDDATA; goto end;
        }
        avcodec_free_context(&used);
    }
    if (!legacy && (ret = avcodec_async_create(&async, &ctx, capacity, notify, &event)) < 0)
        goto end;
#if AVCODEC_ASYNC_API_VERSION >= 2
    if (!legacy && getenv("FFMPEG_LIVE_AUTO_MIN") &&
        (ret = avcodec_async_enable_auto(async, atoi(getenv("FFMPEG_LIVE_AUTO_MIN")), NULL)) < 0)
        goto end;
#endif

    if (!strcmp(argv[4], "api")) {
        uint8_t payload[4] = { 0 };
        int64_t started = av_gettime_relative();
        read_packet->data = payload;
        read_packet->size = sizeof(payload);
        ret = avcodec_async_send_packet(async, read_packet);
        read_packet->data = NULL; read_packet->size = 0;
        if (ret != AVERROR(EINVAL)) { ret = AVERROR_INVALIDDATA; goto end; }
        if ((ret = avcodec_async_send_packet(async, NULL)) < 0) goto end;
        if (avcodec_async_send_packet(async, packets[0]) != AVERROR_EOF) {
            ret = AVERROR_INVALIDDATA; goto end;
        }
        do {
            ret = avcodec_async_receive_frame(async, frame);
            if (ret == AVERROR(EAGAIN)) wait_event(&event, 10);
            if (av_gettime_relative() - started > 3000000) { ret = AVERROR(ETIMEDOUT); goto end; }
        } while (ret == AVERROR(EAGAIN));
        if (ret != AVERROR_EOF) { ret = AVERROR_INVALIDDATA; goto end; }
        if ((ret = avcodec_async_flush(async)) < 0) goto end;
    }

    if (!strcmp(argv[4], "gate") || !strcmp(argv[4], "flush")) {
        int gate = !strcmp(argv[4], "gate");
        int64_t started = av_gettime_relative();
        atomic_store(&buffer_gate, gate);
        if ((ret = avcodec_async_send_packet(async, packets[0])) < 0) goto end;
        do {
            avcodec_async_get_stats(async, &stats);
            if (av_gettime_relative() - started > 3000000) {
                atomic_store(&buffer_gate, 0);
                ret = AVERROR(ETIMEDOUT); goto end;
            }
            av_usleep(100);
        } while (gate ? !atomic_load(&gate_entered) : !stats.peak_active_workers);
        if (gate) {
            /* Decoder is held at a closed gate: neither frontend data call may wait. */
            ret = avcodec_async_send_packet(async, packets[0]);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                atomic_store(&buffer_gate, 0); goto end;
            }
            ret = avcodec_async_receive_frame(async, frame);
            atomic_store(&buffer_gate, 0);
            if (ret != AVERROR(EAGAIN)) { ret = AVERROR_INVALIDDATA; goto end; }
        }
        if ((ret = avcodec_async_flush(async)) < 0) goto end;
    }

    for (cycle = 0; cycle < cycles; cycle++) {
        int next = 0, closed = 0, frames = 0, finished = 0;
        int64_t began = av_gettime_relative();
        if (!strcmp(argv[4], "error") || !strcmp(argv[4], "checksum")) {
            if (!strcmp(argv[4], "checksum")) {
                int changed = 0;
                const AVCodecParameters *params = format->streams[stream]->codecpar;
                if (codec->id != AV_CODEC_ID_HEVC || params->extradata_size < 23 ||
                    params->extradata[0] != 1 || (params->extradata[21] & 3) != 3) {
                    ret = AVERROR(EINVAL); goto end;
                }
                if ((ret = av_packet_ref(read_packet, packets[0])) < 0 ||
                    (ret = av_packet_make_writable(read_packet)) < 0) goto end;
                size_t packet_size = read_packet->size;
                for (size_t pos = 0; pos + 4 < packet_size;) {
                    unsigned size = AV_RB32(read_packet->data + pos); pos += 4;
                    if (size < 2 || size > packet_size - pos) break;
                    if (((read_packet->data[pos] >> 1) & 63) == 40 && size > 6 &&
                        read_packet->data[pos+2] == 132 && read_packet->data[pos+3] == 49) {
                        for (size_t j = pos + 5; j + 1 < pos + size; j++) {
                            if (read_packet->data[j] >= 16) {
                                read_packet->data[j] ^= 1; changed = 1; break;
                            }
                        }
                    }
                    pos += size;
                }
                if (!changed) { ret = AVERROR(EINVAL); goto end; }
            } else {
                if ((ret = av_new_packet(read_packet, 128)) < 0) goto end;
                memset(read_packet->data, 255, read_packet->size);
            }
            if ((ret = avcodec_async_send_packet(async, read_packet)) < 0) goto end;
            av_packet_unref(read_packet);
            do {
                ret = avcodec_async_receive_frame(async, frame);
                if (ret == AVERROR(EAGAIN)) wait_event(&event, 10);
                if (av_gettime_relative() - began > 3000000) { ret = AVERROR(ETIMEDOUT); goto end; }
            } while (ret == AVERROR(EAGAIN));
            if (ret >= 0 || ret == AVERROR_EOF) { ret = AVERROR_INVALIDDATA; goto end; }
            fprintf(stderr,"injected_error=%d\n",ret);
            if ((ret = avcodec_async_flush(async)) < 0) goto end;
            began = av_gettime_relative();
        }
        while (!finished) {
            int progressed = 0, blocked = 0;
            int64_t now = av_gettime_relative();
            if (now - began > 30000000) { ret = AVERROR(ETIMEDOUT); goto end; }
            while (next < count && (!fps || now >= began + (int64_t)next * 1000000 / fps)) {
                AVPacket *packet = packets[next];
                Stamp *stamp;
                int64_t call, duration;
                if (!strcmp(argv[4], "pause") && next == 1 && !frames)
                    break;
                av_buffer_unref(&packet->opaque_ref);
                packet->opaque_ref = av_buffer_alloc(sizeof(Stamp));
                if (!packet->opaque_ref) { ret = AVERROR(ENOMEM); goto end; }
                stamp = (Stamp *)packet->opaque_ref->data;
                stamp->sequence = next;
                stamp->submitted_us = av_gettime_relative();
                call = av_gettime_relative();
                ret = legacy ? avcodec_send_packet(ctx, packet)
                             : avcodec_async_send_packet(async, packet);
                duration = av_gettime_relative() - call;
                if (duration > max_send) max_send = duration;
                if (ret == AVERROR(EAGAIN)) { send_eagain++; blocked = 1; break; }
                if (ret < 0) goto end;
                next++; progressed = 1;
                if (!strcmp(argv[4], "close")) {
                    int64_t started = av_gettime_relative();
                    do {
                        avcodec_async_get_stats(async, &stats);
                        if (av_gettime_relative() - started > 3000000) {
                            ret = AVERROR(ETIMEDOUT); goto end;
                        }
                        av_usleep(100);
                    } while (!stats.peak_active_workers);
                    ret = 0; goto end;
                }
                if (legacy) break; /* Match send, then receive-until-EAGAIN. */
                now = av_gettime_relative();
            }
            if (next == count && !closed) {
                ret = legacy ? avcodec_send_packet(ctx, NULL)
                             : avcodec_async_send_packet(async, NULL);
                if (ret == 0) { closed = 1; progressed = 1; }
                else if (ret != AVERROR(EAGAIN)) goto end;
            }
            for (;;) {
                int64_t call = av_gettime_relative(), now, duration;
                const Stamp *stamp;
                ret = legacy ? avcodec_receive_frame(ctx, frame)
                             : avcodec_async_receive_frame(async, frame);
                now = av_gettime_relative(); duration = now - call;
                if (duration > max_receive) max_receive = duration;
                if (ret == AVERROR_EOF) { finished = 1; break; }
                if (ret == AVERROR(EAGAIN)) break;
                if (ret < 0) goto end;
                if (!frame->opaque_ref || frame->opaque_ref->size != sizeof(Stamp)) {
                    ret = AVERROR_INVALIDDATA; goto end;
                }
                stamp = (Stamp *)frame->opaque_ref->data;
                if (first_output < 0) first_output = now - began;
                total_latency += now - stamp->submitted_us;
                fprintf(csv, "frame,%d,%d,%" PRId64 ",%" PRId64 "\n",
                        cycle, frames, stamp->sequence, now - stamp->submitted_us);
                if (!getenv("NO_HASH") && (ret = digest_frame(digest, frame, time_base)) < 0) goto end;
                av_frame_unref(frame);
                frames++; total_frames++; progressed = 1;
            }
            if (!progressed && !finished) {
                int timeout = 1000;
                now = av_gettime_relative();
                if (fps && next < count && !blocked) {
                    int64_t due = began + (int64_t)next * 1000000 / fps;
                    timeout = due > now ? (int)((due - now + 999) / 1000) : 1;
                }
                if (legacy) av_usleep(timeout * 1000);
                else wait_event(&event, timeout);
            }
        }

        if (legacy) avcodec_flush_buffers(ctx);
        else if ((ret = avcodec_async_flush(async)) < 0) goto end;
    }
    struct rusage usage; getrusage(RUSAGE_SELF,&usage);
    fprintf(stderr,"cpu_us=%lld\n",(long long)(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000+usage.ru_utime.tv_usec+usage.ru_stime.tv_usec);
    if (async) avcodec_async_get_stats(async, &stats);
    fprintf(csv, "summary,codec=%s,threads=%d,frames=%" PRId64
            ",first_frame_us=%" PRId64 ",mean_latency_us=%" PRId64
            ",max_send_us=%" PRId64 ",max_receive_us=%" PRId64
            ",peak_inflight=%d,peak_active_workers=%d,send_eagain=%" PRId64 "\n",
            codec->name, threads, total_frames, first_output, total_frames ? total_latency / total_frames : 0,
            max_send, max_receive, stats.peak_inflight, stats.peak_active_workers, send_eagain);
    ret = 0;
end:
    fprintf(stderr,"terminal_return=%d\n",ret);
    atomic_store(&buffer_gate, 0);
    if (ret < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, message, sizeof(message));
        fprintf(stderr, "Probe failed: %s\n", message);
    }
    avcodec_async_free(&async);
    avcodec_free_context(&ctx);
    av_packet_free(&read_packet); av_frame_free(&frame);
    for (i = 0; i < count; i++) av_packet_free(&packets[i]);
    av_free(packets); avformat_close_input(&format);
    if (csv) fclose(csv);
    if (digest) fclose(digest);
#ifdef _WIN32
    CloseHandle(event.handle);
#else
    close(event.fd); close(event.write_fd);
#endif
    return ret < 0 ? 1 : 0;
}

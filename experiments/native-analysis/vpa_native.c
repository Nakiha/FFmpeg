/* Native validation driver for the analysis-core execution modes.
 *
 * Feeds container packets through the vpa ABI (same code path the WASM core
 * uses), collects the published picture records into a binary dump, and prints
 * a JSON summary. Comparing mode 0 (full) and mode 1 (no pixels) byte-for-byte
 * is the native equivalent of scripts/test-analysis-execution.mjs.
 *
 * Usage: vpa_native <mode 0|1> <file> <dump.bin>
 */
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

int vpa_execution_abi(void);
int vpa_execution_mode(int mode);
int vpa_execution_path(void);
int vpa_execution_reason(void);
int vpa_execution_retry_code(void);
int vpa_open(int codec, const uint8_t *extra, int size);
int vpa_feed_known(const uint8_t *bytes, int size, double pts, double dts, int known, int ordinal);
int vpa_drain(void);
int vpa_step(void);
int vpa_count(void);
intptr_t vpa_blocks(void);
intptr_t vpa_refs(void);
int vpa_ref_count(void);
int vpa_refs_invalid(void);
int vpa_width(void);
int vpa_height(void);
int vpa_depth(void);
int vpa_invalid(void);
int vpa_record_bytes(void);
int vpa_picture_type(void);
int vpa_poc(void);
int vpa_ordinal(void);
double vpa_pts(void);
int vpa_take(int maxBytes);
void vpa_close(void);

typedef struct { FILE *dump; int64_t pictures, bytes; } Sink;

static int collect(Sink *sink) {
    for (int i = 0; i < 4000; i++) {
        int s = vpa_step();
        if (s < 0) return s;
        if (s != 1) return 0;
        const int32_t count = vpa_count();
        const int32_t refs = vpa_ref_count();
        const uint8_t *bp = (const uint8_t *)(uintptr_t)vpa_blocks();
        const uint8_t *rp = (const uint8_t *)(uintptr_t)vpa_refs();
        struct {
            int64_t ordinal, width, height, depth, invalid, picture_type, poc;
            int64_t count, ref_count, refs_invalid, record_bytes, pts_us;
        } head = {
            vpa_ordinal(), vpa_width(), vpa_height(), vpa_depth(), vpa_invalid(),
            vpa_picture_type(), vpa_poc(), count, refs, vpa_refs_invalid(),
            vpa_record_bytes(), (int64_t)llround(vpa_pts() * 1e6),
        };
        if (fwrite(&head, 1, sizeof head, sink->dump) != sizeof head ||
            fwrite(bp, 1, (size_t)count * 12, sink->dump) != (size_t)count * 12 ||
            fwrite(rp, 1, (size_t)refs * 8, sink->dump) != (size_t)refs * 8)
            return -1;
        sink->bytes += (int64_t)sizeof head + count * 12 + refs * 8;
        sink->pictures++;
        int taken = vpa_take(8 * 1024 * 1024);
        if (taken < 0) return taken;
    }
    return -1; /* unbounded step */
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <mode 0|1> <file> <dump.bin>\n", argv[0]);
        return 2;
    }
    const int mode = atoi(argv[1]);
    Sink sink = { .dump = fopen(argv[3], "wb") };
    if (!sink.dump) { perror("dump"); return 2; }
    if (vpa_execution_abi() != 1 || vpa_execution_mode(mode) != 0) {
        fprintf(stderr, "execution ABI mismatch\n");
        return 2;
    }

    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input(&fmt, argv[2], NULL, NULL);
    if (ret < 0) { fprintf(stderr, "open_input failed\n"); return 1; }
    if ((ret = avformat_find_stream_info(fmt, NULL)) < 0) { fprintf(stderr, "find_stream_info failed\n"); return 1; }
    int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream < 0) { fprintf(stderr, "no video stream\n"); return 1; }
    AVStream *st = fmt->streams[stream];
    int codec = st->codecpar->codec_id == AV_CODEC_ID_H264 ? 1 :
                st->codecpar->codec_id == AV_CODEC_ID_HEVC ? 2 :
                st->codecpar->codec_id == AV_CODEC_ID_VVC ? 3 : 0;
    if (!codec) { fprintf(stderr, "unsupported codec %d\n", st->codecpar->codec_id); return 1; }

    if ((ret = vpa_open(codec, st->codecpar->extradata, st->codecpar->extradata_size)) < 0) {
        fprintf(stderr, "vpa_open failed: %d\n", ret);
        return 1;
    }
    const int open_path = vpa_execution_path();

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    AVPacket *pkt = av_packet_alloc();
    int64_t ordinal = 0;
    int error = 0, eof = 0;
    const int inject_at = getenv("VPA_INJECT_GRAIN") ? atoi(getenv("VPA_INJECT_GRAIN")) : -1;
    while (!error) {
        int have_packet = 0;
        if (!eof) {
            ret = av_read_frame(fmt, pkt);
            if (ret < 0) {
                if (ret != AVERROR_EOF) { error = ret; break; }
                eof = 1;
                if ((error = vpa_drain()) < 0) break;
            } else if (pkt->stream_index != stream) {
                av_packet_unref(pkt);
                continue;
            } else {
                have_packet = 1;
            }
        }

        if (have_packet) {
            /* Optional synthetic prefix film-grain SEI prepended to the packet
             * (MP4-framed): the SEI must share the packet with the picture. */
            static const uint8_t grain[] = {
                0x00, 0x00, 0x00, 0x07, 0x00, 0xb9, 19, 2, 0x00, 0x02, 0x80,
            };
            uint8_t *merged = NULL;
            const uint8_t *payload = pkt->data;
            int payload_size = pkt->size;
            if ((int)ordinal == inject_at) {
                merged = malloc(sizeof grain + pkt->size);
                if (!merged) { error = AVERROR(ENOMEM); break; }
                memcpy(merged, pkt->data, pkt->size);
                memcpy(merged + pkt->size, grain, sizeof grain);
                payload = merged;
                payload_size = (int)sizeof grain + pkt->size;
                fprintf(stderr, "inject grain at %d\n", (int)ordinal);
            }
            const int known = (pkt->pts != AV_NOPTS_VALUE ? 1 : 0) | (pkt->dts != AV_NOPTS_VALUE ? 2 : 0);
            const double pts = pkt->pts == AV_NOPTS_VALUE ? 0 : pkt->pts * av_q2d(st->time_base) * 1e6;
            const double dts = pkt->dts == AV_NOPTS_VALUE ? 0 : pkt->dts * av_q2d(st->time_base) * 1e6;
            int r;
            do {
                r = vpa_feed_known(payload, payload_size, pts, dts, known, (int)ordinal);
                if (r == 2 && (error = collect(&sink)) != 0) break;
            } while (r == 2);
            free(merged);
            av_packet_unref(pkt);
            ordinal++;
            if (!error && r < 0) error = r;
            if (error) break;
        }

        if ((error = collect(&sink)) != 0) break;
        if (eof) break;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("{\"file\":\"%s\",\"mode\":%d,\"openPath\":%d,\"path\":%d,\"reason\":%d,\"retryCode\":%d,"
           "\"pictures\":%" PRId64 ",\"bytes\":%" PRId64 ",\"ms\":%.1f,\"error\":%d}\n",
           argv[2], mode, open_path, vpa_execution_path(), vpa_execution_reason(), vpa_execution_retry_code(),
           sink.pictures, sink.bytes, ms, error);
    vpa_close();
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    fclose(sink.dump);
    return error ? 1 : 0;
}

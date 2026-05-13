/*
 * VoidPlayer FFmpeg analysis tool.
 *
 * H.266/VVC, H.265, and H.264 use decoder-internal hooks to emit real VACHUNK payloads.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
#include "libavcodec/codec_id.h"
#include "libavcodec/codec_par.h"
#include "libavcodec/packet.h"
#include "libavcodec/voidplayer_vachunk.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/cpu.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"

typedef struct AnalyzerOptions {
    const char *codec;
    const char *input;
    const char *vachunk;
    int probe_only;
    int has_start_frame;
    int has_end_frame;
    int has_base_revision;
    int has_generator_revision;
    uint64_t start_frame;
    uint64_t end_frame;
    uint64_t base_revision;
    uint64_t generator_revision;
} AnalyzerOptions;

static void print_usage(FILE *out)
{
    fprintf(out,
            "Usage: void_ffmpeg_analyzer --codec <codec> --input <path> [--probe-only | --vachunk <path> --start-frame <n> --end-frame <n> --base-revision <n> --generator-revision <n>]\n"
            "\n"
            "Supported codec names: vvc, h266, hevc, h265, h264\n"
            "\n"
            "VACHUNK generation emits real decoder-derived VVC/HEVC/H264 overlay payloads.\n");
}

static void error_text(int errnum, char *buffer, size_t buffer_size)
{
    if (av_strerror(errnum, buffer, buffer_size) < 0)
        snprintf(buffer, buffer_size, "error %d", errnum);
}

static int codec_id_from_name(const char *name, enum AVCodecID *codec_id)
{
    if (!name || !codec_id)
        return -1;

    if (!strcmp(name, "hevc") || !strcmp(name, "h265")) {
        *codec_id = AV_CODEC_ID_HEVC;
        return 0;
    }
    if (!strcmp(name, "h264") || !strcmp(name, "avc")) {
        *codec_id = AV_CODEC_ID_H264;
        return 0;
    }
    if (!strcmp(name, "vvc") || !strcmp(name, "h266")) {
        *codec_id = AV_CODEC_ID_VVC;
        return 0;
    }

    return -1;
}

static uint16_t vachunk_codec_from_avcodec(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return VOIDPLAYER_VACHUNK_CODEC_H264;
    case AV_CODEC_ID_HEVC:
        return VOIDPLAYER_VACHUNK_CODEC_HEVC;
    case AV_CODEC_ID_VVC:
        return VOIDPLAYER_VACHUNK_CODEC_VVC;
    default:
        return 0;
    }
}

static int parse_u64_arg(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (!text || !out)
        return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || !end || *end != '\0')
        return -1;
    *out = (uint64_t)value;
    return 0;
}

static int parse_args(int argc, char **argv, AnalyzerOptions *options)
{
    int i;

    memset(options, 0, sizeof(*options));

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(stdout);
            return 1;
        }
        if (!strcmp(argv[i], "--probe-only")) {
            options->probe_only = 1;
            continue;
        }
        if (!strcmp(argv[i], "--codec") && i + 1 < argc) {
            options->codec = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            options->input = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--vachunk") && i + 1 < argc) {
            options->vachunk = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--start-frame") && i + 1 < argc) {
            if (parse_u64_arg(argv[++i], &options->start_frame) < 0) {
                fprintf(stderr, "Invalid --start-frame value.\n");
                return -1;
            }
            options->has_start_frame = 1;
            continue;
        }
        if (!strcmp(argv[i], "--end-frame") && i + 1 < argc) {
            if (parse_u64_arg(argv[++i], &options->end_frame) < 0) {
                fprintf(stderr, "Invalid --end-frame value.\n");
                return -1;
            }
            options->has_end_frame = 1;
            continue;
        }
        if (!strcmp(argv[i], "--base-revision") && i + 1 < argc) {
            if (parse_u64_arg(argv[++i], &options->base_revision) < 0) {
                fprintf(stderr, "Invalid --base-revision value.\n");
                return -1;
            }
            options->has_base_revision = 1;
            continue;
        }
        if (!strcmp(argv[i], "--generator-revision") && i + 1 < argc) {
            if (parse_u64_arg(argv[++i], &options->generator_revision) < 0) {
                fprintf(stderr, "Invalid --generator-revision value.\n");
                return -1;
            }
            options->has_generator_revision = 1;
            continue;
        }

        fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
        print_usage(stderr);
        return -1;
    }

    if (!options->codec || !options->input ||
        (!options->probe_only && !options->vachunk)) {
        fprintf(stderr, "Missing required arguments.\n");
        print_usage(stderr);
        return -1;
    }
    if ((options->has_start_frame || options->has_end_frame) && options->probe_only) {
        fprintf(stderr, "Frame windows require generation output.\n");
        print_usage(stderr);
        return -1;
    }
    if (options->has_start_frame && options->has_end_frame &&
        options->start_frame > options->end_frame) {
        fprintf(stderr, "--start-frame must be <= --end-frame.\n");
        return -1;
    }
    if (options->vachunk &&
        (!options->has_start_frame || !options->has_end_frame ||
         !options->has_base_revision || !options->has_generator_revision)) {
        fprintf(stderr, "--vachunk requires frame range and revision arguments.\n");
        print_usage(stderr);
        return -1;
    }
    if (options->vachunk &&
        (options->start_frame > UINT32_MAX || options->end_frame > UINT32_MAX)) {
        fprintf(stderr, "--vachunk frame range exceeds uint32 limits.\n");
        return -1;
    }

    return 0;
}

static int open_input(const AnalyzerOptions *options,
                      enum AVCodecID expected_codec,
                      AVFormatContext **format_out,
                      int *stream_index_out)
{
    AVFormatContext *format = NULL;
    AVCodecParameters *codecpar = NULL;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    int stream_index;
    int ret;

    ret = avformat_open_input(&format, options->input, NULL, NULL);
    if (ret < 0) {
        error_text(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "Failed to open input: %s\n", errbuf);
        return 10;
    }

    ret = avformat_find_stream_info(format, NULL);
    if (ret < 0) {
        error_text(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "Failed to read stream info: %s\n", errbuf);
        avformat_close_input(&format);
        return 11;
    }

    stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream_index < 0) {
        error_text(stream_index, errbuf, sizeof(errbuf));
        fprintf(stderr, "No video stream found: %s\n", errbuf);
        avformat_close_input(&format);
        return 12;
    }

    codecpar = format->streams[stream_index]->codecpar;
    if (!codecpar) {
        fprintf(stderr, "Video stream has no codec parameters.\n");
        avformat_close_input(&format);
        return 13;
    }

    fprintf(stdout,
            "input=%s\nstream=%d\ncodec=%s\nwidth=%d\nheight=%d\n",
            options->input,
            stream_index,
            avcodec_get_name(codecpar->codec_id),
            codecpar->width,
            codecpar->height);

    if (codecpar->codec_id != expected_codec) {
        fprintf(stderr,
                "Codec mismatch: requested %s but input stream is %s.\n",
                options->codec,
                avcodec_get_name(codecpar->codec_id));
        avformat_close_input(&format);
        return 14;
    }

    *format_out = format;
    *stream_index_out = stream_index;
    return 0;
}

static const char *software_decoder_name(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return "h264";
    case AV_CODEC_ID_HEVC:
        return "hevc";
    case AV_CODEC_ID_VVC:
        return "vvc";
    default:
        return NULL;
    }
}

static const char *bitstream_filter_name(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_VVC:
        return "vvc_mp4toannexb";
    default:
        return NULL;
    }
}

static int receive_frames(AVCodecContext *decoder, AVFrame *frame)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    int ret;

    for (;;) {
        ret = avcodec_receive_frame(decoder, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            error_text(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "Failed to decode frame: %s\n", errbuf);
            return ret;
        }

        av_frame_unref(frame);
    }
}

static int send_packet_to_decoder(AVCodecContext *decoder, AVPacket *packet, AVFrame *frame)
{
    int ret = avcodec_send_packet(decoder, packet);
    if (ret < 0)
        return ret;
    return receive_frames(decoder, frame);
}

static void print_available_video_decoders(FILE *out)
{
    const AVCodec *codec = NULL;
    void *iter = NULL;

    fprintf(out, "Available video decoders:");
    while ((codec = av_codec_iterate(&iter))) {
        if (av_codec_is_decoder(codec) && codec->type == AVMEDIA_TYPE_VIDEO)
            fprintf(out, " %s", codec->name);
    }
    fprintf(out, "\n");
}

static int decode_vachunk(const AnalyzerOptions *options,
                          AVFormatContext *format,
                          int stream_index)
{
    AVStream *stream = format->streams[stream_index];
    AVCodecParameters *codecpar = stream->codecpar;
    const AVCodec *codec = NULL;
    AVCodecContext *decoder = NULL;
    AVBSFContext *bsf = NULL;
    AVPacket *packet = NULL;
    AVPacket *filtered_packet = NULL;
    AVFrame *frame = NULL;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    uint16_t vachunk_codec = vachunk_codec_from_avcodec(codecpar->codec_id);
    int writer_started = 0;
    int ret = 0;
    uint64_t video_packet_index = 0;
    const char *failed_stage = "initialization";

    if (!vachunk_codec) {
        fprintf(stderr, "VACHUNK generation is only implemented for H.266/H.265/H.264 in this analyzer build.\n");
        return 26;
    }

    codec = avcodec_find_decoder_by_name(software_decoder_name(codecpar->codec_id));
    if (!codec)
        codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "No decoder found for codec: %s\n", avcodec_get_name(codecpar->codec_id));
        print_available_video_decoders(stderr);
        return 21;
    }

    decoder = avcodec_alloc_context3(codec);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!decoder || !packet || !frame) {
        ret = AVERROR(ENOMEM);
        goto done;
    }

    {
        const char *bsf_name = bitstream_filter_name(codecpar->codec_id);
        if (bsf_name) {
            const AVBitStreamFilter *filter = av_bsf_get_by_name(bsf_name);
            failed_stage = "find bitstream filter";
            if (!filter) {
                fprintf(stderr, "No bitstream filter found: %s\n", bsf_name);
                ret = AVERROR_BSF_NOT_FOUND;
                goto done;
            }

            filtered_packet = av_packet_alloc();
            if (!filtered_packet) {
                ret = AVERROR(ENOMEM);
                goto done;
            }

            ret = av_bsf_alloc(filter, &bsf);
            if (ret < 0)
                goto done;
            ret = avcodec_parameters_copy(bsf->par_in, codecpar);
            if (ret < 0)
                goto done;
            bsf->time_base_in = stream->time_base;
            failed_stage = "initialize bitstream filter";
            ret = av_bsf_init(bsf);
            if (ret < 0)
                goto done;
            codecpar = bsf->par_out;
        }
    }

    failed_stage = "copy codec parameters";
    ret = avcodec_parameters_to_context(decoder, codecpar);
    if (ret < 0)
        goto done;
    if (bsf && codecpar->codec_id == AV_CODEC_ID_VVC) {
        av_freep(&decoder->extradata);
        decoder->extradata_size = 0;
    }
    decoder->pkt_timebase = stream->time_base;
    if (codecpar->codec_id != AV_CODEC_ID_VVC)
        decoder->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    {
        int threads = av_cpu_count();
        if (threads < 2)
            threads = 2;
        if (threads > 16)
            threads = 16;
        decoder->thread_count = threads;
        /*
         * H.264/HEVC workers may finish pictures out of packet order when frame
         * threading is enabled. Each packet gets a COPY_OPAQUE coded-order key;
         * the VACHUNK writer uses that key to sort frame summaries before writing.
         *
         * VVC uses its own executor behind AV_CODEC_CAP_OTHER_THREADS, and
         * rejects the legacy frame/slice thread flags.
         */
        if (codecpar->codec_id != AV_CODEC_ID_VVC)
            decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        decoder->skip_loop_filter = AVDISCARD_ALL;
        decoder->skip_idct = AVDISCARD_ALL;
    }

    failed_stage = "open decoder";
    ret = avcodec_open2(decoder, codec, NULL);
    if (ret < 0)
        goto done;

    failed_stage = "start VACHUNK writer";
    ret = ff_voidplayer_vachunk_start_memory(
        decoder->width > 0 ? decoder->width : codecpar->width,
        decoder->height > 0 ? decoder->height : codecpar->height,
        vachunk_codec);
    if (ret < 0)
        goto done;
    writer_started = 1;
    if (options->has_start_frame || options->has_end_frame) {
        uint64_t start_frame = options->has_start_frame ? options->start_frame : 0;
        uint64_t end_frame = options->has_end_frame ? options->end_frame : UINT64_MAX;
        ret = ff_voidplayer_vachunk_set_frame_window(start_frame, end_frame);
        if (ret < 0)
            goto done;
    }

    failed_stage = "decode packets";
    while ((ret = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == stream_index) {
            if (options->has_end_frame &&
                options->end_frame <= UINT64_MAX - 64 &&
                video_packet_index > options->end_frame + 64) {
                av_packet_unref(packet);
                break;
            }
            void *coded_order_key = (void *)(uintptr_t)(video_packet_index + 1);
            packet->opaque = coded_order_key;
            video_packet_index++;

            if (bsf) {
                ret = av_bsf_send_packet(bsf, packet);
                if (ret < 0) {
                    av_packet_unref(packet);
                    goto done;
                }

                while ((ret = av_bsf_receive_packet(bsf, filtered_packet)) >= 0) {
                    filtered_packet->opaque = coded_order_key;
                    ret = send_packet_to_decoder(decoder, filtered_packet, frame);
                    av_packet_unref(filtered_packet);
                    if (ret < 0) {
                        av_packet_unref(packet);
                        goto done;
                    }
                }
                if (ret == AVERROR(EAGAIN))
                    ret = 0;
                if (ret < 0) {
                    av_packet_unref(packet);
                    goto done;
                }
            } else {
                ret = send_packet_to_decoder(decoder, packet, frame);
                if (ret < 0) {
                    av_packet_unref(packet);
                    goto done;
                }
            }
        }
        av_packet_unref(packet);
    }

    if (ret == AVERROR_EOF)
        ret = 0;
    if (ret < 0)
        goto done;

    failed_stage = "flush decoder";
    if (bsf) {
        failed_stage = "flush bitstream filter";
        ret = av_bsf_send_packet(bsf, NULL);
        if (ret < 0)
            goto done;
        while ((ret = av_bsf_receive_packet(bsf, filtered_packet)) >= 0) {
            ret = send_packet_to_decoder(decoder, filtered_packet, frame);
            av_packet_unref(filtered_packet);
            if (ret < 0)
                goto done;
        }
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
            ret = 0;
        if (ret < 0)
            goto done;
    }

    failed_stage = "flush decoder";
    ret = send_packet_to_decoder(decoder, NULL, frame);
    if (ret < 0)
        goto done;

    {
        uint32_t frames = ff_voidplayer_vachunk_frame_count();
        if (frames == 0) {
            fprintf(stderr, "Decoder produced no VACHUNK frame records.\n");
            ret = 22;
            goto done;
        }
        failed_stage = "finish VACHUNK writer";
        ret = ff_voidplayer_vachunk_finish_vachunk(
            options->vachunk,
            (uint32_t)options->start_frame,
            (uint32_t)options->end_frame,
            options->base_revision,
            options->generator_revision);
        writer_started = 0;
        if (ret < 0)
            goto done;
        fprintf(stdout,
                "vachunk=%s\nframes=%u\n",
                options->vachunk,
                frames);
    }

done:
    if (ret < 0) {
        if (writer_started)
            ff_voidplayer_vachunk_abort();
        error_text(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "VACHUNK generation failed during %s: %s\n", failed_stage, errbuf);
        ret = 23;
    }
    av_frame_free(&frame);
    av_packet_free(&filtered_packet);
    av_packet_free(&packet);
    av_bsf_free(&bsf);
    avcodec_free_context(&decoder);
    return ret;
}

static int analyze_input(const AnalyzerOptions *options, enum AVCodecID expected_codec)
{
    AVFormatContext *format = NULL;
    int stream_index = -1;
    int ret;

    ret = open_input(options, expected_codec, &format, &stream_index);
    if (ret != 0)
        return ret;

    if (options->probe_only) {
        avformat_close_input(&format);
        return 0;
    }

    ret = decode_vachunk(options, format, stream_index);
    avformat_close_input(&format);
    return ret;
}

static int analyzer_main(int argc, char **argv)
{
    AnalyzerOptions options;
    enum AVCodecID expected_codec = AV_CODEC_ID_NONE;
    int ret;

    ret = parse_args(argc, argv, &options);
    if (ret > 0)
        return 0;
    if (ret < 0)
        return 1;

    if (codec_id_from_name(options.codec, &expected_codec) < 0) {
        fprintf(stderr, "Unsupported codec name: %s\n", options.codec);
        print_usage(stderr);
        return 2;
    }

    return analyze_input(&options, expected_codec);
}

#ifdef _WIN32
static char *utf8_from_utf16_arg(const wchar_t *wide)
{
    char *utf8;
    int length;

    if (!wide)
        return NULL;

    length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0)
        return NULL;

    utf8 = (char *)malloc((size_t)length);
    if (!utf8)
        return NULL;

    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, length, NULL, NULL) <= 0) {
        free(utf8);
        return NULL;
    }

    return utf8;
}

int wmain(int argc, wchar_t **wargv)
{
    char **argv;
    int i;
    int ret;

    argv = (char **)calloc((size_t)argc + 1, sizeof(*argv));
    if (!argv) {
        fprintf(stderr, "Out of memory while converting command line.\n");
        return 1;
    }

    for (i = 0; i < argc; ++i) {
        argv[i] = utf8_from_utf16_arg(wargv[i]);
        if (!argv[i]) {
            fprintf(stderr, "Failed to convert command line argument %d to UTF-8.\n", i);
            ret = 1;
            goto done;
        }
    }

    ret = analyzer_main(argc, argv);

done:
    for (i = 0; i < argc; ++i)
        free(argv[i]);
    free(argv);
    return ret;
}
#else
int main(int argc, char **argv)
{
    return analyzer_main(argc, argv);
}
#endif

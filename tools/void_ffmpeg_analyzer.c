/*
 * VoidPlayer FFmpeg analysis tool.
 *
 * H.265 and H.264 use decoder-internal hooks to emit real VBS4 payloads.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/codec_id.h"
#include "libavcodec/codec_par.h"
#include "libavcodec/packet.h"
#include "libavcodec/voidplayer_vbs4.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/cpu.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"

typedef struct AnalyzerOptions {
    const char *codec;
    const char *input;
    const char *vbs4;
    int probe_only;
} AnalyzerOptions;

static void print_usage(FILE *out)
{
    fprintf(out,
            "Usage: void_ffmpeg_analyzer --codec <codec> --input <path> [--probe-only | --vbs4 <path>]\n"
            "\n"
            "Supported codec names: hevc, h265, h264\n"
            "\n"
            "VBS4 generation emits real decoder-derived HEVCCU1/H264MB1 payloads with zstd block compression.\n");
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

    return -1;
}

static uint16_t vbs4_codec_from_avcodec(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return VOIDPLAYER_VBS4_CODEC_H264;
    case AV_CODEC_ID_HEVC:
        return VOIDPLAYER_VBS4_CODEC_HEVC;
    default:
        return 0;
    }
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
        if (!strcmp(argv[i], "--vbs4") && i + 1 < argc) {
            options->vbs4 = argv[++i];
            continue;
        }

        fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
        print_usage(stderr);
        return -1;
    }

    if (!options->codec || !options->input || (!options->probe_only && !options->vbs4)) {
        fprintf(stderr, "Missing required arguments.\n");
        print_usage(stderr);
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

static int decode_vbs4(const AnalyzerOptions *options,
                       AVFormatContext *format,
                       int stream_index)
{
    AVStream *stream = format->streams[stream_index];
    AVCodecParameters *codecpar = stream->codecpar;
    const AVCodec *codec = NULL;
    AVCodecContext *decoder = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    uint16_t vbs4_codec = vbs4_codec_from_avcodec(codecpar->codec_id);
    int writer_started = 0;
    int ret = 0;

    if (!vbs4_codec) {
        fprintf(stderr, "VBS4 generation is only implemented for H.265/H.264 in this analyzer build.\n");
        return 26;
    }

    codec = avcodec_find_decoder_by_name(software_decoder_name(codecpar->codec_id));
    if (!codec)
        codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "No decoder found for codec: %s\n", avcodec_get_name(codecpar->codec_id));
        return 21;
    }

    decoder = avcodec_alloc_context3(codec);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!decoder || !packet || !frame) {
        ret = AVERROR(ENOMEM);
        goto done;
    }

    ret = avcodec_parameters_to_context(decoder, codecpar);
    if (ret < 0)
        goto done;
    decoder->pkt_timebase = stream->time_base;

    {
        int threads = av_cpu_count();
        if (threads < 2)
            threads = 2;
        if (threads > 16)
            threads = 16;
        decoder->thread_count = threads;
        decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        decoder->skip_loop_filter = AVDISCARD_ALL;
        decoder->skip_idct = AVDISCARD_ALL;
    }

    ret = avcodec_open2(decoder, codec, NULL);
    if (ret < 0)
        goto done;

    ret = ff_voidplayer_vbs4_start(options->vbs4,
                                   decoder->width > 0 ? decoder->width : codecpar->width,
                                   decoder->height > 0 ? decoder->height : codecpar->height,
                                   vbs4_codec);
    if (ret < 0)
        goto done;
    writer_started = 1;

    while ((ret = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == stream_index) {
            ret = avcodec_send_packet(decoder, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                goto done;
            }
            ret = receive_frames(decoder, frame);
            if (ret < 0) {
                av_packet_unref(packet);
                goto done;
            }
        }
        av_packet_unref(packet);
    }

    if (ret == AVERROR_EOF)
        ret = 0;
    if (ret < 0)
        goto done;

    ret = avcodec_send_packet(decoder, NULL);
    if (ret >= 0)
        ret = receive_frames(decoder, frame);
    if (ret < 0)
        goto done;

    {
        uint32_t frames = ff_voidplayer_vbs4_frame_count();
        if (frames == 0) {
            fprintf(stderr, "Decoder produced no VBS4 frame records.\n");
            ret = 22;
            goto done;
        }
        ret = ff_voidplayer_vbs4_finish();
        writer_started = 0;
        if (ret < 0)
            goto done;
        fprintf(stdout, "vbs4=%s\nframes=%u\n", options->vbs4, frames);
    }

done:
    if (ret < 0) {
        if (writer_started)
            ff_voidplayer_vbs4_abort();
        error_text(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "VBS4 generation failed: %s\n", errbuf);
        ret = 23;
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
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

    ret = decode_vbs4(options, format, stream_index);
    avformat_close_input(&format);
    return ret;
}

int main(int argc, char **argv)
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

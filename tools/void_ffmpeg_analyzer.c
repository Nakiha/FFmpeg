/*
 * VoidPlayer FFmpeg analysis tool.
 *
 * This first stage proves the Windows/MSVC FFmpeg build and command-line
 * contract. Codec-specific VBS3 emission is added in later patches.
 */

#include <stdio.h>
#include <string.h>

#include "libavcodec/codec_id.h"
#include "libavcodec/codec_par.h"
#include "libavformat/avformat.h"
#include "libavutil/error.h"

typedef struct AnalyzerOptions {
    const char *codec;
    const char *input;
    const char *vbs3;
    int probe_only;
} AnalyzerOptions;

static void print_usage(FILE *out)
{
    fprintf(out,
            "Usage: void_ffmpeg_analyzer --codec <codec> --input <path> [--probe-only | --vbs3 <path>]\n"
            "\n"
            "Supported codec names: hevc, h265, h264, av1, vp9, mpeg2, mpeg2video\n"
            "\n"
            "This build can probe streams. VBS3 emission is not implemented yet.\n");
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
    if (!strcmp(name, "av1")) {
        *codec_id = AV_CODEC_ID_AV1;
        return 0;
    }
    if (!strcmp(name, "vp9")) {
        *codec_id = AV_CODEC_ID_VP9;
        return 0;
    }
    if (!strcmp(name, "mpeg2") || !strcmp(name, "mpeg2video")) {
        *codec_id = AV_CODEC_ID_MPEG2VIDEO;
        return 0;
    }

    return -1;
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
        if (!strcmp(argv[i], "--vbs3") && i + 1 < argc) {
            options->vbs3 = argv[++i];
            continue;
        }

        fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
        print_usage(stderr);
        return -1;
    }

    if (!options->codec || !options->input || (!options->probe_only && !options->vbs3)) {
        fprintf(stderr, "Missing required arguments.\n");
        print_usage(stderr);
        return -1;
    }

    return 0;
}

static int probe_input(const AnalyzerOptions *options, enum AVCodecID expected_codec)
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

    avformat_close_input(&format);
    return 0;
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

    ret = probe_input(&options, expected_codec);
    if (ret != 0)
        return ret;

    if (options.probe_only)
        return 0;

    fprintf(stderr,
            "VBS3 emission is not implemented yet. Refusing to write placeholder output: %s\n",
            options.vbs3);
    return 20;
}

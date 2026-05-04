/*
 * VoidPlayer FFmpeg analysis tool.
 *
 * This tool owns the command-line contract used by the main VoidPlayer repo.
 * H.265 and H.264 use decoder-internal hooks to emit real VBS3 CU payloads.
 * Less important codecs still use frame summaries with an empty CU section.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/codec_id.h"
#include "libavcodec/codec_par.h"
#include "libavcodec/packet.h"
#include "libavcodec/voidplayer_vbs3.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/cpu.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"

typedef struct AnalyzerOptions {
    const char *codec;
    const char *input;
    const char *vbs3;
    int probe_only;
} AnalyzerOptions;

#pragma pack(push, 1)
typedef struct Vbs3Header {
    char     magic[4];
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
    uint32_t section_count;
    uint64_t section_table_offset;
    uint64_t file_size;
    uint64_t content_revision;
    uint64_t reserved;
} Vbs3Header;

typedef struct Vbs3SectionEntry {
    char     type[4];
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;
    uint32_t entry_count;
    uint64_t checksum;
    uint64_t reserved;
} Vbs3SectionEntry;

typedef struct Vbs3FrameSummary {
    int32_t  poc;
    uint32_t coded_order;
    uint32_t vcl_nalu_index;
    uint32_t flags;
    uint8_t  temporal_id;
    uint8_t  slice_type;
    uint8_t  nal_unit_type;
    uint8_t  avg_qp;
    uint8_t  num_ref_l0;
    uint8_t  num_ref_l1;
    uint8_t  qp_min;
    uint8_t  qp_max;
    int32_t  ref_pocs_l0[15];
    int32_t  ref_pocs_l1[15];
    uint32_t num_cus;
    uint32_t cu_index_entry;
    uint32_t reserved[2];
} Vbs3FrameSummary;

typedef struct Vbs3CuIndexEntry {
    uint64_t offset;
    uint64_t byte_size;
    uint32_t cu_count;
    uint32_t flags;
} Vbs3CuIndexEntry;
#pragma pack(pop)

typedef struct SummaryList {
    Vbs3FrameSummary *items;
    uint32_t count;
    uint32_t capacity;
} SummaryList;

static void print_usage(FILE *out)
{
    fprintf(out,
            "Usage: void_ffmpeg_analyzer --codec <codec> --input <path> [--probe-only | --vbs3 <path>]\n"
            "\n"
            "Supported codec names: hevc, h265, h264, av1, vp9, mpeg2, mpeg2video\n"
            "\n"
            "VBS3 generation currently supports hevc/h265, h264, vp9, and mpeg2/mpeg2video.\n"
            "H.265/H.264 emit CUBL records; VP9/MPEG-2 are frame-summary only. AV1 is probe-only until a real software decode path is added.\n");
}

static void error_text(int errnum, char *buffer, size_t buffer_size)
{
    if (av_strerror(errnum, buffer, buffer_size) < 0)
        snprintf(buffer, buffer_size, "error %d", errnum);
}

static void set_fourcc(char dst[4], const char src[4])
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
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

static int append_summary(SummaryList *list, const Vbs3FrameSummary *summary)
{
    Vbs3FrameSummary *new_items;
    uint32_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity ? list->capacity * 2 : 256;
        if (new_capacity < list->capacity)
            return AVERROR(ENOMEM);
        new_items = av_realloc_array(list->items, new_capacity, sizeof(*list->items));
        if (!new_items)
            return AVERROR(ENOMEM);
        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count++] = *summary;
    return 0;
}

static uint8_t slice_type_from_picture(enum AVPictureType pict_type, int is_key)
{
    switch (pict_type) {
    case AV_PICTURE_TYPE_B:
        return 0;
    case AV_PICTURE_TYPE_P:
        return 1;
    case AV_PICTURE_TYPE_I:
        return 2;
    default:
        return is_key ? 2 : 1;
    }
}

static uint8_t nal_type_from_frame(enum AVCodecID codec_id, uint8_t slice_type, int is_key)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return is_key ? 5 : 1;
    case AV_CODEC_ID_HEVC:
        return is_key ? 19 : 1;
    case AV_CODEC_ID_MPEG2VIDEO:
        return 1;
    case AV_CODEC_ID_AV1:
    case AV_CODEC_ID_VP9:
    default:
        return slice_type == 2 ? 0 : 1;
    }
}

static uint8_t qp_from_frame(const AVFrame *frame)
{
    int qp;

    if (!frame || frame->quality <= 0)
        return 0;
    if (frame->quality <= 63)
        return (uint8_t)frame->quality;

    qp = (frame->quality + FF_QP2LAMBDA / 2) / FF_QP2LAMBDA;
    if (qp < 0)
        qp = 0;
    if (qp > 63)
        qp = 63;
    return (uint8_t)qp;
}

static const char *software_decoder_name(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return "h264";
    case AV_CODEC_ID_HEVC:
        return "hevc";
    case AV_CODEC_ID_AV1:
        return "av1";
    case AV_CODEC_ID_VP9:
        return "vp9";
    case AV_CODEC_ID_MPEG2VIDEO:
        return "mpeg2video";
    default:
        return NULL;
    }
}

static int collect_frame_summary(SummaryList *summaries,
                                 const AVCodecContext *decoder,
                                 const AVFrame *frame)
{
    Vbs3FrameSummary summary;
    uint8_t qp;
    int is_key;
    int i;

    memset(&summary, 0, sizeof(summary));

    is_key = !!(frame->flags & AV_FRAME_FLAG_KEY);
    summary.poc = (int32_t)summaries->count;
    summary.coded_order = summaries->count;
    summary.vcl_nalu_index = 0xFFFFFFFFu;
    summary.flags = is_key ? 1u : 0u;
    summary.temporal_id = 0;
    summary.slice_type = slice_type_from_picture(frame->pict_type, is_key);
    summary.nal_unit_type = nal_type_from_frame(decoder->codec_id, summary.slice_type, is_key);
    qp = qp_from_frame(frame);
    summary.avg_qp = qp;
    summary.num_ref_l0 = 0;
    summary.num_ref_l1 = 0;
    summary.qp_min = 0;
    summary.qp_max = 0;
    for (i = 0; i < 15; ++i) {
        summary.ref_pocs_l0[i] = -1;
        summary.ref_pocs_l1[i] = -1;
    }
    summary.num_cus = 0;
    summary.cu_index_entry = summaries->count;

    return append_summary(summaries, &summary);
}

static int receive_frames(AVCodecContext *decoder, AVFrame *frame, SummaryList *summaries)
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

        ret = summaries ? collect_frame_summary(summaries, decoder, frame) : 0;
        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }
}

static Vbs3SectionEntry make_section(const char type[4],
                                     uint64_t offset,
                                     uint64_t size,
                                     uint32_t entry_size,
                                     uint32_t entry_count)
{
    Vbs3SectionEntry entry;

    memset(&entry, 0, sizeof(entry));
    set_fourcc(entry.type, type);
    entry.offset = offset;
    entry.size = size;
    entry.entry_size = entry_size;
    entry.entry_count = entry_count;
    return entry;
}

static int write_exact(FILE *file, const void *data, size_t size)
{
    return fwrite(data, 1, size, file) == size ? 0 : AVERROR(EIO);
}

static int write_empty_cuid(FILE *file, uint32_t frame_count)
{
    Vbs3CuIndexEntry entry;
    uint32_t i;

    memset(&entry, 0, sizeof(entry));
    for (i = 0; i < frame_count; ++i) {
        if (write_exact(file, &entry, sizeof(entry)) < 0)
            return AVERROR(EIO);
    }
    return 0;
}

static int write_vbs3_file(const char *path,
                           uint32_t width,
                           uint32_t height,
                           const SummaryList *summaries)
{
    const uint32_t section_count = 3;
    const uint64_t cubl_offset = sizeof(Vbs3Header);
    const uint64_t cubl_size = 0;
    const uint64_t fsum_offset = cubl_offset + cubl_size;
    const uint64_t fsum_size = (uint64_t)summaries->count * sizeof(Vbs3FrameSummary);
    const uint64_t cuid_offset = fsum_offset + fsum_size;
    const uint64_t cuid_size = (uint64_t)summaries->count * sizeof(Vbs3CuIndexEntry);
    const uint64_t section_table_offset = cuid_offset + cuid_size;
    const uint64_t file_size = section_table_offset + section_count * sizeof(Vbs3SectionEntry);
    Vbs3SectionEntry sections[3];
    Vbs3Header header;
    FILE *file;
    int ret = 0;

    memset(&header, 0, sizeof(header));
    set_fourcc(header.magic, "VBS3");
    header.version_major = 3;
    header.version_minor = 0;
    header.header_size = sizeof(Vbs3Header);
    header.section_entry_size = sizeof(Vbs3SectionEntry);
    header.width = width;
    header.height = height;
    header.frame_count = summaries->count;
    header.section_count = section_count;
    header.section_table_offset = section_table_offset;
    header.file_size = file_size;

    sections[0] = make_section("FSUM", fsum_offset, fsum_size,
                               sizeof(Vbs3FrameSummary), summaries->count);
    sections[1] = make_section("CUID", cuid_offset, cuid_size,
                               sizeof(Vbs3CuIndexEntry), summaries->count);
    sections[2] = make_section("CUBL", cubl_offset, cubl_size, 0, summaries->count);

    file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Failed to open VBS3 output: %s\n", path);
        return 30;
    }

    if (write_exact(file, &header, sizeof(header)) < 0 ||
        (fsum_size > 0 && write_exact(file, summaries->items, (size_t)fsum_size) < 0) ||
        write_empty_cuid(file, summaries->count) < 0 ||
        write_exact(file, sections, sizeof(sections)) < 0) {
        fprintf(stderr, "Failed to write VBS3 output: %s\n", path);
        ret = 31;
    }

    if (fclose(file) != 0 && ret == 0) {
        fprintf(stderr, "Failed to close VBS3 output: %s\n", path);
        ret = 32;
    }

    return ret;
}

static int decode_vbs3(const AnalyzerOptions *options,
                       AVFormatContext *format,
                       int stream_index)
{
    AVStream *stream = format->streams[stream_index];
    AVCodecParameters *codecpar = stream->codecpar;
    const AVCodec *codec = NULL;
    AVCodecContext *decoder = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    SummaryList summaries = { 0 };
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    int instrumented_vbs3 = 0;
    int instrumented_started = 0;
    int ret = 0;

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
    instrumented_vbs3 = codecpar->codec_id == AV_CODEC_ID_HEVC ||
                        codecpar->codec_id == AV_CODEC_ID_H264;
    if (instrumented_vbs3) {
        int threads = av_cpu_count();

        if (threads < 2)
            threads = 2;
        if (threads > 16)
            threads = 16;
        decoder->thread_count = threads;
        decoder->thread_type = FF_THREAD_SLICE;
    }

    ret = avcodec_open2(decoder, codec, NULL);
    if (ret < 0)
        goto done;

    if (instrumented_vbs3) {
        ret = ff_voidplayer_vbs3_start(options->vbs3,
                                       decoder->width > 0 ? decoder->width : codecpar->width,
                                       decoder->height > 0 ? decoder->height : codecpar->height);
        if (ret < 0)
            goto done;
        instrumented_started = 1;
    }

    while ((ret = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == stream_index) {
            ret = avcodec_send_packet(decoder, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                goto done;
            }
            ret = receive_frames(decoder, frame, instrumented_vbs3 ? NULL : &summaries);
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
        ret = receive_frames(decoder, frame, instrumented_vbs3 ? NULL : &summaries);
    if (ret < 0)
        goto done;

    if (instrumented_vbs3) {
        uint32_t frames = ff_voidplayer_vbs3_frame_count();
        if (frames == 0) {
            fprintf(stderr, "Decoder produced no VBS3 frame records.\n");
            ret = 22;
            goto done;
        }
        ret = ff_voidplayer_vbs3_finish();
        instrumented_started = 0;
        if (ret < 0)
            goto done;
        fprintf(stdout, "vbs3=%s\nframes=%u\n", options->vbs3, frames);
        goto done;
    }

    if (summaries.count == 0) {
        fprintf(stderr, "Decoder produced no frames.\n");
        ret = 22;
        goto done;
    }

    ret = write_vbs3_file(options->vbs3,
                          decoder->width > 0 ? decoder->width : codecpar->width,
                          decoder->height > 0 ? decoder->height : codecpar->height,
                          &summaries);
    if (ret == 0) {
        fprintf(stdout, "vbs3=%s\nframes=%u\n", options->vbs3, summaries.count);
    }

done:
    if (ret < 0) {
        if (instrumented_started)
            ff_voidplayer_vbs3_abort();
        error_text(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "VBS3 generation failed: %s\n", errbuf);
        ret = 23;
    }
    av_freep(&summaries.items);
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

    if (expected_codec == AV_CODEC_ID_AV1) {
        fprintf(stderr,
                "AV1 VBS3 generation is disabled in this build because the bundled FFmpeg AV1 decoder requires hardware acceleration. Add libdav1d/libaom or native software decode support first.\n");
        ret = 26;
    } else {
        ret = decode_vbs3(options, format, stream_index);
    }
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

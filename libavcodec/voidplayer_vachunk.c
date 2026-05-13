#include "voidplayer_vachunk.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#else
#include <windows.h>
#endif

#if defined(VOIDPLAYER_VACHUNK_ZSTD)
#include "zstd.h"
#endif

#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"

#define VACHUNK_PROFILE_FIRST 1u
#define VACHUNK_COMPRESSION_NONE 0u
#define VACHUNK_COMPRESSION_ZSTD 1u

#define VACHUNK_ENC_RAW 0u
#define VACHUNK_ENC_BITSET 1u
#define VACHUNK_ENC_ULEB128 2u
#define VACHUNK_ENC_SLEB128_ZIGZAG 3u
#define VACHUNK_ENC_FRAME_PREFIX_U32 7u

#define VACHUNK_STREAM_FRAME_PREFIX 1u

#define VACHUNK_HEVC_X 2u
#define VACHUNK_HEVC_Y 3u
#define VACHUNK_HEVC_LOG2_W 4u
#define VACHUNK_HEVC_LOG2_H 5u
#define VACHUNK_HEVC_DEPTH 6u
#define VACHUNK_HEVC_PRED_MODE 7u
#define VACHUNK_HEVC_QP_DELTA 8u
#define VACHUNK_HEVC_INTRA_MODE 9u
#define VACHUNK_HEVC_MIP_FLAG 10u
#define VACHUNK_HEVC_ISP_MODE 11u
#define VACHUNK_HEVC_SKIP_FLAG 12u
#define VACHUNK_HEVC_MERGE_FLAG 13u
#define VACHUNK_HEVC_INTER_DIR 14u
#define VACHUNK_HEVC_MV_L0_X 15u
#define VACHUNK_HEVC_MV_L0_Y 16u
#define VACHUNK_HEVC_MV_L1_X 17u
#define VACHUNK_HEVC_MV_L1_Y 18u
#define VACHUNK_HEVC_REF_L0 19u
#define VACHUNK_HEVC_REF_L1 20u

#define VACHUNK_H264_IS_INTRA 2u
#define VACHUNK_H264_SKIP_FLAG 3u
#define VACHUNK_H264_MERGE_FLAG 4u
#define VACHUNK_H264_INTER_DIR 5u
#define VACHUNK_H264_QP_DELTA 6u
#define VACHUNK_H264_INTRA_MODE 7u
#define VACHUNK_H264_REF_L0 8u
#define VACHUNK_H264_REF_L1 9u
#define VACHUNK_H264_MV_L0_X 10u
#define VACHUNK_H264_MV_L0_Y 11u
#define VACHUNK_H264_MV_L1_X 12u
#define VACHUNK_H264_MV_L1_Y 13u

#define VACHUNK_VERSION_MAJOR 1u
#define VACHUNK_VERSION_MINOR 0u
#define VACHUNK_KIND_OVERLAY 3u
#define VACHUNK_OVERLAY_FRAME_FLAG_COMPLETE 0x00000001u
#define VACHUNK_OVERLAY_FRAME_FLAG_EXACT 0x00000002u
#define VACHUNK_OVERLAY_FEATURE_FLAGS 0x000000000000011full

#pragma pack(push, 1)
typedef struct VachunkArchiveHeader {
    char     magic[4];
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint16_t codec;
    uint16_t profile;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
    uint32_t block_count;
    uint32_t section_count;
    uint32_t reserved0;
    uint64_t section_table_offset;
    uint64_t file_size;
    uint64_t content_revision;
    uint64_t reserved1;
    uint32_t reserved2;
} VachunkArchiveHeader;

typedef struct VachunkArchiveSectionEntry {
    char     type[4];
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;
    uint32_t entry_count;
    uint64_t checksum;
    uint64_t reserved0;
    uint64_t reserved1;
} VachunkArchiveSectionEntry;

typedef struct VachunkFrameSummary {
    int32_t  poc;
    uint32_t coded_order;
    uint32_t vcl_unit_index;
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
} VachunkFrameSummary;

typedef struct VachunkArchiveFrameIndexEntry {
    uint32_t block_index;
    uint32_t local_frame;
    uint32_t first_record;
    uint32_t record_count;
    uint32_t flags;
    uint32_t reserved;
} VachunkArchiveFrameIndexEntry;

typedef struct VachunkArchiveBlockIndexEntry {
    uint32_t first_frame;
    uint32_t frame_count;
    uint32_t first_record;
    uint32_t record_count;
    uint64_t payload_offset;
    uint64_t payload_size;
    uint64_t decoded_size;
    uint16_t codec_profile;
    uint16_t compression;
    uint32_t flags;
    uint64_t checksum;
    uint64_t reserved;
} VachunkArchiveBlockIndexEntry;

typedef struct VachunkArchiveDecodedBlockHeader {
    char     magic[4];
    uint16_t header_size;
    uint16_t stream_entry_size;
    uint16_t codec_profile;
    uint16_t stream_count;
    uint32_t frame_count;
    uint32_t record_count;
    uint32_t flags;
    uint64_t reserved;
} VachunkArchiveDecodedBlockHeader;

typedef struct VachunkArchiveStreamEntry {
    uint16_t stream_id;
    uint16_t encoding;
    uint32_t offset;
    uint32_t size;
    uint32_t value_count;
    uint32_t flags;
} VachunkArchiveStreamEntry;

typedef struct VachunkHeader {
    char     magic[4];
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint32_t section_count;
    uint16_t kind;
    uint16_t codec;
    uint64_t feature_flags;
    uint64_t base_content_revision;
    uint64_t generator_revision;
    uint16_t track_index;
    uint16_t compression;
    uint32_t start_frame;
    uint32_t end_frame;
    uint32_t start_packet;
    uint32_t end_packet;
    uint32_t start_unit;
    uint32_t end_unit;
    uint64_t section_table_offset;
    uint64_t file_size;
    uint64_t checksum;
    uint64_t reserved1[4];
} VachunkHeader;

typedef struct VachunkSectionEntry {
    char     type[4];
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;
    uint32_t entry_count;
    uint64_t decoded_size;
    uint64_t checksum;
    uint64_t reserved;
} VachunkSectionEntry;

typedef struct VachunkOverlayFrameIndexEntry {
    uint32_t frame_index;
    uint32_t first_unit;
    uint32_t unit_count;
    uint32_t first_stream_value;
    uint32_t flags;
    uint32_t reserved;
} VachunkOverlayFrameIndexEntry;

typedef struct VachunkCuIntra {
    uint8_t intra_mode;
    uint8_t mip_flag;
    uint8_t isp_mode;
} VachunkCuIntra;

typedef struct VachunkCuInter {
    uint8_t  skip;
    uint8_t  merge_flag;
    uint8_t  inter_dir;
    int16_t  mv_l0_x;
    int16_t  mv_l0_y;
    int16_t  mv_l1_x;
    int16_t  mv_l1_y;
    int8_t   ref_l0;
    int8_t   ref_l1;
} VachunkCuInter;

typedef struct VachunkPackedCuRecord {
    uint16_t x;
    uint16_t y;
    uint8_t  w;
    uint8_t  h;
    uint8_t  depth;
    uint8_t  qp;
    uint8_t  pred_mode;
    uint32_t bit_count;
    union {
        VachunkCuIntra intra;
        VachunkCuInter inter;
    } data;
} VachunkPackedCuRecord;
#pragma pack(pop)

typedef char VachunkArchiveHeaderMustBe80[(sizeof(VachunkArchiveHeader) == 80) ? 1 : -1];
typedef char VachunkArchiveSectionMustBe56[(sizeof(VachunkArchiveSectionEntry) == 56) ? 1 : -1];
typedef char VachunkSummaryMustBe160[(sizeof(VachunkFrameSummary) == 160) ? 1 : -1];
typedef char VachunkFidxMustBe24[(sizeof(VachunkArchiveFrameIndexEntry) == 24) ? 1 : -1];
typedef char VachunkBidxMustBe64[(sizeof(VachunkArchiveBlockIndexEntry) == 64) ? 1 : -1];
typedef char VachunkBlockHeaderMustBe32[(sizeof(VachunkArchiveDecodedBlockHeader) == 32) ? 1 : -1];
typedef char VachunkArchiveStreamEntryMustBe20[(sizeof(VachunkArchiveStreamEntry) == 20) ? 1 : -1];
typedef char VachunkChunkHeaderMustBe128[(sizeof(VachunkHeader) == 128) ? 1 : -1];
typedef char VachunkChunkSectionMustBe56[(sizeof(VachunkSectionEntry) == 56) ? 1 : -1];
typedef char VachunkOverlayFrameIndexMustBe24[(sizeof(VachunkOverlayFrameIndexEntry) == 24) ? 1 : -1];
typedef char VachunkPackedCuRecordMustBe26[(sizeof(VachunkPackedCuRecord) == 26) ? 1 : -1];

typedef struct VachunkCuRecord {
    uint16_t x;
    uint16_t y;
    uint8_t  w;
    uint8_t  h;
    uint8_t  depth;
    uint8_t  qp;
    uint8_t  pred_mode;
    uint32_t bit_count;
    uint8_t  intra_mode;
    uint8_t  mip_flag;
    uint8_t  isp_mode;
    uint8_t  skip;
    uint8_t  merge_flag;
    uint8_t  inter_dir;
    int16_t  mv_l0_x;
    int16_t  mv_l0_y;
    int16_t  mv_l1_x;
    int16_t  mv_l1_y;
    int8_t   ref_l0;
    int8_t   ref_l1;
} VachunkCuRecord;

typedef struct VachunkFrameBuffer {
    VachunkFrameSummary summary;
    VachunkCuRecord *records;
    uint32_t record_count;
    uint32_t record_capacity;
    uint64_t qp_sum;
    uint16_t first_x;
    uint16_t first_y;
    uint8_t first_w;
    uint8_t first_h;
    uint8_t first_depth;
    int has_first_cu;
    uintptr_t frame_identity;
    uint64_t coded_order_key;
    int has_coded_order_key;
} VachunkFrameBuffer;

typedef struct VachunkBlockFrame {
    uint32_t first_record;
    uint32_t record_count;
} VachunkBlockFrame;

typedef struct VachunkPendingBlock {
    uint32_t first_frame;
    uint32_t first_record;
    VachunkBlockFrame *frames;
    uint32_t frame_count;
    uint32_t frame_capacity;
    VachunkCuRecord *records;
    uint32_t record_count;
    uint32_t record_capacity;
} VachunkPendingBlock;

typedef struct VachunkBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} VachunkBuffer;

typedef struct VachunkStreamData {
    uint16_t id;
    uint16_t encoding;
    uint32_t value_count;
    VachunkBuffer bytes;
} VachunkStreamData;

typedef struct VachunkStreamList {
    VachunkStreamData *items;
    uint32_t count;
    uint32_t capacity;
} VachunkStreamList;

typedef struct VoidVachunkState {
    FILE *file;
    uint16_t codec;
    uint16_t profile;
    uint32_t width;
    uint32_t height;
    VachunkFrameBuffer *frames;
    uint32_t frame_count;
    uint32_t frame_capacity;
    VachunkArchiveFrameIndexEntry *frame_index;
    uint32_t frame_index_count;
    uint32_t frame_index_capacity;
    VachunkArchiveBlockIndexEntry *block_index;
    uint32_t block_index_count;
    uint32_t block_index_capacity;
    uint64_t total_records;
    uint64_t cpay_payload_offset;
    uint64_t cpay_bytes;
    int error;
    AVMutex lock;
    int lock_initialized;
    int active;
    int has_frame_window;
    uint64_t start_frame;
    uint64_t end_frame;
} VoidVachunkState;

static VoidVachunkState g_vachunk;

static FILE *open_utf8_file(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wide_path = NULL;
    wchar_t *wide_mode = NULL;
    FILE *file = NULL;
    int path_len;
    int mode_len;

    if (!path || !mode)
        return NULL;

    path_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    mode_len = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (path_len <= 0 || mode_len <= 0)
        return NULL;

    wide_path = (wchar_t *)av_malloc_array((size_t)path_len, sizeof(*wide_path));
    wide_mode = (wchar_t *)av_malloc_array((size_t)mode_len, sizeof(*wide_mode));
    if (!wide_path || !wide_mode)
        goto done;

    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, path_len) <= 0)
        goto done;
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, mode_len) <= 0)
        goto done;

    file = _wfopen(wide_path, wide_mode);

done:
    av_free(wide_path);
    av_free(wide_mode);
    return file;
#else
    return fopen(path, mode);
#endif
}

static void vachunk_lock(void)
{
    if (g_vachunk.lock_initialized)
        ff_mutex_lock(&g_vachunk.lock);
}

static void vachunk_unlock(void)
{
    if (g_vachunk.lock_initialized)
        ff_mutex_unlock(&g_vachunk.lock);
}

static void set_fourcc(char dst[4], const char src[4])
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

static int write_exact(FILE *file, const void *data, size_t size)
{
    if (!size)
        return 0;
    return fwrite(data, 1, size, file) == size ? 0 : AVERROR(EIO);
}

static int seek_file(FILE *file, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, (int64_t)offset, SEEK_SET);
#else
    return fseeko(file, (off_t)offset, SEEK_SET);
#endif
}

static int64_t tell_file(FILE *file)
{
#ifdef _WIN32
    return _ftelli64(file);
#else
    return ftello(file);
#endif
}

static int buffer_reserve(VachunkBuffer *buffer, size_t additional)
{
    size_t required;
    size_t new_capacity;
    uint8_t *new_data;

    if (additional > SIZE_MAX - buffer->size)
        return AVERROR(ENOMEM);
    required = buffer->size + additional;
    if (required <= buffer->capacity)
        return 0;

    new_capacity = buffer->capacity ? buffer->capacity * 2 : 4096;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }

    new_data = av_realloc(buffer->data, new_capacity);
    if (!new_data)
        return AVERROR(ENOMEM);
    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return 0;
}

static int buffer_append(VachunkBuffer *buffer, const void *data, size_t size)
{
    int ret;

    if (!size)
        return 0;
    ret = buffer_reserve(buffer, size);
    if (ret < 0)
        return ret;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static int buffer_append_u8(VachunkBuffer *buffer, uint8_t value)
{
    return buffer_append(buffer, &value, sizeof(value));
}

static int buffer_append_u32(VachunkBuffer *buffer, uint32_t value)
{
    uint8_t bytes[4];

    bytes[0] = (uint8_t)(value & 0xFF);
    bytes[1] = (uint8_t)((value >> 8) & 0xFF);
    bytes[2] = (uint8_t)((value >> 16) & 0xFF);
    bytes[3] = (uint8_t)((value >> 24) & 0xFF);
    return buffer_append(buffer, bytes, sizeof(bytes));
}

static int buffer_append_uleb(VachunkBuffer *buffer, uint32_t value)
{
    int ret;

    do {
        uint8_t byte = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value)
            byte |= 0x80;
        ret = buffer_append_u8(buffer, byte);
        if (ret < 0)
            return ret;
    } while (value);
    return 0;
}

static int buffer_append_sleb_zigzag(VachunkBuffer *buffer, int32_t value)
{
    uint32_t zigzag = ((uint32_t)value << 1) ^ (uint32_t)(value >> 31);
    return buffer_append_uleb(buffer, zigzag);
}

static int buffer_append_bit(VachunkBuffer *buffer, uint32_t index, int value)
{
    size_t byte_index = index >> 3;
    int ret;

    if (byte_index >= buffer->size) {
        ret = buffer_reserve(buffer, byte_index + 1 - buffer->size);
        if (ret < 0)
            return ret;
        memset(buffer->data + buffer->size, 0, byte_index + 1 - buffer->size);
        buffer->size = byte_index + 1;
    }
    if (value)
        buffer->data[byte_index] |= (uint8_t)(1u << (index & 7));
    return 0;
}

static uint8_t log2_size(uint8_t value)
{
    uint8_t result = 0;

    while (value > 1) {
        value >>= 1;
        result++;
    }
    return result;
}

static int stream_list_push(VachunkStreamList *streams,
                            uint16_t id,
                            uint16_t encoding,
                            uint32_t value_count,
                            VachunkBuffer *bytes)
{
    VachunkStreamData *new_items;
    uint32_t new_capacity;

    if (streams->count == streams->capacity) {
        new_capacity = streams->capacity ? streams->capacity * 2 : 16;
        if (new_capacity < streams->capacity)
            return AVERROR(ENOMEM);
        new_items = av_realloc_array(streams->items, new_capacity, sizeof(*streams->items));
        if (!new_items)
            return AVERROR(ENOMEM);
        memset(new_items + streams->capacity, 0,
               (new_capacity - streams->capacity) * sizeof(*new_items));
        streams->items = new_items;
        streams->capacity = new_capacity;
    }

    streams->items[streams->count].id = id;
    streams->items[streams->count].encoding = encoding;
    streams->items[streams->count].value_count = value_count;
    streams->items[streams->count].bytes = *bytes;
    memset(bytes, 0, sizeof(*bytes));
    streams->count++;
    return 0;
}

static void stream_list_free(VachunkStreamList *streams)
{
    uint32_t i;

    for (i = 0; i < streams->count; ++i)
        av_freep(&streams->items[i].bytes.data);
    av_freep(&streams->items);
    memset(streams, 0, sizeof(*streams));
}

static int append_frame_index(VachunkArchiveFrameIndexEntry entry)
{
    VachunkArchiveFrameIndexEntry *new_items;
    uint32_t new_capacity;

    if (g_vachunk.frame_index_count == g_vachunk.frame_index_capacity) {
        new_capacity = g_vachunk.frame_index_capacity ? g_vachunk.frame_index_capacity * 2 : 256;
        if (new_capacity < g_vachunk.frame_index_capacity)
            return AVERROR(ENOMEM);
        new_items = av_realloc_array(g_vachunk.frame_index, new_capacity, sizeof(*new_items));
        if (!new_items)
            return AVERROR(ENOMEM);
        g_vachunk.frame_index = new_items;
        g_vachunk.frame_index_capacity = new_capacity;
    }
    g_vachunk.frame_index[g_vachunk.frame_index_count++] = entry;
    return 0;
}

static int append_block_index(VachunkArchiveBlockIndexEntry entry)
{
    VachunkArchiveBlockIndexEntry *new_items;
    uint32_t new_capacity;

    if (g_vachunk.block_index_count == g_vachunk.block_index_capacity) {
        new_capacity = g_vachunk.block_index_capacity ? g_vachunk.block_index_capacity * 2 : 64;
        if (new_capacity < g_vachunk.block_index_capacity)
            return AVERROR(ENOMEM);
        new_items = av_realloc_array(g_vachunk.block_index, new_capacity, sizeof(*new_items));
        if (!new_items)
            return AVERROR(ENOMEM);
        g_vachunk.block_index = new_items;
        g_vachunk.block_index_capacity = new_capacity;
    }
    g_vachunk.block_index[g_vachunk.block_index_count++] = entry;
    return 0;
}

static void free_frames(void)
{
    uint32_t i;

    for (i = 0; i < g_vachunk.frame_count; ++i)
        av_freep(&g_vachunk.frames[i].records);
    av_freep(&g_vachunk.frames);
    g_vachunk.frame_count = 0;
    g_vachunk.frame_capacity = 0;
}

static void reset_state(void)
{
    int had_lock = g_vachunk.lock_initialized;

    if (g_vachunk.file)
        fclose(g_vachunk.file);
    free_frames();
    av_freep(&g_vachunk.frame_index);
    av_freep(&g_vachunk.block_index);
    if (had_lock)
        ff_mutex_destroy(&g_vachunk.lock);
    memset(&g_vachunk, 0, sizeof(g_vachunk));
}

static int ensure_frame_capacity(void)
{
    VachunkFrameBuffer *new_frames;
    uint32_t new_capacity;

    if (g_vachunk.frame_count < g_vachunk.frame_capacity)
        return 0;

    new_capacity = g_vachunk.frame_capacity ? g_vachunk.frame_capacity * 2 : 256;
    if (new_capacity < g_vachunk.frame_capacity)
        return AVERROR(ENOMEM);
    new_frames = av_realloc_array(g_vachunk.frames, new_capacity, sizeof(*g_vachunk.frames));
    if (!new_frames)
        return AVERROR(ENOMEM);
    memset(new_frames + g_vachunk.frame_capacity, 0,
           (new_capacity - g_vachunk.frame_capacity) * sizeof(*new_frames));
    g_vachunk.frames = new_frames;
    g_vachunk.frame_capacity = new_capacity;
    return 0;
}

static int ensure_record_capacity(VachunkFrameBuffer *frame)
{
    VachunkCuRecord *new_records;
    uint32_t new_capacity;

    if (frame->record_count < frame->record_capacity)
        return 0;

    new_capacity = frame->record_capacity ? frame->record_capacity * 2 : 1024;
    if (new_capacity < frame->record_capacity)
        return AVERROR(ENOMEM);
    new_records = av_realloc_array(frame->records, new_capacity, sizeof(*new_records));
    if (!new_records)
        return AVERROR(ENOMEM);
    frame->records = new_records;
    frame->record_capacity = new_capacity;
    return 0;
}

static void fill_summary(VachunkFrameSummary *summary, const VoidPlayerVachunkFrameInfo *info)
{
    int i;

    memset(summary, 0, sizeof(*summary));
    summary->poc = info->poc;
    summary->coded_order = g_vachunk.frame_count;
    summary->vcl_unit_index = 0xFFFFFFFFu;
    summary->temporal_id = info->temporal_id;
    summary->slice_type = info->slice_type;
    summary->nal_unit_type = info->nal_unit_type;
    summary->num_ref_l0 = info->num_ref_l0 > 15 ? 15 : info->num_ref_l0;
    summary->num_ref_l1 = info->num_ref_l1 > 15 ? 15 : info->num_ref_l1;
    summary->cu_index_entry = g_vachunk.frame_count;
    for (i = 0; i < 15; ++i) {
        summary->ref_pocs_l0[i] = i < summary->num_ref_l0 ? info->ref_pocs_l0[i] : -1;
        summary->ref_pocs_l1[i] = i < summary->num_ref_l1 ? info->ref_pocs_l1[i] : -1;
    }
}

static int frame_can_accept_cu(const VachunkFrameBuffer *frame,
                               const VoidPlayerVachunkFrameInfo *info,
                               uint16_t x,
                               uint16_t y,
                               uint8_t w,
                               uint8_t h,
                               uint8_t depth)
{
    if (info->expected_cus && frame->summary.num_cus >= info->expected_cus)
        return 0;
    if (info->frame_identity)
        return frame->frame_identity == info->frame_identity;
    if (!info->expected_cus && frame->has_first_cu &&
        frame->first_x == x &&
        frame->first_y == y &&
        frame->first_w == w &&
        frame->first_h == h &&
        frame->first_depth == depth)
        return 0;
    return 1;
}

static VachunkFrameBuffer *find_frame(const VoidPlayerVachunkFrameInfo *info,
                                   uint16_t x,
                                   uint16_t y,
                                   uint8_t w,
                                   uint8_t h,
                                   uint8_t depth)
{
    uint32_t i;

    for (i = g_vachunk.frame_count; i > 0; --i) {
        VachunkFrameBuffer *frame = &g_vachunk.frames[i - 1];
        if (frame->summary.poc == info->poc &&
            frame_can_accept_cu(frame, info, x, y, w, h, depth))
            return frame;
    }
    return NULL;
}

static VachunkFrameBuffer *get_or_create_frame(const VoidPlayerVachunkFrameInfo *info,
                                            uint16_t x,
                                            uint16_t y,
                                            uint8_t w,
                                            uint8_t h,
                                            uint8_t depth)
{
    VachunkFrameBuffer *frame;

    if (!info) {
        g_vachunk.error = AVERROR(EINVAL);
        return NULL;
    }

    frame = find_frame(info, x, y, w, h, depth);
    if (frame)
        return frame;

    g_vachunk.error = ensure_frame_capacity();
    if (g_vachunk.error)
        return NULL;

    frame = &g_vachunk.frames[g_vachunk.frame_count];
    memset(frame, 0, sizeof(*frame));
    fill_summary(&frame->summary, info);
    frame->frame_identity = info->frame_identity;
    frame->coded_order_key = info->coded_order_key;
    frame->has_coded_order_key = info->has_coded_order_key;
    g_vachunk.frame_count++;
    if (info->width)
        g_vachunk.width = info->width;
    if (info->height)
        g_vachunk.height = info->height;
    return frame;
}

static int compare_cu_raster(const void *lhs, const void *rhs)
{
    const VachunkCuRecord *a = lhs;
    const VachunkCuRecord *b = rhs;

    if (a->y != b->y)
        return a->y < b->y ? -1 : 1;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    return 0;
}

static int compare_frame_coded_order_key(const void *lhs, const void *rhs)
{
    const VachunkFrameBuffer *a = lhs;
    const VachunkFrameBuffer *b = rhs;

    if (a->coded_order_key != b->coded_order_key)
        return a->coded_order_key < b->coded_order_key ? -1 : 1;
    if (a->summary.coded_order != b->summary.coded_order)
        return a->summary.coded_order < b->summary.coded_order ? -1 : 1;
    return 0;
}

static int all_frames_have_coded_order_keys(void)
{
    uint32_t i;

    for (i = 0; i < g_vachunk.frame_count; ++i) {
        if (!g_vachunk.frames[i].has_coded_order_key)
            return 0;
    }
    return g_vachunk.frame_count > 0;
}

static int pending_reserve_frames(VachunkPendingBlock *block, uint32_t count)
{
    VachunkBlockFrame *new_frames;
    uint32_t new_capacity;

    if (count <= block->frame_capacity)
        return 0;
    new_capacity = block->frame_capacity ? block->frame_capacity * 2 : 256;
    while (new_capacity < count) {
        if (new_capacity > UINT32_MAX / 2) {
            new_capacity = count;
            break;
        }
        new_capacity *= 2;
    }
    new_frames = av_realloc_array(block->frames, new_capacity, sizeof(*new_frames));
    if (!new_frames)
        return AVERROR(ENOMEM);
    block->frames = new_frames;
    block->frame_capacity = new_capacity;
    return 0;
}

static int pending_reserve_records(VachunkPendingBlock *block, uint32_t count)
{
    VachunkCuRecord *new_records;
    uint32_t new_capacity;

    if (count <= block->record_capacity)
        return 0;
    new_capacity = block->record_capacity ? block->record_capacity * 2 : 4096;
    while (new_capacity < count) {
        if (new_capacity > UINT32_MAX / 2) {
            new_capacity = count;
            break;
        }
        new_capacity *= 2;
    }
    new_records = av_realloc_array(block->records, new_capacity, sizeof(*new_records));
    if (!new_records)
        return AVERROR(ENOMEM);
    block->records = new_records;
    block->record_capacity = new_capacity;
    return 0;
}

static void pending_clear(VachunkPendingBlock *block)
{
    block->first_frame = 0;
    block->first_record = 0;
    block->frame_count = 0;
    block->record_count = 0;
}

static void pending_free(VachunkPendingBlock *block)
{
    av_freep(&block->frames);
    av_freep(&block->records);
    memset(block, 0, sizeof(*block));
}

static uint64_t estimate_block_size(const VachunkPendingBlock *block, const VachunkFrameBuffer *frame)
{
    uint64_t records = (uint64_t)block->record_count + frame->record_count;
    uint64_t streams = g_vachunk.codec == VOIDPLAYER_VACHUNK_CODEC_H264 ? 13 : 20;

    return sizeof(VachunkArchiveDecodedBlockHeader) +
           streams * sizeof(VachunkArchiveStreamEntry) +
           records * 24u;
}

static int append_frame_to_pending(VachunkPendingBlock *block, uint32_t frame_index)
{
    VachunkFrameBuffer *frame = &g_vachunk.frames[frame_index];
    VachunkBlockFrame block_frame;
    int ret;

    ret = pending_reserve_frames(block, block->frame_count + 1);
    if (ret < 0)
        return ret;
    ret = pending_reserve_records(block, block->record_count + frame->record_count);
    if (ret < 0)
        return ret;

    if (block->frame_count == 0) {
        block->first_frame = frame_index;
        block->first_record = (uint32_t)g_vachunk.total_records;
    }

    block_frame.first_record = block->first_record + block->record_count;
    block_frame.record_count = frame->record_count;
    block->frames[block->frame_count++] = block_frame;
    if (frame->record_count) {
        memcpy(block->records + block->record_count, frame->records,
               (size_t)frame->record_count * sizeof(*frame->records));
        block->record_count += frame->record_count;
    }
    g_vachunk.total_records += frame->record_count;
    return 0;
}

static int add_common_inter_streams(VachunkStreamList *streams,
                                    const VachunkCuRecord *records,
                                    uint32_t record_count,
                                    int include_hevc_shape)
{
    VachunkBuffer x = { 0 };
    VachunkBuffer y = { 0 };
    VachunkBuffer log2w = { 0 };
    VachunkBuffer log2h = { 0 };
    VachunkBuffer depth = { 0 };
    VachunkBuffer pred_mode = { 0 };
    VachunkBuffer qp_delta = { 0 };
    VachunkBuffer intra_mode = { 0 };
    VachunkBuffer mip_flag = { 0 };
    VachunkBuffer isp_mode = { 0 };
    VachunkBuffer skip_flag = { 0 };
    VachunkBuffer merge_flag = { 0 };
    VachunkBuffer inter_dir = { 0 };
    VachunkBuffer mv_l0_x = { 0 };
    VachunkBuffer mv_l0_y = { 0 };
    VachunkBuffer mv_l1_x = { 0 };
    VachunkBuffer mv_l1_y = { 0 };
    VachunkBuffer ref_l0 = { 0 };
    VachunkBuffer ref_l1 = { 0 };
    int32_t prev_qp = 0;
    uint32_t i;
    int ret = 0;

#define CHECK(EXPR) do { ret = (EXPR); if (ret < 0) goto fail; } while (0)
    for (i = 0; i < record_count; ++i) {
        const VachunkCuRecord *cu = &records[i];
        if (include_hevc_shape) {
            CHECK(buffer_append_uleb(&x, cu->x));
            CHECK(buffer_append_uleb(&y, cu->y));
            CHECK(buffer_append_u8(&log2w, log2_size(cu->w)));
            CHECK(buffer_append_u8(&log2h, log2_size(cu->h)));
            CHECK(buffer_append_u8(&depth, cu->depth));
            CHECK(buffer_append_u8(&pred_mode, cu->pred_mode));
        }
        CHECK(buffer_append_sleb_zigzag(&qp_delta, (int32_t)cu->qp - prev_qp));
        prev_qp = cu->qp;
        CHECK(buffer_append_u8(&intra_mode, cu->intra_mode));
        CHECK(buffer_append_bit(&skip_flag, i, cu->skip != 0));
        CHECK(buffer_append_bit(&merge_flag, i, cu->merge_flag != 0));
        CHECK(buffer_append_u8(&inter_dir, cu->inter_dir));
        CHECK(buffer_append_sleb_zigzag(&mv_l0_x, cu->mv_l0_x));
        CHECK(buffer_append_sleb_zigzag(&mv_l0_y, cu->mv_l0_y));
        CHECK(buffer_append_sleb_zigzag(&mv_l1_x, cu->mv_l1_x));
        CHECK(buffer_append_sleb_zigzag(&mv_l1_y, cu->mv_l1_y));
        CHECK(buffer_append_u8(&ref_l0, (uint8_t)cu->ref_l0));
        CHECK(buffer_append_u8(&ref_l1, (uint8_t)cu->ref_l1));
        if (include_hevc_shape) {
            CHECK(buffer_append_bit(&mip_flag, i, cu->mip_flag != 0));
            CHECK(buffer_append_u8(&isp_mode, cu->isp_mode));
        }
    }

    if (include_hevc_shape) {
        CHECK(stream_list_push(streams, VACHUNK_HEVC_X, VACHUNK_ENC_ULEB128, record_count, &x));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_Y, VACHUNK_ENC_ULEB128, record_count, &y));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_LOG2_W, VACHUNK_ENC_RAW, record_count, &log2w));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_LOG2_H, VACHUNK_ENC_RAW, record_count, &log2h));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_DEPTH, VACHUNK_ENC_RAW, record_count, &depth));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_PRED_MODE, VACHUNK_ENC_RAW, record_count, &pred_mode));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_QP_DELTA, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &qp_delta));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_INTRA_MODE, VACHUNK_ENC_RAW, record_count, &intra_mode));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_MIP_FLAG, VACHUNK_ENC_BITSET, record_count, &mip_flag));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_ISP_MODE, VACHUNK_ENC_RAW, record_count, &isp_mode));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_SKIP_FLAG, VACHUNK_ENC_BITSET, record_count, &skip_flag));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_MERGE_FLAG, VACHUNK_ENC_BITSET, record_count, &merge_flag));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_INTER_DIR, VACHUNK_ENC_RAW, record_count, &inter_dir));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_MV_L0_X, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &mv_l0_x));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_MV_L0_Y, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &mv_l0_y));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_MV_L1_X, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &mv_l1_x));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_MV_L1_Y, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &mv_l1_y));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_REF_L0, VACHUNK_ENC_RAW, record_count, &ref_l0));
        CHECK(stream_list_push(streams, VACHUNK_HEVC_REF_L1, VACHUNK_ENC_RAW, record_count, &ref_l1));
    } else {
        VachunkBuffer is_intra = { 0 };
        for (i = 0; i < record_count; ++i)
            CHECK(buffer_append_bit(&is_intra, i, records[i].pred_mode != 0));
        CHECK(stream_list_push(streams, VACHUNK_H264_IS_INTRA, VACHUNK_ENC_BITSET, record_count, &is_intra));
        CHECK(stream_list_push(streams, VACHUNK_H264_SKIP_FLAG, VACHUNK_ENC_BITSET, record_count, &skip_flag));
        CHECK(stream_list_push(streams, VACHUNK_H264_MERGE_FLAG, VACHUNK_ENC_BITSET, record_count, &merge_flag));
        CHECK(stream_list_push(streams, VACHUNK_H264_INTER_DIR, VACHUNK_ENC_RAW, record_count, &inter_dir));
        CHECK(stream_list_push(streams, VACHUNK_H264_QP_DELTA, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &qp_delta));
        CHECK(stream_list_push(streams, VACHUNK_H264_INTRA_MODE, VACHUNK_ENC_RAW, record_count, &intra_mode));
        CHECK(stream_list_push(streams, VACHUNK_H264_REF_L0, VACHUNK_ENC_RAW, record_count, &ref_l0));
        CHECK(stream_list_push(streams, VACHUNK_H264_REF_L1, VACHUNK_ENC_RAW, record_count, &ref_l1));
        CHECK(stream_list_push(streams, VACHUNK_H264_MV_L0_X, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &mv_l0_x));
        CHECK(stream_list_push(streams, VACHUNK_H264_MV_L0_Y, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &mv_l0_y));
        CHECK(stream_list_push(streams, VACHUNK_H264_MV_L1_X, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &mv_l1_x));
        CHECK(stream_list_push(streams, VACHUNK_H264_MV_L1_Y, VACHUNK_ENC_SLEB128_ZIGZAG, record_count, &mv_l1_y));
    }
#undef CHECK
    return 0;

fail:
    av_freep(&x.data);
    av_freep(&y.data);
    av_freep(&log2w.data);
    av_freep(&log2h.data);
    av_freep(&depth.data);
    av_freep(&pred_mode.data);
    av_freep(&qp_delta.data);
    av_freep(&intra_mode.data);
    av_freep(&mip_flag.data);
    av_freep(&isp_mode.data);
    av_freep(&skip_flag.data);
    av_freep(&merge_flag.data);
    av_freep(&inter_dir.data);
    av_freep(&mv_l0_x.data);
    av_freep(&mv_l0_y.data);
    av_freep(&mv_l1_x.data);
    av_freep(&mv_l1_y.data);
    av_freep(&ref_l0.data);
    av_freep(&ref_l1.data);
    return ret;
}

static int build_decoded_block(const VachunkPendingBlock *block, VachunkBuffer *out)
{
    VachunkStreamList streams = { 0 };
    VachunkBuffer frame_prefix = { 0 };
    VachunkArchiveDecodedBlockHeader header;
    uint32_t prefix = 0;
    uint32_t payload_offset;
    uint32_t i;
    int ret = 0;

#define CHECK(EXPR) do { ret = (EXPR); if (ret < 0) goto done; } while (0)
    for (i = 0; i < block->frame_count; ++i) {
        CHECK(buffer_append_u32(&frame_prefix, prefix));
        prefix += block->frames[i].record_count;
    }
    CHECK(buffer_append_u32(&frame_prefix, prefix));
    CHECK(stream_list_push(&streams, VACHUNK_STREAM_FRAME_PREFIX, VACHUNK_ENC_FRAME_PREFIX_U32,
                           block->frame_count + 1, &frame_prefix));

    CHECK(add_common_inter_streams(&streams, block->records, block->record_count,
                                   g_vachunk.codec == VOIDPLAYER_VACHUNK_CODEC_HEVC));

    memset(&header, 0, sizeof(header));
    set_fourcc(header.magic, "BLK4");
    header.header_size = sizeof(VachunkArchiveDecodedBlockHeader);
    header.stream_entry_size = sizeof(VachunkArchiveStreamEntry);
    header.codec_profile = g_vachunk.profile;
    header.stream_count = (uint16_t)streams.count;
    header.frame_count = block->frame_count;
    header.record_count = block->record_count;
    CHECK(buffer_append(out, &header, sizeof(header)));
    CHECK(buffer_reserve(out, streams.count * sizeof(VachunkArchiveStreamEntry)));
    memset(out->data + out->size, 0, streams.count * sizeof(VachunkArchiveStreamEntry));
    out->size += streams.count * sizeof(VachunkArchiveStreamEntry);

    payload_offset = (uint32_t)out->size;
    for (i = 0; i < streams.count; ++i) {
        VachunkArchiveStreamEntry entry;

        memset(&entry, 0, sizeof(entry));
        entry.stream_id = streams.items[i].id;
        entry.encoding = streams.items[i].encoding;
        entry.offset = payload_offset;
        entry.size = (uint32_t)streams.items[i].bytes.size;
        entry.value_count = streams.items[i].value_count;
        memcpy(out->data + sizeof(VachunkArchiveDecodedBlockHeader) + i * sizeof(VachunkArchiveStreamEntry),
               &entry, sizeof(entry));
        CHECK(buffer_append(out, streams.items[i].bytes.data, streams.items[i].bytes.size));
        payload_offset += entry.size;
    }
#undef CHECK

done:
    av_freep(&frame_prefix.data);
    stream_list_free(&streams);
    return ret;
}

static int no_compression_requested(void)
{
    const char *env = getenv("VOIDPLAYER_VACHUNK_NO_COMPRESSION");
    return env && env[0] && strcmp(env, "0");
}

static int compress_zstd(const VachunkBuffer *decoded, VachunkBuffer *compressed)
{
#if defined(VOIDPLAYER_VACHUNK_ZSTD)
    size_t bound;
    size_t size;

    if (no_compression_requested() || !decoded->size)
        return 0;
    bound = ZSTD_compressBound(decoded->size);
    if (buffer_reserve(compressed, bound) < 0)
        return AVERROR(ENOMEM);
    size = ZSTD_compress(compressed->data, compressed->capacity,
                         decoded->data, decoded->size, 3);
    if (ZSTD_isError(size) || size >= decoded->size) {
        compressed->size = 0;
        return 0;
    }
    compressed->size = size;
    return 1;
#else
    (void)decoded;
    (void)compressed;
    return 0;
#endif
}

static int flush_block(VachunkPendingBlock *block)
{
    VachunkBuffer decoded = { 0 };
    VachunkBuffer compressed = { 0 };
    const VachunkBuffer *payload;
    VachunkArchiveBlockIndexEntry bidx;
    uint32_t block_idx;
    uint32_t i;
    int compressed_used;
    int ret;

    if (!block->frame_count)
        return 0;

    ret = build_decoded_block(block, &decoded);
    if (ret < 0)
        goto done;
    compressed_used = compress_zstd(&decoded, &compressed);
    if (compressed_used < 0) {
        ret = compressed_used;
        goto done;
    }
    payload = compressed_used ? &compressed : &decoded;

    memset(&bidx, 0, sizeof(bidx));
    bidx.first_frame = block->first_frame;
    bidx.frame_count = block->frame_count;
    bidx.first_record = block->first_record;
    bidx.record_count = block->record_count;
    bidx.payload_offset = g_vachunk.cpay_bytes;
    bidx.payload_size = (uint64_t)payload->size;
    bidx.decoded_size = (uint64_t)decoded.size;
    bidx.codec_profile = g_vachunk.profile;
    bidx.compression = compressed_used ? VACHUNK_COMPRESSION_ZSTD : VACHUNK_COMPRESSION_NONE;

    if (write_exact(g_vachunk.file, payload->data, payload->size) < 0) {
        ret = AVERROR(EIO);
        goto done;
    }
    g_vachunk.cpay_bytes += payload->size;
    block_idx = g_vachunk.block_index_count;
    ret = append_block_index(bidx);
    if (ret < 0)
        goto done;

    for (i = 0; i < block->frame_count; ++i) {
        VachunkArchiveFrameIndexEntry fidx;

        memset(&fidx, 0, sizeof(fidx));
        fidx.block_index = block_idx;
        fidx.local_frame = i;
        fidx.first_record = block->frames[i].first_record;
        fidx.record_count = block->frames[i].record_count;
        ret = append_frame_index(fidx);
        if (ret < 0)
            goto done;
    }
    pending_clear(block);

done:
    av_freep(&decoded.data);
    av_freep(&compressed.data);
    return ret;
}

static VachunkArchiveSectionEntry make_section(const char type[4],
                                               uint64_t offset,
                                               uint64_t size,
                                               uint32_t entry_size,
                                               uint32_t entry_count)
{
    VachunkArchiveSectionEntry entry;

    memset(&entry, 0, sizeof(entry));
    set_fourcc(entry.type, type);
    entry.offset = offset;
    entry.size = size;
    entry.entry_size = entry_size;
    entry.entry_count = entry_count;
    return entry;
}

static int append_cu_record(const VoidPlayerVachunkFrameInfo *info,
                            const VachunkCuRecord *record)
{
    VachunkFrameBuffer *frame;
    int ret = 0;

    vachunk_lock();
    if (!g_vachunk.active || g_vachunk.error) {
        ret = g_vachunk.error ? g_vachunk.error : AVERROR(EINVAL);
        goto done;
    }
    if (g_vachunk.has_frame_window && info && info->has_coded_order_key &&
        info->coded_order_key > 0) {
        uint64_t source_frame = info->coded_order_key - 1;
        if (source_frame < g_vachunk.start_frame || source_frame > g_vachunk.end_frame)
            goto done;
    }

    frame = get_or_create_frame(info, record->x, record->y, record->w, record->h, record->depth);
    if (!frame) {
        ret = g_vachunk.error ? g_vachunk.error : AVERROR(EINVAL);
        goto done;
    }

    ret = ensure_record_capacity(frame);
    if (ret < 0)
        goto fail;
    frame->records[frame->record_count++] = *record;
    frame->summary.num_cus = frame->record_count;
    if (!frame->has_first_cu) {
        frame->first_x = record->x;
        frame->first_y = record->y;
        frame->first_w = record->w;
        frame->first_h = record->h;
        frame->first_depth = record->depth;
        frame->has_first_cu = 1;
    }
    frame->qp_sum += record->qp;
    if (frame->summary.num_cus == 1 || record->qp < frame->summary.qp_min)
        frame->summary.qp_min = record->qp;
    if (frame->summary.num_cus == 1 || record->qp > frame->summary.qp_max)
        frame->summary.qp_max = record->qp;
    goto done;

fail:
    g_vachunk.error = ret;

done:
    vachunk_unlock();
    return ret;
}

int ff_voidplayer_vachunk_set_frame_window(uint64_t start_frame, uint64_t end_frame)
{
    int ret = 0;

    if (start_frame > end_frame)
        return AVERROR(EINVAL);

    vachunk_lock();
    if (!g_vachunk.active || g_vachunk.error) {
        ret = g_vachunk.error ? g_vachunk.error : AVERROR(EINVAL);
        goto done;
    }
    g_vachunk.has_frame_window = 1;
    g_vachunk.start_frame = start_frame;
    g_vachunk.end_frame = end_frame;

done:
    vachunk_unlock();
    return ret;
}

static int start_common(uint32_t width, uint32_t height, uint16_t codec)
{
    int ret;

    ff_voidplayer_vachunk_abort();
    if (codec != VOIDPLAYER_VACHUNK_CODEC_H264 &&
        codec != VOIDPLAYER_VACHUNK_CODEC_HEVC &&
        codec != VOIDPLAYER_VACHUNK_CODEC_VVC)
        return AVERROR(ENOSYS);

    ret = ff_mutex_init(&g_vachunk.lock, NULL);
    if (ret)
        return AVERROR(EINVAL);
    g_vachunk.lock_initialized = 1;
    g_vachunk.active = 1;
    g_vachunk.codec = codec;
    g_vachunk.profile = VACHUNK_PROFILE_FIRST;
    g_vachunk.width = width;
    g_vachunk.height = height;
    return 0;
}

int ff_voidplayer_vachunk_start_memory(uint32_t width, uint32_t height, uint16_t codec)
{
    return start_common(width, height, codec);
}

int ff_voidplayer_vachunk_start(const char *path, uint32_t width, uint32_t height, uint16_t codec)
{
    VachunkArchiveHeader header;
    int ret;

    if (!path)
        return AVERROR(EINVAL);

    ret = start_common(width, height, codec);
    if (ret < 0)
        return ret;

    g_vachunk.file = open_utf8_file(path, "w+b");
    if (!g_vachunk.file) {
        ret = AVERROR(errno ? errno : EIO);
        reset_state();
        return ret;
    }

    g_vachunk.cpay_payload_offset = sizeof(VachunkArchiveHeader);
    memset(&header, 0, sizeof(header));
    if (write_exact(g_vachunk.file, &header, sizeof(header)) < 0) {
        reset_state();
        return AVERROR(EIO);
    }
    return 0;
}

static void finalize_frame_summaries(void)
{
    uint32_t i;

    if (all_frames_have_coded_order_keys())
        qsort(g_vachunk.frames, g_vachunk.frame_count, sizeof(*g_vachunk.frames),
              compare_frame_coded_order_key);

    for (i = 0; i < g_vachunk.frame_count; ++i) {
        VachunkFrameBuffer *frame = &g_vachunk.frames[i];

        if (frame->summary.num_cus) {
            frame->summary.avg_qp =
                (uint8_t)((frame->qp_sum + frame->summary.num_cus / 2) / frame->summary.num_cus);
        }
        frame->summary.coded_order = i;
        frame->summary.cu_index_entry = i;
        if (g_vachunk.codec == VOIDPLAYER_VACHUNK_CODEC_H264 && frame->record_count > 1)
            qsort(frame->records, frame->record_count, sizeof(*frame->records), compare_cu_raster);
    }
}

static VachunkSectionEntry make_vachunk_section(const char type[4],
                                                uint64_t offset,
                                                uint64_t size,
                                                uint32_t entry_size,
                                                uint32_t entry_count)
{
    VachunkSectionEntry entry;

    memset(&entry, 0, sizeof(entry));
    set_fourcc(entry.type, type);
    entry.offset = offset;
    entry.size = size;
    entry.entry_size = entry_size;
    entry.entry_count = entry_count;
    entry.decoded_size = size;
    return entry;
}

static uint64_t total_frame_records(void)
{
    uint64_t total = 0;
    uint32_t i;

    for (i = 0; i < g_vachunk.frame_count; ++i)
        total += g_vachunk.frames[i].record_count;
    return total;
}

static VachunkPackedCuRecord make_vachunk_cu_record(const VachunkCuRecord *source)
{
    VachunkPackedCuRecord out;

    memset(&out, 0, sizeof(out));
    out.x = source->x;
    out.y = source->y;
    out.w = source->w;
    out.h = source->h;
    out.depth = source->depth;
    out.qp = source->qp;
    out.pred_mode = source->pred_mode;
    out.bit_count = source->bit_count;
    if (source->pred_mode == 1) {
        out.data.intra.intra_mode = source->intra_mode;
        out.data.intra.mip_flag = source->mip_flag;
        out.data.intra.isp_mode = source->isp_mode;
    } else {
        out.data.inter.skip = source->skip;
        out.data.inter.merge_flag = source->merge_flag;
        out.data.inter.inter_dir = source->inter_dir;
        out.data.inter.mv_l0_x = source->mv_l0_x;
        out.data.inter.mv_l0_y = source->mv_l0_y;
        out.data.inter.mv_l1_x = source->mv_l1_x;
        out.data.inter.mv_l1_y = source->mv_l1_y;
        out.data.inter.ref_l0 = source->ref_l0;
        out.data.inter.ref_l1 = source->ref_l1;
    }
    return out;
}

int ff_voidplayer_vachunk_finish_vachunk(const char *path,
                                      uint32_t source_start_frame,
                                      uint32_t source_end_frame,
                                      uint64_t base_content_revision,
                                      uint64_t generator_revision)
{
    const uint32_t section_count = 3;
    VachunkSectionEntry sections[3];
    VachunkHeader header;
    FILE *out = NULL;
    uint64_t table_offset = sizeof(VachunkHeader);
    uint64_t table_size = section_count * sizeof(VachunkSectionEntry);
    uint64_t payload_offset = table_offset + table_size;
    uint64_t fsum_size;
    uint64_t fidx_size;
    uint64_t cu4r_size;
    uint64_t cursor;
    uint64_t first_unit = 0;
    uint64_t record_count;
    uint32_t i;
    int ret = 0;

    if (!path || !g_vachunk.active || g_vachunk.file || source_start_frame > source_end_frame)
        return AVERROR(EINVAL);
    if (g_vachunk.error) {
        ret = g_vachunk.error;
        goto done;
    }
    if (g_vachunk.frame_count == 0 ||
        (uint64_t)source_end_frame - source_start_frame + 1 != g_vachunk.frame_count) {
        ret = AVERROR(EINVAL);
        goto done;
    }

    finalize_frame_summaries();
    record_count = total_frame_records();
    if (record_count > UINT32_MAX) {
        ret = AVERROR(EOVERFLOW);
        goto done;
    }

    fsum_size = (uint64_t)g_vachunk.frame_count * sizeof(VachunkFrameSummary);
    fidx_size = (uint64_t)g_vachunk.frame_count * sizeof(VachunkOverlayFrameIndexEntry);
    cu4r_size = record_count * sizeof(VachunkPackedCuRecord);
    cursor = payload_offset;
    sections[0] = make_vachunk_section("FSUM", cursor, fsum_size,
                                       sizeof(VachunkFrameSummary), g_vachunk.frame_count);
    cursor += fsum_size;
    sections[1] = make_vachunk_section("FIDX", cursor, fidx_size,
                                       sizeof(VachunkOverlayFrameIndexEntry), g_vachunk.frame_count);
    cursor += fidx_size;
    sections[2] = make_vachunk_section("CU4R", cursor, cu4r_size,
                                       sizeof(VachunkPackedCuRecord), (uint32_t)record_count);
    cursor += cu4r_size;

    memset(&header, 0, sizeof(header));
    set_fourcc(header.magic, "VCK1");
    header.version_major = VACHUNK_VERSION_MAJOR;
    header.version_minor = VACHUNK_VERSION_MINOR;
    header.header_size = sizeof(VachunkHeader);
    header.section_entry_size = sizeof(VachunkSectionEntry);
    header.section_count = section_count;
    header.kind = VACHUNK_KIND_OVERLAY;
    header.codec = g_vachunk.codec;
    header.feature_flags = VACHUNK_OVERLAY_FEATURE_FLAGS;
    header.base_content_revision = base_content_revision;
    header.generator_revision = generator_revision;
    header.compression = VACHUNK_COMPRESSION_NONE;
    header.start_frame = source_start_frame;
    header.end_frame = source_end_frame;
    header.start_packet = UINT32_MAX;
    header.end_packet = UINT32_MAX;
    header.start_unit = UINT32_MAX;
    header.end_unit = UINT32_MAX;
    header.section_table_offset = table_offset;
    header.file_size = cursor;

    out = open_utf8_file(path, "w+b");
    if (!out) {
        ret = AVERROR(errno ? errno : EIO);
        goto done;
    }
    if (write_exact(out, &header, sizeof(header)) < 0 ||
        write_exact(out, sections, sizeof(sections)) < 0) {
        ret = AVERROR(EIO);
        goto done;
    }
    for (i = 0; i < g_vachunk.frame_count; ++i) {
        if (write_exact(out, &g_vachunk.frames[i].summary,
                        sizeof(g_vachunk.frames[i].summary)) < 0) {
            ret = AVERROR(EIO);
            goto done;
        }
    }
    for (i = 0; i < g_vachunk.frame_count; ++i) {
        VachunkOverlayFrameIndexEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.frame_index = source_start_frame + i;
        entry.first_unit = (uint32_t)first_unit;
        entry.unit_count = g_vachunk.frames[i].record_count;
        entry.flags =
            VACHUNK_OVERLAY_FRAME_FLAG_COMPLETE |
            VACHUNK_OVERLAY_FRAME_FLAG_EXACT;
        first_unit += g_vachunk.frames[i].record_count;
        if (write_exact(out, &entry, sizeof(entry)) < 0) {
            ret = AVERROR(EIO);
            goto done;
        }
    }
    for (i = 0; i < g_vachunk.frame_count; ++i) {
        VachunkFrameBuffer *frame = &g_vachunk.frames[i];
        uint32_t j;
        for (j = 0; j < frame->record_count; ++j) {
            VachunkPackedCuRecord record = make_vachunk_cu_record(&frame->records[j]);
            if (write_exact(out, &record, sizeof(record)) < 0) {
                ret = AVERROR(EIO);
                goto done;
            }
        }
    }

done:
    if (out && fclose(out) != 0 && ret == 0)
        ret = AVERROR(EIO);
    reset_state();
    return ret;
}

int ff_voidplayer_vachunk_finish(void)
{
    const uint32_t section_count = 4;
    VachunkPendingBlock block = { 0 };
    VachunkArchiveSectionEntry sections[4];
    VachunkArchiveHeader header;
    uint64_t fsum_offset;
    uint64_t fsum_size;
    uint64_t fidx_offset;
    uint64_t fidx_size;
    uint64_t bidx_offset;
    uint64_t bidx_size;
    uint64_t section_table_offset;
    uint64_t file_size;
    uint32_t i;
    int64_t pos;
    int ret = 0;

    if (!g_vachunk.file)
        return AVERROR(EINVAL);
    if (g_vachunk.error) {
        ret = g_vachunk.error;
        goto done;
    }
    if (seek_file(g_vachunk.file, sizeof(VachunkArchiveHeader)) != 0) {
        ret = AVERROR(EIO);
        goto done;
    }

    finalize_frame_summaries();

    for (i = 0; i < g_vachunk.frame_count; ++i) {
        VachunkFrameBuffer *frame = &g_vachunk.frames[i];

        if (block.frame_count &&
            (block.frame_count >= 4096 || estimate_block_size(&block, frame) > 8ull * 1024ull * 1024ull)) {
            ret = flush_block(&block);
            if (ret < 0)
                goto done;
        }
        ret = append_frame_to_pending(&block, i);
        if (ret < 0)
            goto done;
    }
    ret = flush_block(&block);
    if (ret < 0)
        goto done;

    pos = tell_file(g_vachunk.file);
    if (pos < 0) {
        ret = AVERROR(EIO);
        goto done;
    }
    fsum_offset = (uint64_t)pos;
    for (i = 0; i < g_vachunk.frame_count; ++i) {
        if (write_exact(g_vachunk.file, &g_vachunk.frames[i].summary,
                        sizeof(g_vachunk.frames[i].summary)) < 0) {
            ret = AVERROR(EIO);
            goto done;
        }
    }

    pos = tell_file(g_vachunk.file);
    if (pos < 0) {
        ret = AVERROR(EIO);
        goto done;
    }
    fidx_offset = (uint64_t)pos;
    fidx_size = (uint64_t)g_vachunk.frame_index_count * sizeof(VachunkArchiveFrameIndexEntry);
    if (write_exact(g_vachunk.file, g_vachunk.frame_index, (size_t)fidx_size) < 0) {
        ret = AVERROR(EIO);
        goto done;
    }

    pos = tell_file(g_vachunk.file);
    if (pos < 0) {
        ret = AVERROR(EIO);
        goto done;
    }
    bidx_offset = (uint64_t)pos;
    bidx_size = (uint64_t)g_vachunk.block_index_count * sizeof(VachunkArchiveBlockIndexEntry);
    if (write_exact(g_vachunk.file, g_vachunk.block_index, (size_t)bidx_size) < 0) {
        ret = AVERROR(EIO);
        goto done;
    }

    pos = tell_file(g_vachunk.file);
    if (pos < 0) {
        ret = AVERROR(EIO);
        goto done;
    }
    section_table_offset = (uint64_t)pos;
    fsum_size = (uint64_t)g_vachunk.frame_count * sizeof(VachunkFrameSummary);
    sections[0] = make_section("FSUM", fsum_offset, fsum_size,
                               sizeof(VachunkFrameSummary), g_vachunk.frame_count);
    sections[1] = make_section("FIDX", fidx_offset, fidx_size,
                               sizeof(VachunkArchiveFrameIndexEntry), g_vachunk.frame_index_count);
    sections[2] = make_section("BIDX", bidx_offset, bidx_size,
                               sizeof(VachunkArchiveBlockIndexEntry), g_vachunk.block_index_count);
    sections[3] = make_section("CPAY", g_vachunk.cpay_payload_offset, g_vachunk.cpay_bytes,
                               0, g_vachunk.block_index_count);

    if (write_exact(g_vachunk.file, sections, sizeof(sections)) < 0) {
        ret = AVERROR(EIO);
        goto done;
    }

    pos = tell_file(g_vachunk.file);
    if (pos < 0) {
        ret = AVERROR(EIO);
        goto done;
    }
    file_size = (uint64_t)pos;

    memset(&header, 0, sizeof(header));
    set_fourcc(header.magic, "VACHUNK");
    header.version_major = 4;
    header.header_size = sizeof(VachunkArchiveHeader);
    header.section_entry_size = sizeof(VachunkArchiveSectionEntry);
    header.codec = g_vachunk.codec;
    header.profile = g_vachunk.profile;
    header.width = g_vachunk.width;
    header.height = g_vachunk.height;
    header.frame_count = g_vachunk.frame_count;
    header.block_count = g_vachunk.block_index_count;
    header.section_count = section_count;
    header.section_table_offset = section_table_offset;
    header.file_size = file_size;

    if (seek_file(g_vachunk.file, 0) != 0 ||
        write_exact(g_vachunk.file, &header, sizeof(header)) < 0) {
        ret = AVERROR(EIO);
        goto done;
    }

done:
    pending_free(&block);
    if (g_vachunk.file && fclose(g_vachunk.file) != 0 && ret == 0)
        ret = AVERROR(EIO);
    g_vachunk.file = NULL;
    free_frames();
    av_freep(&g_vachunk.frame_index);
    av_freep(&g_vachunk.block_index);
    if (g_vachunk.lock_initialized)
        ff_mutex_destroy(&g_vachunk.lock);
    memset(&g_vachunk, 0, sizeof(g_vachunk));
    return ret;
}

void ff_voidplayer_vachunk_abort(void)
{
    reset_state();
}

int ff_voidplayer_vachunk_is_active(void)
{
    return g_vachunk.active && !g_vachunk.error;
}

uint32_t ff_voidplayer_vachunk_frame_count(void)
{
    uint32_t frame_count;

    vachunk_lock();
    frame_count = g_vachunk.frame_count;
    vachunk_unlock();
    return frame_count;
}

uint32_t ff_voidplayer_vachunk_last_frame_cu_count(void)
{
    uint32_t cu_count = 0;

    vachunk_lock();
    if (g_vachunk.frame_count)
        cu_count = g_vachunk.frames[g_vachunk.frame_count - 1].summary.num_cus;
    vachunk_unlock();
    return cu_count;
}

void ff_voidplayer_vachunk_write_intra_cu(const VoidPlayerVachunkFrameInfo *info,
                                       uint16_t x,
                                       uint16_t y,
                                       uint8_t w,
                                       uint8_t h,
                                       uint8_t depth,
                                       uint8_t qp,
                                       uint8_t intra_mode,
                                       uint8_t mip_flag,
                                       uint8_t isp_mode,
                                       uint32_t bit_count)
{
    VachunkCuRecord record;

    memset(&record, 0, sizeof(record));
    record.x = x;
    record.y = y;
    record.w = w;
    record.h = h;
    record.depth = depth;
    record.qp = qp;
    record.pred_mode = 1;
    record.bit_count = bit_count;
    record.intra_mode = intra_mode;
    record.mip_flag = mip_flag;
    record.isp_mode = isp_mode;
    record.ref_l0 = -1;
    record.ref_l1 = -1;
    append_cu_record(info, &record);
}

void ff_voidplayer_vachunk_write_inter_cu(const VoidPlayerVachunkFrameInfo *info,
                                       uint16_t x,
                                       uint16_t y,
                                       uint8_t w,
                                       uint8_t h,
                                       uint8_t depth,
                                       uint8_t qp,
                                       uint8_t skip,
                                       uint8_t merge_flag,
                                       uint8_t inter_dir,
                                       int16_t mv_l0_x,
                                       int16_t mv_l0_y,
                                       int16_t mv_l1_x,
                                       int16_t mv_l1_y,
                                       int8_t ref_l0,
                                       int8_t ref_l1,
                                       uint32_t bit_count)
{
    VachunkCuRecord record;

    memset(&record, 0, sizeof(record));
    record.x = x;
    record.y = y;
    record.w = w;
    record.h = h;
    record.depth = depth;
    record.qp = qp;
    record.pred_mode = 0;
    record.bit_count = bit_count;
    record.intra_mode = 255;
    record.skip = skip;
    record.merge_flag = merge_flag;
    record.inter_dir = inter_dir;
    record.mv_l0_x = mv_l0_x;
    record.mv_l0_y = mv_l0_y;
    record.mv_l1_x = mv_l1_x;
    record.mv_l1_y = mv_l1_y;
    record.ref_l0 = ref_l0;
    record.ref_l1 = ref_l1;
    append_cu_record(info, &record);
}

void ff_voidplayer_vachunk_write_h264_mb(const VoidPlayerVachunkFrameInfo *info,
                                      uint16_t x,
                                      uint16_t y,
                                      uint8_t qp,
                                      uint8_t is_intra,
                                      uint8_t intra_mode,
                                      uint8_t skip,
                                      uint8_t merge_flag,
                                      uint8_t inter_dir,
                                      int16_t mv_l0_x,
                                      int16_t mv_l0_y,
                                      int16_t mv_l1_x,
                                      int16_t mv_l1_y,
                                      int8_t ref_l0,
                                      int8_t ref_l1,
                                      uint32_t bit_count)
{
    VachunkCuRecord record;

    memset(&record, 0, sizeof(record));
    record.x = x;
    record.y = y;
    record.w = 16;
    record.h = 16;
    record.qp = qp;
    record.pred_mode = is_intra ? 1 : 0;
    record.bit_count = bit_count;
    record.intra_mode = is_intra ? intra_mode : 255;
    record.skip = skip;
    record.merge_flag = merge_flag;
    record.inter_dir = inter_dir;
    record.mv_l0_x = mv_l0_x;
    record.mv_l0_y = mv_l0_y;
    record.mv_l1_x = mv_l1_x;
    record.mv_l1_y = mv_l1_y;
    record.ref_l0 = ref_l0;
    record.ref_l1 = ref_l1;
    append_cu_record(info, &record);
}

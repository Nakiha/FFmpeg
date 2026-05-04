#include "voidplayer_vbs3.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"

#define VBS3_CUID_FLAG_COMPRESSED_XPRESS_HUFF 0x00000001u
#define VBS3_CUBL_SECTION_FLAG_PER_FRAME_COMPRESSION 0x00000001u

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

typedef struct VbsCuCommon {
    uint16_t x;
    uint16_t y;
    uint8_t  w;
    uint8_t  h;
    uint8_t  depth;
    uint8_t  qp;
    uint8_t  pred_mode;
} VbsCuCommon;

typedef struct VbsCuIntra {
    uint8_t intra_mode;
    uint8_t mip_flag;
    uint8_t isp_mode;
} VbsCuIntra;

typedef struct VbsCuInter {
    uint8_t  skip;
    uint8_t  merge_flag;
    uint8_t  inter_dir;
    int16_t  mv_l0_x;
    int16_t  mv_l0_y;
    int16_t  mv_l1_x;
    int16_t  mv_l1_y;
    int8_t   ref_l0;
    int8_t   ref_l1;
} VbsCuInter;
#pragma pack(pop)

typedef struct Vbs3FrameBuffer {
    Vbs3FrameSummary summary;
    uint8_t *data;
    uint64_t data_size;
    uint64_t data_capacity;
    uint64_t qp_sum;
    uint16_t first_x;
    uint16_t first_y;
    uint8_t first_w;
    uint8_t first_h;
    uint8_t first_depth;
    int has_first_cu;
    uintptr_t frame_identity;
} Vbs3FrameBuffer;

typedef struct VoidVbs3State {
    FILE *file;
    uint32_t width;
    uint32_t height;
    Vbs3FrameBuffer *frames;
    uint32_t frame_count;
    uint32_t frame_capacity;
    int error;
    AVMutex lock;
    int lock_initialized;
} VoidVbs3State;

static VoidVbs3State g_vbs3;

typedef struct Vbs3CompressedFrame {
    uint8_t *data;
    size_t size;
    uint32_t flags;
} Vbs3CompressedFrame;

static void vbs3_lock(void)
{
    if (g_vbs3.lock_initialized)
        ff_mutex_lock(&g_vbs3.lock);
}

static void vbs3_unlock(void)
{
    if (g_vbs3.lock_initialized)
        ff_mutex_unlock(&g_vbs3.lock);
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
    if (size == 0)
        return 0;
    return fwrite(data, 1, size, file) == size ? 0 : AVERROR(EIO);
}

#ifdef _WIN32
#define VOID_COMPRESS_ALGORITHM_XPRESS_HUFF 4u

typedef void *VoidCompressorHandle;
typedef BOOL (WINAPI *VoidCreateCompressorProc)(DWORD, void *, VoidCompressorHandle *);
typedef BOOL (WINAPI *VoidCompressProc)(VoidCompressorHandle, void *, size_t, void *, size_t, size_t *);
typedef BOOL (WINAPI *VoidCloseCompressorProc)(VoidCompressorHandle);

typedef struct VoidCompressionApi {
    int initialized;
    int available;
    HMODULE module;
    VoidCreateCompressorProc create_compressor;
    VoidCompressProc compress;
    VoidCloseCompressorProc close_compressor;
} VoidCompressionApi;

static VoidCompressionApi g_compression_api;

static int load_compression_api(void)
{
    if (!g_compression_api.initialized) {
        g_compression_api.initialized = 1;
        g_compression_api.module = LoadLibraryA("Cabinet.dll");
        if (g_compression_api.module) {
            g_compression_api.create_compressor =
                (VoidCreateCompressorProc)GetProcAddress(g_compression_api.module, "CreateCompressor");
            g_compression_api.compress =
                (VoidCompressProc)GetProcAddress(g_compression_api.module, "Compress");
            g_compression_api.close_compressor =
                (VoidCloseCompressorProc)GetProcAddress(g_compression_api.module, "CloseCompressor");
            g_compression_api.available = g_compression_api.create_compressor &&
                                          g_compression_api.compress &&
                                          g_compression_api.close_compressor;
        }
    }
    return g_compression_api.available;
}
#endif

static int try_compress_frame(const uint8_t *src, uint64_t src_size, Vbs3CompressedFrame *dst)
{
#ifdef _WIN32
    VoidCompressorHandle compressor = NULL;
    size_t compressed_size = 0;
    size_t capacity;
    uint8_t *buffer;

    memset(dst, 0, sizeof(*dst));
    if (!src || src_size < 256 || (uint64_t)(size_t)src_size != src_size)
        return 0;
    if (!load_compression_api())
        return 0;
    if (src_size > (uint64_t)(SIZE_MAX - 4096) / 17 * 16)
        return 0;

    capacity = (size_t)src_size + (size_t)(src_size / 16) + 4096;
    if (capacity >= (size_t)src_size)
        capacity = (size_t)src_size - 1;
    if (!capacity)
        return 0;

    buffer = av_malloc(capacity);
    if (!buffer)
        return AVERROR(ENOMEM);

    if (!g_compression_api.create_compressor(VOID_COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &compressor)) {
        av_free(buffer);
        return 0;
    }
    if (g_compression_api.compress(compressor, (void *)src, (size_t)src_size,
                                   buffer, capacity, &compressed_size) &&
        compressed_size > 0 && compressed_size < src_size) {
        dst->data = buffer;
        dst->size = compressed_size;
        dst->flags = VBS3_CUID_FLAG_COMPRESSED_XPRESS_HUFF;
    } else {
        av_free(buffer);
    }
    g_compression_api.close_compressor(compressor);
    return 0;
#else
    (void)src;
    (void)src_size;
    memset(dst, 0, sizeof(*dst));
    return 0;
#endif
}

static void free_frames(void)
{
    uint32_t i;

    for (i = 0; i < g_vbs3.frame_count; ++i)
        av_freep(&g_vbs3.frames[i].data);
    av_freep(&g_vbs3.frames);
    g_vbs3.frame_count = 0;
    g_vbs3.frame_capacity = 0;
}

static void reset_state(void)
{
    int had_lock = g_vbs3.lock_initialized;

    if (g_vbs3.file)
        fclose(g_vbs3.file);
    free_frames();
    if (had_lock)
        ff_mutex_destroy(&g_vbs3.lock);
    memset(&g_vbs3, 0, sizeof(g_vbs3));
}

static int ensure_frame_capacity(void)
{
    Vbs3FrameBuffer *new_frames;
    uint32_t new_capacity;

    if (g_vbs3.frame_count < g_vbs3.frame_capacity)
        return 0;

    new_capacity = g_vbs3.frame_capacity ? g_vbs3.frame_capacity * 2 : 256;
    if (new_capacity < g_vbs3.frame_capacity)
        return AVERROR(ENOMEM);
    new_frames = av_realloc_array(g_vbs3.frames, new_capacity, sizeof(*g_vbs3.frames));
    if (!new_frames)
        return AVERROR(ENOMEM);
    memset(new_frames + g_vbs3.frame_capacity, 0,
           (new_capacity - g_vbs3.frame_capacity) * sizeof(*new_frames));
    g_vbs3.frames = new_frames;
    g_vbs3.frame_capacity = new_capacity;
    return 0;
}

static void fill_summary(Vbs3FrameSummary *summary, const VoidPlayerVbs3FrameInfo *info)
{
    int i;

    memset(summary, 0, sizeof(*summary));
    summary->poc = info->poc;
    summary->coded_order = g_vbs3.frame_count;
    summary->vcl_nalu_index = 0xFFFFFFFFu;
    summary->temporal_id = info->temporal_id;
    summary->slice_type = info->slice_type;
    summary->nal_unit_type = info->nal_unit_type;
    summary->num_ref_l0 = info->num_ref_l0 > 15 ? 15 : info->num_ref_l0;
    summary->num_ref_l1 = info->num_ref_l1 > 15 ? 15 : info->num_ref_l1;
    summary->cu_index_entry = g_vbs3.frame_count;
    for (i = 0; i < 15; ++i) {
        summary->ref_pocs_l0[i] = i < summary->num_ref_l0 ? info->ref_pocs_l0[i] : -1;
        summary->ref_pocs_l1[i] = i < summary->num_ref_l1 ? info->ref_pocs_l1[i] : -1;
    }
}

static int frame_can_accept_cu(const Vbs3FrameBuffer *frame,
                               const VoidPlayerVbs3FrameInfo *info,
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

static Vbs3FrameBuffer *find_frame(const VoidPlayerVbs3FrameInfo *info,
                                   uint16_t x,
                                   uint16_t y,
                                   uint8_t w,
                                   uint8_t h,
                                   uint8_t depth)
{
    uint32_t i;

    for (i = g_vbs3.frame_count; i > 0; --i) {
        Vbs3FrameBuffer *frame = &g_vbs3.frames[i - 1];
        if (frame->summary.poc == info->poc &&
            frame_can_accept_cu(frame, info, x, y, w, h, depth))
            return frame;
    }
    return NULL;
}

static Vbs3FrameBuffer *get_or_create_frame(const VoidPlayerVbs3FrameInfo *info,
                                            uint16_t x,
                                            uint16_t y,
                                            uint8_t w,
                                            uint8_t h,
                                            uint8_t depth)
{
    Vbs3FrameBuffer *frame;

    if (!info) {
        g_vbs3.error = AVERROR(EINVAL);
        return NULL;
    }

    frame = find_frame(info, x, y, w, h, depth);
    if (frame)
        return frame;

    g_vbs3.error = ensure_frame_capacity();
    if (g_vbs3.error)
        return NULL;

    frame = &g_vbs3.frames[g_vbs3.frame_count];
    memset(frame, 0, sizeof(*frame));
    fill_summary(&frame->summary, info);
    frame->frame_identity = info->frame_identity;
    g_vbs3.frame_count++;
    if (info->width)
        g_vbs3.width = info->width;
    if (info->height)
        g_vbs3.height = info->height;
    return frame;
}

static int append_bytes(Vbs3FrameBuffer *frame, const void *data, size_t size)
{
    uint8_t *new_data;
    uint64_t required;
    uint64_t new_capacity;

    if (!size)
        return 0;
    if (frame->data_size > UINT64_MAX - size)
        return AVERROR(ENOMEM);
    required = frame->data_size + size;
    if (required > frame->data_capacity) {
        new_capacity = frame->data_capacity ? frame->data_capacity * 2 : 4096;
        while (new_capacity < required) {
            if (new_capacity > UINT64_MAX / 2) {
                new_capacity = required;
                break;
            }
            new_capacity *= 2;
        }
        if ((uint64_t)(size_t)new_capacity != new_capacity)
            return AVERROR(ENOMEM);
        new_data = av_realloc(frame->data, (size_t)new_capacity);
        if (!new_data)
            return AVERROR(ENOMEM);
        frame->data = new_data;
        frame->data_capacity = new_capacity;
    }
    memcpy(frame->data + frame->data_size, data, size);
    frame->data_size = required;
    return 0;
}

static int append_cu_record(const VoidPlayerVbs3FrameInfo *info,
                            uint16_t x,
                            uint16_t y,
                            uint8_t w,
                            uint8_t h,
                            uint8_t depth,
                            const void *common, size_t common_size,
                            const void *extra, size_t extra_size,
                            uint8_t qp)
{
    Vbs3FrameBuffer *frame;
    int ret = 0;

    vbs3_lock();
    if (!g_vbs3.file || g_vbs3.error) {
        ret = g_vbs3.error ? g_vbs3.error : AVERROR(EINVAL);
        goto done;
    }

    frame = get_or_create_frame(info, x, y, w, h, depth);
    if (!frame) {
        ret = g_vbs3.error ? g_vbs3.error : AVERROR(EINVAL);
        goto done;
    }

    ret = append_bytes(frame, common, common_size);
    if (ret < 0)
        goto fail;
    ret = append_bytes(frame, extra, extra_size);
    if (ret < 0)
        goto fail;

    frame->summary.num_cus++;
    if (!frame->has_first_cu) {
        frame->first_x = x;
        frame->first_y = y;
        frame->first_w = w;
        frame->first_h = h;
        frame->first_depth = depth;
        frame->has_first_cu = 1;
    }
    frame->qp_sum += qp;
    if (frame->summary.num_cus == 1 || qp < frame->summary.qp_min)
        frame->summary.qp_min = qp;
    if (frame->summary.num_cus == 1 || qp > frame->summary.qp_max)
        frame->summary.qp_max = qp;
    goto done;

fail:
    g_vbs3.error = ret;

done:
    vbs3_unlock();
    return ret;
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

int ff_voidplayer_vbs3_start(const char *path, uint32_t width, uint32_t height)
{
    Vbs3Header header;
    int ret;

    ff_voidplayer_vbs3_abort();
    if (!path)
        return AVERROR(EINVAL);

    ret = ff_mutex_init(&g_vbs3.lock, NULL);
    if (ret)
        return AVERROR(EINVAL);
    g_vbs3.lock_initialized = 1;

    g_vbs3.file = fopen(path, "w+b");
    if (!g_vbs3.file) {
        ret = AVERROR(errno ? errno : EIO);
        reset_state();
        return ret;
    }

    g_vbs3.width = width;
    g_vbs3.height = height;
    memset(&header, 0, sizeof(header));
    if (write_exact(g_vbs3.file, &header, sizeof(header)) < 0) {
        reset_state();
        return AVERROR(EIO);
    }
    return 0;
}

int ff_voidplayer_vbs3_finish(void)
{
    const uint32_t section_count = 3;
    Vbs3CuIndexEntry *cu_index = NULL;
    uint64_t cubl_offset;
    uint64_t cubl_bytes = 0;
    uint64_t fsum_offset;
    uint64_t fsum_size;
    uint64_t cuid_offset;
    uint64_t cuid_size;
    uint64_t section_table_offset;
    uint64_t file_size;
    Vbs3SectionEntry sections[3];
    Vbs3Header header;
    uint32_t i;
    uint32_t cubl_flags = 0;
    int ret = 0;

    if (!g_vbs3.file)
        return AVERROR(EINVAL);
    if (g_vbs3.error) {
        ret = g_vbs3.error;
        goto done;
    }

    cu_index = av_calloc(g_vbs3.frame_count ? g_vbs3.frame_count : 1, sizeof(*cu_index));
    if (!cu_index) {
        ret = AVERROR(ENOMEM);
        goto done;
    }

    cubl_offset = sizeof(Vbs3Header);
    if (fseek(g_vbs3.file, (long)cubl_offset, SEEK_SET) != 0) {
        ret = AVERROR(EIO);
        goto done;
    }

    for (i = 0; i < g_vbs3.frame_count; ++i) {
        Vbs3FrameBuffer *frame = &g_vbs3.frames[i];
        Vbs3CompressedFrame compressed;
        const uint8_t *payload;
        uint64_t payload_size;

        if (frame->summary.num_cus) {
            frame->summary.avg_qp =
                (uint8_t)((frame->qp_sum + frame->summary.num_cus / 2) / frame->summary.num_cus);
        }
        frame->summary.coded_order = i;
        frame->summary.cu_index_entry = i;

        ret = try_compress_frame(frame->data, frame->data_size, &compressed);
        if (ret < 0)
            goto done;
        payload = compressed.data ? compressed.data : frame->data;
        payload_size = compressed.data ? (uint64_t)compressed.size : frame->data_size;

        cu_index[i].offset = cubl_bytes;
        cu_index[i].byte_size = payload_size;
        cu_index[i].cu_count = frame->summary.num_cus;
        cu_index[i].flags = compressed.flags;
        if (compressed.flags)
            cubl_flags |= VBS3_CUBL_SECTION_FLAG_PER_FRAME_COMPRESSION;
        if (write_exact(g_vbs3.file, payload, (size_t)payload_size) < 0) {
            av_freep(&compressed.data);
            ret = AVERROR(EIO);
            goto done;
        }
        av_freep(&compressed.data);
        cubl_bytes += payload_size;
    }

    fsum_offset = cubl_offset + cubl_bytes;
    fsum_size = (uint64_t)g_vbs3.frame_count * sizeof(Vbs3FrameSummary);
    cuid_offset = fsum_offset + fsum_size;
    cuid_size = (uint64_t)g_vbs3.frame_count * sizeof(Vbs3CuIndexEntry);
    section_table_offset = cuid_offset + cuid_size;
    file_size = section_table_offset + section_count * sizeof(Vbs3SectionEntry);

    sections[0] = make_section("FSUM", fsum_offset, fsum_size,
                               sizeof(Vbs3FrameSummary), g_vbs3.frame_count);
    sections[1] = make_section("CUID", cuid_offset, cuid_size,
                               sizeof(Vbs3CuIndexEntry), g_vbs3.frame_count);
    sections[2] = make_section("CUBL", cubl_offset, cubl_bytes, 0,
                               g_vbs3.frame_count);
    sections[2].flags = cubl_flags;

    memset(&header, 0, sizeof(header));
    set_fourcc(header.magic, "VBS3");
    header.version_major = 3;
    header.version_minor = cubl_flags ? 1 : 0;
    header.header_size = sizeof(Vbs3Header);
    header.section_entry_size = sizeof(Vbs3SectionEntry);
    header.width = g_vbs3.width;
    header.height = g_vbs3.height;
    header.frame_count = g_vbs3.frame_count;
    header.section_count = section_count;
    header.section_table_offset = section_table_offset;
    header.file_size = file_size;

    for (i = 0; i < g_vbs3.frame_count; ++i) {
        if (write_exact(g_vbs3.file, &g_vbs3.frames[i].summary,
                        sizeof(g_vbs3.frames[i].summary)) < 0) {
            ret = AVERROR(EIO);
            goto done;
        }
    }
    if (write_exact(g_vbs3.file, cu_index, (size_t)cuid_size) < 0 ||
        write_exact(g_vbs3.file, sections, sizeof(sections)) < 0 ||
        fseek(g_vbs3.file, 0, SEEK_SET) != 0 ||
        write_exact(g_vbs3.file, &header, sizeof(header)) < 0) {
        ret = AVERROR(EIO);
    }

done:
    av_freep(&cu_index);
    if (g_vbs3.file && fclose(g_vbs3.file) != 0 && ret == 0)
        ret = AVERROR(EIO);
    g_vbs3.file = NULL;
    free_frames();
    if (g_vbs3.lock_initialized)
        ff_mutex_destroy(&g_vbs3.lock);
    memset(&g_vbs3, 0, sizeof(g_vbs3));
    return ret;
}

void ff_voidplayer_vbs3_abort(void)
{
    reset_state();
}

int ff_voidplayer_vbs3_is_active(void)
{
    return g_vbs3.file && !g_vbs3.error;
}

uint32_t ff_voidplayer_vbs3_frame_count(void)
{
    uint32_t frame_count;

    vbs3_lock();
    frame_count = g_vbs3.frame_count;
    vbs3_unlock();
    return frame_count;
}

uint32_t ff_voidplayer_vbs3_last_frame_cu_count(void)
{
    uint32_t cu_count = 0;

    vbs3_lock();
    if (g_vbs3.frame_count)
        cu_count = g_vbs3.frames[g_vbs3.frame_count - 1].summary.num_cus;
    vbs3_unlock();
    return cu_count;
}

void ff_voidplayer_vbs3_write_intra_cu(const VoidPlayerVbs3FrameInfo *info,
                                       uint16_t x,
                                       uint16_t y,
                                       uint8_t w,
                                       uint8_t h,
                                       uint8_t depth,
                                       uint8_t qp,
                                       uint8_t intra_mode,
                                       uint8_t mip_flag,
                                       uint8_t isp_mode)
{
    VbsCuCommon common = { x, y, w, h, depth, qp, 1 };
    VbsCuIntra intra = { intra_mode, mip_flag, isp_mode };

    append_cu_record(info, x, y, w, h, depth,
                     &common, sizeof(common), &intra, sizeof(intra), qp);
}

void ff_voidplayer_vbs3_write_inter_cu(const VoidPlayerVbs3FrameInfo *info,
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
                                       int8_t ref_l1)
{
    VbsCuCommon common = { x, y, w, h, depth, qp, 0 };
    VbsCuInter inter = {
        skip, merge_flag, inter_dir,
        mv_l0_x, mv_l0_y, mv_l1_x, mv_l1_y,
        ref_l0, ref_l1
    };

    append_cu_record(info, x, y, w, h, depth,
                     &common, sizeof(common), &inter, sizeof(inter), qp);
}

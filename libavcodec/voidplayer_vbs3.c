#include "voidplayer_vbs3.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"

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

typedef struct VoidVbs3State {
    FILE *file;
    uint32_t width;
    uint32_t height;
    uint64_t cubl_bytes;
    Vbs3FrameSummary *summaries;
    Vbs3CuIndexEntry *cu_index;
    uint32_t frame_count;
    uint32_t frame_capacity;
    Vbs3FrameSummary current_summary;
    Vbs3CuIndexEntry current_index;
    uint64_t current_cu_start;
    uint32_t current_cu_count;
    uint64_t current_cu_bytes;
    uint64_t qp_sum;
    uint8_t qp_min;
    uint8_t qp_max;
    int frame_active;
    int error;
    AVMutex lock;
    int lock_initialized;
} VoidVbs3State;

static VoidVbs3State g_vbs3;

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

static void reset_state(void)
{
    int had_lock = g_vbs3.lock_initialized;

    if (g_vbs3.file)
        fclose(g_vbs3.file);
    av_freep(&g_vbs3.summaries);
    av_freep(&g_vbs3.cu_index);
    if (had_lock)
        ff_mutex_destroy(&g_vbs3.lock);
    memset(&g_vbs3, 0, sizeof(g_vbs3));
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

static int append_frame(Vbs3FrameSummary *summary, const Vbs3CuIndexEntry *index)
{
    Vbs3FrameSummary *new_summaries;
    Vbs3CuIndexEntry *new_indices;
    uint32_t new_capacity;

    if (g_vbs3.frame_count == g_vbs3.frame_capacity) {
        new_capacity = g_vbs3.frame_capacity ? g_vbs3.frame_capacity * 2 : 256;
        if (new_capacity < g_vbs3.frame_capacity)
            return AVERROR(ENOMEM);
        new_summaries = av_realloc_array(g_vbs3.summaries, new_capacity, sizeof(*g_vbs3.summaries));
        if (!new_summaries)
            return AVERROR(ENOMEM);
        g_vbs3.summaries = new_summaries;

        new_indices = av_realloc_array(g_vbs3.cu_index, new_capacity, sizeof(*g_vbs3.cu_index));
        if (!new_indices)
            return AVERROR(ENOMEM);
        g_vbs3.cu_index = new_indices;
        g_vbs3.frame_capacity = new_capacity;
    }

    g_vbs3.summaries[g_vbs3.frame_count] = *summary;
    g_vbs3.cu_index[g_vbs3.frame_count] = *index;
    g_vbs3.frame_count++;
    return 0;
}

static void end_current_frame(void)
{
    if (!g_vbs3.frame_active || g_vbs3.error)
        return;

    g_vbs3.current_summary.num_cus = g_vbs3.current_cu_count;
    if (g_vbs3.current_cu_count) {
        g_vbs3.current_summary.avg_qp =
            (uint8_t)((g_vbs3.qp_sum + g_vbs3.current_cu_count / 2) / g_vbs3.current_cu_count);
        g_vbs3.current_summary.qp_min = g_vbs3.qp_min;
        g_vbs3.current_summary.qp_max = g_vbs3.qp_max;
    }
    g_vbs3.current_index.offset = g_vbs3.current_cu_start;
    g_vbs3.current_index.byte_size = g_vbs3.current_cu_bytes;
    g_vbs3.current_index.cu_count = g_vbs3.current_cu_count;

    g_vbs3.error = append_frame(&g_vbs3.current_summary, &g_vbs3.current_index);
    g_vbs3.frame_active = 0;
}

static int write_cu_record(const void *common, size_t common_size,
                           const void *extra, size_t extra_size,
                           uint8_t qp)
{
    int ret = 0;

    vbs3_lock();
    if (!g_vbs3.file || g_vbs3.error) {
        ret = g_vbs3.error ? g_vbs3.error : AVERROR(EINVAL);
        goto done;
    }
    if (!g_vbs3.frame_active) {
        ret = AVERROR(EINVAL);
        goto done;
    }

    if (write_exact(g_vbs3.file, common, common_size) < 0 ||
        write_exact(g_vbs3.file, extra, extra_size) < 0) {
        g_vbs3.error = AVERROR(EIO);
        ret = g_vbs3.error;
        goto done;
    }

    g_vbs3.current_cu_count++;
    g_vbs3.current_cu_bytes += common_size + extra_size;
    g_vbs3.cubl_bytes += common_size + extra_size;
    g_vbs3.qp_sum += qp;
    if (g_vbs3.current_cu_count == 1 || qp < g_vbs3.qp_min)
        g_vbs3.qp_min = qp;
    if (g_vbs3.current_cu_count == 1 || qp > g_vbs3.qp_max)
        g_vbs3.qp_max = qp;

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
    uint64_t cubl_offset;
    uint64_t fsum_offset;
    uint64_t fsum_size;
    uint64_t cuid_offset;
    uint64_t cuid_size;
    uint64_t section_table_offset;
    uint64_t file_size;
    Vbs3SectionEntry sections[3];
    Vbs3Header header;
    int ret = 0;

    if (!g_vbs3.file)
        return AVERROR(EINVAL);

    end_current_frame();
    if (g_vbs3.error) {
        ret = g_vbs3.error;
        goto done;
    }

    cubl_offset = sizeof(Vbs3Header);
    fsum_offset = cubl_offset + g_vbs3.cubl_bytes;
    fsum_size = (uint64_t)g_vbs3.frame_count * sizeof(Vbs3FrameSummary);
    cuid_offset = fsum_offset + fsum_size;
    cuid_size = (uint64_t)g_vbs3.frame_count * sizeof(Vbs3CuIndexEntry);
    section_table_offset = cuid_offset + cuid_size;
    file_size = section_table_offset + section_count * sizeof(Vbs3SectionEntry);

    sections[0] = make_section("FSUM", fsum_offset, fsum_size,
                               sizeof(Vbs3FrameSummary), g_vbs3.frame_count);
    sections[1] = make_section("CUID", cuid_offset, cuid_size,
                               sizeof(Vbs3CuIndexEntry), g_vbs3.frame_count);
    sections[2] = make_section("CUBL", cubl_offset, g_vbs3.cubl_bytes, 0,
                               g_vbs3.frame_count);

    memset(&header, 0, sizeof(header));
    set_fourcc(header.magic, "VBS3");
    header.version_major = 3;
    header.version_minor = 0;
    header.header_size = sizeof(Vbs3Header);
    header.section_entry_size = sizeof(Vbs3SectionEntry);
    header.width = g_vbs3.width;
    header.height = g_vbs3.height;
    header.frame_count = g_vbs3.frame_count;
    header.section_count = section_count;
    header.section_table_offset = section_table_offset;
    header.file_size = file_size;

    if (fseek(g_vbs3.file, 0, SEEK_END) != 0 ||
        write_exact(g_vbs3.file, g_vbs3.summaries, (size_t)fsum_size) < 0 ||
        write_exact(g_vbs3.file, g_vbs3.cu_index, (size_t)cuid_size) < 0 ||
        write_exact(g_vbs3.file, sections, sizeof(sections)) < 0 ||
        fseek(g_vbs3.file, 0, SEEK_SET) != 0 ||
        write_exact(g_vbs3.file, &header, sizeof(header)) < 0) {
        ret = AVERROR(EIO);
    }

done:
    if (fclose(g_vbs3.file) != 0 && ret == 0)
        ret = AVERROR(EIO);
    g_vbs3.file = NULL;
    av_freep(&g_vbs3.summaries);
    av_freep(&g_vbs3.cu_index);
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
    frame_count = g_vbs3.frame_count + (g_vbs3.frame_active ? 1 : 0);
    vbs3_unlock();
    return frame_count;
}

uint32_t ff_voidplayer_vbs3_last_frame_cu_count(void)
{
    uint32_t cu_count = 0;

    vbs3_lock();
    if (g_vbs3.frame_active)
        cu_count = g_vbs3.current_cu_count;
    else if (g_vbs3.frame_count)
        cu_count = g_vbs3.cu_index[g_vbs3.frame_count - 1].cu_count;
    vbs3_unlock();
    return cu_count;
}

void ff_voidplayer_vbs3_begin_frame(int32_t poc,
                                    uint32_t width,
                                    uint32_t height,
                                    uint8_t temporal_id,
                                    uint8_t slice_type,
                                    uint8_t nal_unit_type,
                                    uint8_t num_ref_l0,
                                    uint8_t num_ref_l1,
                                    const int32_t *ref_pocs_l0,
                                    const int32_t *ref_pocs_l1)
{
    int i;

    if (!ff_voidplayer_vbs3_is_active())
        return;

    vbs3_lock();
    if (g_vbs3.frame_active && g_vbs3.current_summary.poc == poc)
        goto done;

    end_current_frame();
    if (g_vbs3.error)
        goto done;

    if (width)
        g_vbs3.width = width;
    if (height)
        g_vbs3.height = height;

    memset(&g_vbs3.current_summary, 0, sizeof(g_vbs3.current_summary));
    memset(&g_vbs3.current_index, 0, sizeof(g_vbs3.current_index));
    g_vbs3.current_summary.poc = poc;
    g_vbs3.current_summary.coded_order = g_vbs3.frame_count;
    g_vbs3.current_summary.vcl_nalu_index = 0xFFFFFFFFu;
    g_vbs3.current_summary.temporal_id = temporal_id;
    g_vbs3.current_summary.slice_type = slice_type;
    g_vbs3.current_summary.nal_unit_type = nal_unit_type;
    g_vbs3.current_summary.num_ref_l0 = num_ref_l0 > 15 ? 15 : num_ref_l0;
    g_vbs3.current_summary.num_ref_l1 = num_ref_l1 > 15 ? 15 : num_ref_l1;
    g_vbs3.current_summary.cu_index_entry = g_vbs3.frame_count;
    for (i = 0; i < 15; ++i) {
        g_vbs3.current_summary.ref_pocs_l0[i] =
            (ref_pocs_l0 && i < g_vbs3.current_summary.num_ref_l0) ? ref_pocs_l0[i] : -1;
        g_vbs3.current_summary.ref_pocs_l1[i] =
            (ref_pocs_l1 && i < g_vbs3.current_summary.num_ref_l1) ? ref_pocs_l1[i] : -1;
    }
    g_vbs3.current_cu_start = g_vbs3.cubl_bytes;
    g_vbs3.current_cu_count = 0;
    g_vbs3.current_cu_bytes = 0;
    g_vbs3.qp_sum = 0;
    g_vbs3.qp_min = 0;
    g_vbs3.qp_max = 0;
    g_vbs3.frame_active = 1;

done:
    vbs3_unlock();
}

void ff_voidplayer_vbs3_write_intra_cu(uint16_t x,
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

    write_cu_record(&common, sizeof(common), &intra, sizeof(intra), qp);
}

void ff_voidplayer_vbs3_write_inter_cu(uint16_t x,
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

    write_cu_record(&common, sizeof(common), &inter, sizeof(inter), qp);
}

#ifndef AVCODEC_VOIDPLAYER_VACHUNK_H
#define AVCODEC_VOIDPLAYER_VACHUNK_H

#include <stdint.h>

enum VoidPlayerVachunkCodec {
    VOIDPLAYER_VACHUNK_CODEC_H264 = 1,
    VOIDPLAYER_VACHUNK_CODEC_HEVC = 2,
    VOIDPLAYER_VACHUNK_CODEC_VVC  = 3,
    VOIDPLAYER_VACHUNK_CODEC_VP9  = 4,
    VOIDPLAYER_VACHUNK_CODEC_MPEG2 = 5,
};

typedef struct VoidPlayerVachunkFrameInfo {
    int32_t poc;
    uint32_t width;
    uint32_t height;
    uint8_t temporal_id;
    uint8_t slice_type;
    uint8_t nal_unit_type;
    uint8_t num_ref_l0;
    uint8_t num_ref_l1;
    int32_t ref_pocs_l0[15];
    int32_t ref_pocs_l1[15];
    uint32_t expected_cus;
    uintptr_t frame_identity;
    uint64_t coded_order_key;
    uint8_t has_coded_order_key;
} VoidPlayerVachunkFrameInfo;

int ff_voidplayer_vachunk_start(const char *path,
                             uint32_t width,
                             uint32_t height,
                             uint16_t codec);
int ff_voidplayer_vachunk_start_memory(uint32_t width,
                                    uint32_t height,
                                    uint16_t codec);
int ff_voidplayer_vachunk_set_frame_window(uint64_t start_frame,
                                        uint64_t end_frame);
int ff_voidplayer_vachunk_finish_vachunk(const char *path,
                                      uint32_t source_start_frame,
                                      uint32_t source_end_frame,
                                      uint64_t base_content_revision,
                                      uint64_t generator_revision);
int ff_voidplayer_vachunk_finish(void);
void ff_voidplayer_vachunk_abort(void);
int ff_voidplayer_vachunk_is_active(void);
uint32_t ff_voidplayer_vachunk_frame_count(void);
uint32_t ff_voidplayer_vachunk_last_frame_cu_count(void);

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
                                       uint32_t bit_count);

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
                                       uint32_t bit_count);

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
                                      uint32_t bit_count);

#endif

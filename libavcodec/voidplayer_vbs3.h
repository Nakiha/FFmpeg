#ifndef AVCODEC_VOIDPLAYER_VBS3_H
#define AVCODEC_VOIDPLAYER_VBS3_H

#include <stdint.h>

typedef struct VoidPlayerVbs3FrameInfo {
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
} VoidPlayerVbs3FrameInfo;

int ff_voidplayer_vbs3_start(const char *path, uint32_t width, uint32_t height);
int ff_voidplayer_vbs3_finish(void);
void ff_voidplayer_vbs3_abort(void);
int ff_voidplayer_vbs3_is_active(void);
uint32_t ff_voidplayer_vbs3_frame_count(void);
uint32_t ff_voidplayer_vbs3_last_frame_cu_count(void);

void ff_voidplayer_vbs3_write_intra_cu(const VoidPlayerVbs3FrameInfo *info,
                                       uint16_t x,
                                       uint16_t y,
                                       uint8_t w,
                                       uint8_t h,
                                       uint8_t depth,
                                       uint8_t qp,
                                       uint8_t intra_mode,
                                       uint8_t mip_flag,
                                       uint8_t isp_mode);

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
                                       int8_t ref_l1);

#endif

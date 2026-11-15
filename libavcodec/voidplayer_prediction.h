/* LGPL-2.1-or-later. Prediction observation hooks for the analysis collector. */
#ifndef AVCODEC_VOIDPLAYER_PREDICTION_H
#define AVCODEC_VOIDPLAYER_PREDICTION_H
#include "config.h"
#include <stdint.h>

#if CONFIG_VOIDPLAYER_ANALYSIS && defined(VP_PREDICTION_ABI)
#define VP_COLLECT_PREDICTIONS 1
enum VPPredictionProduct { VP_INTRA = 1, VP_INTER = 2 };
enum VPIntraFamily { VP_DIRECTION, VP_MIP, VP_CCLM, VP_BDPCM };
enum VPSelection { VP_INFERRED, VP_CHROMA_SELECTOR, VP_CCLM_INDEX, VP_MPM, VP_REMAINDER };
enum VPInterModel { VP_OTHER, VP_TRANSLATION, VP_AFFINE4, VP_AFFINE6, VP_SBTMVP, VP_IBC };

typedef struct {
    uint16_t x, y;
    uint8_t w, h, components, tree;
    uint8_t family, mode, selection, value, isp, mrl, act, reserved;
} VPIntraPrediction;

typedef struct {
    uint16_t x, y;
    uint8_t w, h, flags, lists;
    int32_t ref_au[2];
    int16_t mv[2][2];
    int8_t ref_idx[2];
    uint8_t merge_idx, model, gpm_idx, reserved[3];
} VPInterPrediction;

_Static_assert(sizeof(VPIntraPrediction) == 16, "intra prediction ABI");
_Static_assert(sizeof(VPInterPrediction) == 32, "inter prediction ABI");
int ff_vp_wants_prediction(uintptr_t id, int product);
void ff_vp_prediction_unsupported(uintptr_t id);
void ff_vp_write_prediction(uintptr_t id, const void *record);
#else
#define VP_COLLECT_PREDICTIONS 0
#endif
#endif

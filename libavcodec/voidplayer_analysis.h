/* Analysis execution extension v1. Block/reference ABIs remain unchanged. */
#ifndef AVCODEC_VOIDPLAYER_ANALYSIS_H
#define AVCODEC_VOIDPLAYER_ANALYSIS_H
#include "libavutil/error.h"
/* Only this explicit result permits a fresh full-decoder retry. */
#define VP_ANALYSIS_RETRY_FULL FFERRTAG('V','P','R','F')
enum VoidPlayerAnalysisReason {
    VP_ANALYSIS_REASON_NONE,
    VP_ANALYSIS_REASON_CONFIG,
    VP_ANALYSIS_REASON_PARAMETER_CHANGE,
    VP_ANALYSIS_REASON_FILM_GRAIN,
    VP_ANALYSIS_REASON_PIXEL_HASH,
};
#endif

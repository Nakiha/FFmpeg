/* Analysis-only Main8 pixel pruning. Default and playback builds stay full. */
#ifndef VP_ANALYSIS_PRUNE_H
#define VP_ANALYSIS_PRUNE_H
#ifndef VP_ANALYSIS_ONLY
#define VP_ANALYSIS_ONLY 0
#endif
#ifndef VP_HEVC_NO_PIXELS
#define VP_HEVC_NO_PIXELS 1
#endif
#include "libavcodec/voidplayer_vachunk.h"
#include "libavcodec/voidplayer_analysis.h"
static inline int vp_hevc_prune_supported(const HEVCPPS *pps)
{
    const HEVCSPS *sps = pps->sps;
    return sps->ptl.general_ptl.profile_idc == AV_PROFILE_HEVC_MAIN &&
           sps->vps->vps_max_layers == 1 &&
           sps->bit_depth == 8 && sps->chroma_format_idc == 1 &&
           !sps->vui.field_seq_flag && !sps->pcm_enabled &&
           !sps->extension_present && !pps->transquant_bypass_enable_flag &&
           !pps->pps_extension_present_flag;
}
static inline int vp_hevc_prune(const HEVCContext *s, int stage)
{
#if VP_ANALYSIS_ONLY
    return s->vp_analysis_path == 1;
#else
    return 0;
#endif
}
#if VP_ANALYSIS_ONLY
/* Before the first pixel is pruned, ineligible input selects the original
 * decoder. Afterwards it must discard this DPB and restart from a proven anchor. */
static inline int vp_hevc_require_full(HEVCContext *s, int reason)
{
    s->vp_analysis_reason = reason;
    if (s->vp_analysis_started)
        return VP_ANALYSIS_RETRY_FULL;
    s->vp_analysis_path = 0;
    return 0;
}
static inline int vp_hevc_admit_config(HEVCContext *s)
{
    int pps_count = 0;
    s->vp_analysis_path = 0;
    if (!VP_HEVC_NO_PIXELS || !s->vp_analysis_mode || !ff_voidplayer_vachunk_is_active() ||
        s->avctx->thread_count != 1 || s->avctx->thread_type ||
        (s->avctx->err_recognition & AV_EF_CRCCHECK))
        return 0;
    for (int i = 0; i < HEVC_MAX_PPS_COUNT; i++) {
        const HEVCPPS *pps = s->ps.pps_list[i];
        if (!pps) continue;
        pps_count++;
        if (!vp_hevc_prune_supported(pps)) {
            s->vp_analysis_reason = VP_ANALYSIS_REASON_CONFIG;
            return 0;
        }
    }
    if (pps_count) s->vp_analysis_path = 1;
    else s->vp_analysis_reason = VP_ANALYSIS_REASON_CONFIG;
    return 0;
}
#endif
static inline int vp_hevc_parameter_set(HEVCContext *s)
{
#if VP_ANALYSIS_ONLY
    if (s->vp_analysis_path && s->vp_analysis_started)
        return vp_hevc_require_full(s, VP_ANALYSIS_REASON_PARAMETER_CHANGE);
#endif
    return 0;
}
#endif

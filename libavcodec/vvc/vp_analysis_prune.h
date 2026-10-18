/* Analysis-only VVC pixel pruning. Default and playback stay full.
 *
 * Mirrors libavcodec/hevc/vp_analysis_prune.h: entropy/parameter parsing,
 * CU/TU traversal, QP, intra modes, merge/MVP/temporal motion tables,
 * references, DPB and picture completion stay active; prediction/MC,
 * residual materialization and all loop filters (LMCS/deblock/SAO/ALF) are
 * bypassed. Analysis records are produced from parsed syntax only, so both
 * paths must publish byte-identical blocks/references.
 */
#ifndef VP_VVC_ANALYSIS_PRUNE_H
#define VP_VVC_ANALYSIS_PRUNE_H

#ifndef VP_ANALYSIS_ONLY
#define VP_ANALYSIS_ONLY 0
#endif
#ifndef VP_VVC_NO_PIXELS
#define VP_VVC_NO_PIXELS 1
#endif

#include "libavcodec/cbs_h266.h"
#include "libavcodec/defs.h"
#include "libavcodec/voidplayer_analysis.h"
#include "libavcodec/voidplayer_vachunk.h"
#include "libavcodec/vvc.h"
#include "libavcodec/vvc/dec.h"
#include "libavcodec/vvc/ps.h"

static inline int vp_vvc_prune_supported_raw(const H266RawSPS *r, int bit_depth)
{
    if (!r || !r->sps_ptl_dpb_hrd_params_present_flag)
        return 0;
    return r->profile_tier_level.general_profile_idc == AV_PROFILE_VVC_MAIN_10 &&
           bit_depth >= 8 && bit_depth <= 10 &&
           r->sps_chroma_format_idc == CHROMA_FORMAT_420 &&
           !r->profile_tier_level.ptl_multilayer_enabled_flag &&
           !r->sps_gdr_enabled_flag;
}

static inline int vp_vvc_prune_supported(const VVCSPS *sps)
{
    return sps && sps->r && vp_vvc_prune_supported_raw(sps->r, sps->bit_depth);
}

static inline int vp_vvc_prune(const VVCContext *s)
{
#if VP_ANALYSIS_ONLY
    return s->vp_analysis_path == 1;
#else
    return 0;
#endif
}

/* Per-picture prune flag for code paths that only hold a VVCLocalContext
 * (entropy/residual parsing); set once per picture from vp_analysis_path. */
#if defined(VP_ANALYSIS_ONLY) && VP_ANALYSIS_ONLY
#define VP_VVC_PRUNE_FRAME(lc) ((lc)->fc->vp_analysis_prune)
#else
#define VP_VVC_PRUNE_FRAME(lc) 0
#endif

#if VP_ANALYSIS_ONLY
/* Before the first pixel task is pruned, ineligible input selects the original
 * decoder. Afterwards it must discard this DPB and restart from a proven
 * anchor. */
static inline int vp_vvc_require_full(VVCContext *s, int reason)
{
    s->vp_analysis_reason = reason;
    if (s->vp_analysis_started)
        return VP_ANALYSIS_RETRY_FULL;
    s->vp_analysis_path = 0;
    return 0;
}

/* Called from the decoder init after extradata is read, so vp_analysis_path
 * reports the selected path before the first frame, mirroring HEVC. */
static inline int vp_vvc_admit_extradata(VVCContext *s)
{
    int eligible = 0;
    s->vp_analysis_path = 0;
    if (!VP_VVC_NO_PIXELS || !s->vp_analysis_mode || !ff_voidplayer_vachunk_is_active() ||
        s->avctx->thread_count != 1 || s->avctx->thread_type || s->avctx->hwaccel ||
        (s->avctx->err_recognition & AV_EF_CRCCHECK))
        return 0;
    for (int i = 0; i < s->current_frame.nb_units; i++) {
        const CodedBitstreamUnit *unit = &s->current_frame.units[i];
        const H266RawSPS *r;
        if (unit->type != VVC_SPS_NUT)
            continue;
        r = unit->content;
        if (vp_vvc_prune_supported_raw(r, r ? r->sps_bitdepth_minus8 + 8 : 0)) {
            eligible = 1;
        } else {
            s->vp_analysis_reason = VP_ANALYSIS_REASON_CONFIG;
            return 0;
        }
    }
    if (eligible)
        s->vp_analysis_path = 1;
    return 0;
}

/* Called per frame after parameter sets are decoded. */
static inline int vp_vvc_admit_config(VVCContext *s, const VVCSPS *sps)
{
    s->vp_analysis_path = 0;
    if (!VP_VVC_NO_PIXELS || !s->vp_analysis_mode || !ff_voidplayer_vachunk_is_active() ||
        s->avctx->thread_count != 1 || s->avctx->thread_type || s->avctx->hwaccel ||
        (s->avctx->err_recognition & AV_EF_CRCCHECK))
        return s->vp_analysis_started ? vp_vvc_require_full(s, VP_ANALYSIS_REASON_CONFIG) : 0;
    if (!vp_vvc_prune_supported(sps)) {
        s->vp_analysis_reason = VP_ANALYSIS_REASON_CONFIG;
        return s->vp_analysis_started ? vp_vvc_require_full(s, VP_ANALYSIS_REASON_CONFIG) : 0;
    }
    s->vp_analysis_path = 1;
    return 0;
}
#endif

/* Any in-band parameter set after pruning started discards this DPB and
 * retries full, including a byte-identical re-send (HEVC contract). */
static inline int vp_vvc_parameter_set(VVCContext *s)
{
#if VP_ANALYSIS_ONLY
    if (s->vp_analysis_path && s->vp_analysis_started)
        return vp_vvc_require_full(s, VP_ANALYSIS_REASON_PARAMETER_CHANGE);
#endif
    return 0;
}

/* Film-grain SEI needs real pixels and is checked where the prefix SEI is
 * decoded: the per-frame SEI context can be overwritten by a later picture
 * before the delayed frame_end, so frame_end only covers the suffix hash. */
static inline int vp_vvc_sei_grain(VVCContext *s, const VVCSEI *sei)
{
#if VP_ANALYSIS_ONLY
    if (s->vp_analysis_path &&
        ((sei->common.film_grain_characteristics &&
          sei->common.film_grain_characteristics->present) ||
         sei->common.itut_t35.aom_film_grain.enable))
        return vp_vvc_require_full(s, VP_ANALYSIS_REASON_FILM_GRAIN);
#endif
    return 0;
}

/* Decoded-picture-hash SEI needs real pixels. */
static inline int vp_vvc_frame_sei(VVCContext *s, const VVCSEI *sei)
{
#if VP_ANALYSIS_ONLY
    if (s->vp_analysis_path && sei->picture_hash.present)
        return vp_vvc_require_full(s, VP_ANALYSIS_REASON_PIXEL_HASH);
#endif
    return 0;
}

#endif /* VP_VVC_ANALYSIS_PRUNE_H */

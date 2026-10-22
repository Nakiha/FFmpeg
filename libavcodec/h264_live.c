/* H.264 completed-picture presentation owner, LGPL-2.1-or-later.
 * Decode DPB/MMCO/setup remain ordinary. Only immutable AVFrame references and
 * validated AU facts enter this owner, in packet submission order. */
#include <limits.h>
#include "h264dec.h"
#include "early_output_internal.h"
#include "thread.h"
#include "libavutil/avassert.h"
#include "libavutil/container_fifo.h"
#include "libavutil/refstruct.h"
#include "libavutil/thread.h"

#define LIVE_PENDING (H264_MAX_DPB_FRAMES + 2)
#define LIVE_PROMOTED (H264_MAX_PICTURE_COUNT * 2)

typedef struct LivePicture {
    AVFrame *frame;
    uint64_t uid, serial;
    int poc;
    int64_t tick;
} LivePicture;

typedef struct H264LiveOutput {
    AVMutex mutex;
    AVCond changed;
    uint64_t next_ticket, retiring, serial;
    LivePicture pending[LIVE_PENDING];
    uint64_t promoted[LIVE_PROMOTED];
    int nb_pending, nb_promoted;
    int active, hrd_ready, cpb_last, last_poc;
    int64_t cpb_base, cpb_msb, last_tick;
    const SPS *sps;
    const PPS *pps;
} H264LiveOutput;

static void remove_pending(H264LiveOutput *s, int index)
{
    AVFrame *frame = s->pending[index].frame;
    av_frame_unref(frame);
    memmove(s->pending + index, s->pending + index + 1,
            (--s->nb_pending - index) * sizeof(*s->pending));
    s->pending[s->nb_pending] = (LivePicture){ .frame = frame };
}

static void reset_proof(H264LiveOutput *s)
{
    while (s->nb_pending)
        remove_pending(s, s->nb_pending - 1);
    s->active = s->hrd_ready = 0;
    av_refstruct_unref(&s->sps);
    av_refstruct_unref(&s->pps);
}

static void live_free(AVRefStructOpaque opaque, void *object)
{
    H264LiveOutput *s = object;
    reset_proof(s);
    for (int i = 0; i < LIVE_PENDING; i++)
        av_frame_free(&s->pending[i].frame);
    ff_cond_destroy(&s->changed);
    ff_mutex_destroy(&s->mutex);
}

int ff_h264_live_init(H264Context *h)
{
    H264LiveOutput *s;
    if (!h->live_poc_early_output)
        return 0;
    if (ff_thread_sync_ref(h->avctx, offsetof(H264Context, live_state)) == FF_THREAD_IS_COPY)
        return 0;
    s = av_refstruct_alloc_ext(sizeof(*s), 0, NULL, live_free);
    if (!s)
        return AVERROR(ENOMEM);
    ff_mutex_init(&s->mutex, NULL);
    ff_cond_init(&s->changed, NULL);
    h->live_state = s;
    for (int i = 0; i < LIVE_PENDING; i++) {
        s->pending[i].frame = av_frame_alloc();
        if (!s->pending[i].frame)
            return AVERROR(ENOMEM);
    }
    return 0;
}

int ff_h264_live_begin(H264Context *h)
{
    H264LiveOutput *s = h->live_state;
    if (!s)
        return 0;
    ff_mutex_lock(&s->mutex);
    if (s->next_ticket == UINT64_MAX) {
        ff_mutex_unlock(&s->mutex);
        return AVERROR(EOVERFLOW);
    }
    h->live_ticket = s->next_ticket++;
    ff_mutex_unlock(&s->mutex);
    return 0;
}

/* No new eligibility is inferred from recent POCs or timestamps. Shape and
 * decode-status checks precede completion; only the owner maintains HRD time. */
int ff_h264_live_candidate(H264Context *h)
{
    const SPS *sps = h->ps.sps;
    const H264Picture *cur = h->cur_pic_ptr;
    h->live_reason = AV_EARLY_UNSUPPORTED;
    if (h->live_au_invalid) {h->live_reason=AV_EARLY_INVALID_AU;return 0;}
    if (!h->live_has_picture || !cur || !sps ||
        sps->poc_type != 0 || !sps->frame_mbs_only_flag ||
        h->ps.pps->pic_order_present || cur->needs_fg || cur->invalid_gap || h->avctx->hwaccel ||
        (h->avctx->flags2 & AV_CODEC_FLAG2_CHUNKS) ||
        h->avctx->skip_frame != AVDISCARD_DEFAULT ||
        cur->field_poc[0] != cur->field_poc[1] || cur->f->pts == AV_NOPTS_VALUE ||
        h->sei.common.frame_packing.present ||
        cur->f->decode_error_flags || (cur->decode_error_flags &&
        atomic_load_explicit(cur->decode_error_flags, memory_order_relaxed)))
        return 0;
    for (int i = 0; i < h->nb_mmco; i++)
        if (h->mmco[i].opcode == MMCO_RESET)
            return 0;
    h->live_reason=AV_EARLY_ELIGIBLE;
    if (h->live_poc_verified_even)
        return cur->poc >= 0 && !(cur->poc & 1);
    int eligible = (sps->profile_idc == 77 || sps->profile_idc == 100) &&
           sps->timing_info_present_flag && sps->fixed_frame_rate_flag &&
           !sps->pic_struct_present_flag && sps->nal_hrd_parameters_present_flag &&
           !sps->vcl_hrd_parameters_present_flag && !sps->low_delay_hrd_flag &&
           sps->cpb_cnt == 1 && sps->num_units_in_tick && sps->time_scale &&
           sps->cpb_removal_delay_length > 0 && sps->cpb_removal_delay_length <= 30 &&
           sps->dpb_output_delay_length > 0 && sps->dpb_output_delay_length <= 30 &&
           h->live_timing_count == 1 && h->live_timing_bits >=
           sps->cpb_removal_delay_length + sps->dpb_output_delay_length &&
           h->sei.picture_timing.present && h->sei.picture_timing.cpb_removal_delay >= 0 &&
           h->sei.picture_timing.dpb_output_delay >= 0 &&
           (!h->live_bp_count || (h->live_bp_sps_id == sps->sps_id &&
            h->live_bp_bits >= 2 * sps->cpb_cnt * sps->initial_cpb_removal_delay_length &&
            h->sei.buffering_period.present && h->sei.buffering_period.initial_cpb_removal_delay[0]));
    if (!eligible) h->live_reason=AV_EARLY_NO_TIMING;
    return eligible;
}

static int same_parameters(const H264LiveOutput *s, const H264Context *h)
{
    return s->sps && s->pps && s->sps->data_size == h->ps.sps->data_size &&
           !memcmp(s->sps->data, h->ps.sps->data, s->sps->data_size) &&
           s->pps->data_size == h->ps.pps->data_size &&
           !memcmp(s->pps->data, h->ps.pps->data, s->pps->data_size);
}

static int candidate_tick(H264LiveOutput *s, const H264Context *h, int64_t *tick)
{
    const SPS *sps = h->ps.sps;
    if (h->picture_idr) {
        reset_proof(s);
        if ((!h->live_poc_verified_even && !h->sei.buffering_period.present) || s->serial == UINT64_MAX)
            return 0;
        s->serial++;
        s->cpb_last = s->cpb_msb = s->cpb_base = 0;
        s->hrd_ready = 1;
        av_refstruct_replace(&s->sps, sps);
        av_refstruct_replace(&s->pps, h->ps.pps);
        *tick = h->live_poc_verified_even ? 0 : h->sei.picture_timing.dpb_output_delay;
        return 1;
    }
    if (!s->hrd_ready || !same_parameters(s, h))
        return 0;
    if (h->live_poc_verified_even) {
        *tick = 0;
        return 1;
    }
    int cpb = h->sei.picture_timing.cpb_removal_delay;
    if (cpb < s->cpb_last)
        s->cpb_msb += INT64_C(1) << sps->cpb_removal_delay_length;
    if (cpb == s->cpb_last || s->cpb_msb > INT64_MAX - INT_MAX * INT64_C(2) ||
        s->cpb_base > INT64_MAX - s->cpb_msb - INT_MAX * INT64_C(2))
        return 0;
    *tick = s->cpb_base + s->cpb_msb + cpb + h->sei.picture_timing.dpb_output_delay;
    if (h->sei.buffering_period.present) {
        s->cpb_base += s->cpb_msb + cpb;
        s->cpb_msb = s->cpb_last = 0;
    } else
        s->cpb_last = cpb;
    return 1;
}

static int promoted_index(const H264LiveOutput *s, uint64_t uid)
{
    for (int i = 0; i < s->nb_promoted; i++)
        if (s->promoted[i] == uid)
            return i;
    return -1;
}

static int pending_index(const H264LiveOutput *s, uint64_t uid)
{
    for (int i = 0; i < s->nb_pending; i++)
        if (s->pending[i].uid == uid)
            return i;
    return -1;
}

/* Prepare every extension before changing delivery identities/watermarks.
 * The baseline frame is only referenced here; failure can still return it. */
static int prepare(H264Context *h, AVFrame *ordinary, int ordinary_output,
                   H264LiveOutput *s, const int *chosen, int count)
{
    AVContainerFifo *staged = av_container_fifo_alloc_avframe(0);
    AVFrame *tmp = av_frame_alloc();
    int ret = AVERROR(ENOMEM);
    if (!staged || !tmp)
        goto end;
    for (int i = ordinary_output ? -1 : 0; i < count; i++) {
        AVFrame *source = i < 0 ? ordinary : s->pending[chosen[i]].frame;
        ret = av_frame_ref(tmp, source);
        if (ret < 0)
            goto end;
        tmp->pkt_dts = h->live_pkt_dts;
        ret = av_container_fifo_write(staged, tmp, 0);
        if (ret < 0)
            goto end;
    }
    av_assert0(!av_container_fifo_can_read(h->live_output));
    av_frame_unref(ordinary);
    ret = av_container_fifo_read(staged, ordinary, 0);
    av_assert0(ret >= 0);
    FFSWAP(AVContainerFifo *, h->live_output, staged);
    ret = 0;
end:
    av_frame_free(&tmp);
    av_container_fifo_free(&staged);
    return ret;
}

static int retire_picture(H264Context *h, AVFrame *ordinary, int *got, AVFrame *candidate)
{
    H264LiveOutput *s = h->live_state;
    int chosen[LIVE_PENDING], selected[LIVE_PENDING] = {0}, count = 0;
    int suppressed = *got ? promoted_index(s, h->live_ordinary_uid) : -1;
    int ordinary_output = *got && suppressed < 0;
    int ordinary_key = *got && (ordinary->flags & AV_FRAME_FLAG_KEY);
    int valid = candidate && candidate->buf[0];
    int64_t tick = 0, last_tick;
    int last_poc;
    LivePicture anchor = {0};

    if (h->live_has_picture) {
        int had_clock=s->hrd_ready, same=valid&&same_parameters(s,h);
        int clock=valid&&candidate_tick(s,h,&tick);
        if (!valid || !clock || s->nb_pending == LIVE_PENDING) {
            if(valid)h->live_reason=s->nb_pending==LIVE_PENDING?AV_EARLY_BOUNDED_CAPACITY:
                had_clock&&!same&&!h->picture_idr?AV_EARLY_PARAMETERS:AV_EARLY_AWAIT_ANCHOR;
            reset_proof(s);
            valid = 0;
        } else {
            LivePicture *pic = &s->pending[s->nb_pending++];
            av_frame_move_ref(pic->frame, candidate);
            pic->uid = h->cur_pic_ptr->live_uid;
            pic->poc = h->cur_pic_ptr->poc;
            pic->serial = s->serial;
            pic->tick = tick;
            for (int a = 0; a < s->nb_pending; a++)
                for (int b = a + 1; b < s->nb_pending; b++) {
                    LivePicture *pa = &s->pending[a], *pb = &s->pending[b];
                    if (pa->poc == pb->poc || (!h->live_poc_verified_even &&
                        ((pa->poc < pb->poc && pa->tick >= pb->tick) ||
                         (pa->poc > pb->poc && pa->tick <= pb->tick))))
                        valid = 0;
                }
            if (!valid || (s->active && !h->picture_idr &&
                (pic->poc <= s->last_poc || (!h->live_poc_verified_even && tick <= s->last_tick)))) {
                h->live_reason=AV_EARLY_INVALID_AU;
                reset_proof(s);
                valid = 0;
            }
        }
    }
    if (*got) {
        int index = pending_index(s, h->live_ordinary_uid);
        if (index >= 0) {
            anchor = s->pending[index];
            /* Keep only immutable scalar facts, never the recycled AVFrame. */
            anchor.frame = NULL;
            remove_pending(s, index);
        }
        if (ordinary_output && anchor.uid && anchor.serial == s->serial) {
            if (ordinary->flags & AV_FRAME_FLAG_KEY)
                s->active = 1;
            if (s->active) {
                s->last_poc = anchor.poc;
                s->last_tick = anchor.tick;
            }
        }
    }
    if (valid && h->live_poc_verified_even && h->picture_idr &&
        (!h->delayed_pic[0] || (h->delayed_pic[0] == h->cur_pic_ptr && !h->delayed_pic[1])) &&
        pending_index(s, h->cur_pic_ptr->live_uid) >= 0) {
        s->active = 1;
        s->last_poc = h->cur_pic_ptr->poc - 2;
    }
    last_poc = s->last_poc;
    last_tick = s->last_tick;
    while (s->active && valid && count < s->nb_pending &&
           s->nb_promoted + count < LIVE_PROMOTED) {
        int index = -1;
        for (int i = 0; i < s->nb_pending; i++)
            if (!selected[i] && (index < 0 || s->pending[i].poc < s->pending[index].poc))
                index = i;
        if (index < 0)
            break;
        LivePicture *pic = &s->pending[index];
        int consecutive = h->live_poc_verified_even ?
            last_poc <= INT_MAX - 2 && pic->poc == last_poc + 2 :
            last_tick <= INT64_MAX - 2 && pic->tick == last_tick + 2 && pic->poc > last_poc;
        if (pic->serial != s->serial || !consecutive)
            break;
        chosen[count++] = index;
        selected[index] = 1;
        last_poc = pic->poc;
        last_tick = pic->tick;
    }
    if (count && prepare(h, ordinary, ordinary_output, s, chosen, count) < 0) {
        h->live_reason=AV_EARLY_OUTPUT_ALLOCATION;
        reset_proof(s);
        valid=0;
        count = 0; /* No extension was committed; ordinary is still intact. */
    }
    if (h->live_has_picture) {
        if (valid)h->live_reason=count?AV_EARLY_ELIGIBLE:s->active?AV_EARLY_PRESENTATION_GAP:AV_EARLY_AWAIT_ANCHOR;
        ff_early_output_picture(h->avctx,h->live_reason,h->live_candidate_qualified);
    }
    ff_early_output_frames(h->avctx,AV_EARLY_ORDINARY,ordinary_output);
    ff_early_output_frames(h->avctx,h->live_poc_verified_even?AV_EARLY_H264_VERIFIED:AV_EARLY_H264_HRD,count);
    if (suppressed >= 0) {
        ff_early_output_suppressed(h->avctx);
        memmove(s->promoted + suppressed, s->promoted + suppressed + 1,
                (--s->nb_promoted - suppressed) * sizeof(*s->promoted));
    }
    if (count) {
        for (int i = 0; i < count; i++)
            s->promoted[s->nb_promoted++] = s->pending[chosen[i]].uid;
        s->last_poc = last_poc;
        s->last_tick = last_tick;
        for (int i = s->nb_pending - 1; i >= 0; i--)
            if (selected[i])
                remove_pending(s, i);
        *got = 1;
    } else if (!ordinary_output) {
        av_frame_unref(ordinary);
        *got = 0;
    }
    /* Once ordinary presentation reaches an IDR, no earlier CVS output can
     * follow it. Retire obsolete duplicate-suppression identities as well. */
    if (ordinary_key) {
        for (int i = s->nb_promoted - 1; i >= 0; i--)
            if (s->promoted[i] < h->live_ordinary_uid) {
                memmove(s->promoted + i, s->promoted + i + 1,
                        (--s->nb_promoted - i) * sizeof(*s->promoted));
            }
    }
    return 0;
}

int ff_h264_live_retire(H264Context *h, AVFrame *ordinary, int *got,
                        AVFrame *candidate, int decode_result)
{
    H264LiveOutput *s = h->live_state;
    if (!s) {
        if (decode_result>=0) {
            if (h->live_has_picture)ff_early_output_picture(h->avctx,AV_EARLY_DISABLED,0);
            ff_early_output_frames(h->avctx,AV_EARLY_ORDINARY,*got);
        }
        return decode_result;
    }
    ff_mutex_lock(&s->mutex);
    while (h->live_ticket != s->retiring)
        ff_cond_wait(&s->changed, &s->mutex);
    if (decode_result < 0) {
        ff_early_output_picture(h->avctx,AV_EARLY_INVALID_AU,0);
        reset_proof(s);
        av_frame_unref(ordinary);
        *got = 0;
    } else
        retire_picture(h, ordinary, got, candidate);
    s->retiring++;
    ff_cond_broadcast(&s->changed);
    ff_mutex_unlock(&s->mutex);
    return decode_result;
}

void ff_h264_live_flush(H264Context *h)
{
    H264LiveOutput *s = h->live_state;
    if (!s)
        return;
    /* External flush parks all frame workers before codec flush callbacks. */
    ff_mutex_lock(&s->mutex);
    av_assert0(s->next_ticket == s->retiring);
    reset_proof(s);
    s->nb_promoted = 0;
    ff_mutex_unlock(&s->mutex);
}

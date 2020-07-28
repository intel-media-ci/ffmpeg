/*
 * AV1video decoder
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "avcodec.h"
#include "get_bits.h"
#include "hwconfig.h"
#include "internal.h"
#include "profiles.h"
#include "thread.h"
#include "videodsp.h"
#include "libavutil/avassert.h"
#include "libavutil/pixdesc.h"

#include "av1dec.h"
#include "libavformat/av1.h"

static void setup_past_independence(AV1Frame *f)
{
    f->loop_filter_delta_enabled = 1;

    f->loop_filter_ref_deltas[AV1_REF_FRAME_INTRA] = 1;
    f->loop_filter_ref_deltas[AV1_REF_FRAME_LAST] = 0;
    f->loop_filter_ref_deltas[AV1_REF_FRAME_LAST2] = 0;
    f->loop_filter_ref_deltas[AV1_REF_FRAME_LAST3] = 0;
    f->loop_filter_ref_deltas[AV1_REF_FRAME_GOLDEN] = -1;
    f->loop_filter_ref_deltas[AV1_REF_FRAME_BWDREF] = 0;
    f->loop_filter_ref_deltas[AV1_REF_FRAME_ALTREF2] = -1;
    f->loop_filter_ref_deltas[AV1_REF_FRAME_ALTREF] = -1;

    f->loop_filter_mode_deltas[0] = 0;
    f->loop_filter_mode_deltas[1] = 0;
}

static void load_previous_and_update(AV1DecContext *s)
{
    int i;

    memcpy(s->cur_frame.loop_filter_ref_deltas,
           s->ref[s->raw_frame_header.primary_ref_frame].loop_filter_ref_deltas,
           AV1_TOTAL_REFS_PER_FRAME * sizeof(int8_t));
    memcpy(s->cur_frame.loop_filter_mode_deltas,
           s->ref[s->raw_frame_header.primary_ref_frame].loop_filter_mode_deltas,
           2 * sizeof(int8_t));

    if (s->raw_frame_header.loop_filter_delta_update) {
        for (i = 0; i < AV1_TOTAL_REFS_PER_FRAME; i++) {
            if (s->raw_frame_header.update_ref_delta[i])
                s->cur_frame.loop_filter_ref_deltas[i] =
                    s->raw_frame_header.loop_filter_ref_deltas[i];
        }

        for (i = 0; i < 2; i++) {
            if (s->raw_frame_header.update_mode_delta[i])
                s->cur_frame.loop_filter_mode_deltas[i] =
                    s->raw_frame_header.loop_filter_mode_deltas[i];
        }
    }

    s->cur_frame.loop_filter_delta_enabled =
        s->raw_frame_header.loop_filter_delta_enabled;
}

static uint32_t inverse_recenter(int r, uint32_t v)
{
    if (v > 2 * r)
        return v;
    else if (v & 1)
        return r - ((v + 1) >> 1);
    else
        return r + (v >> 1);
}

static uint32_t decode_unsigned_subexp_with_ref(uint32_t sub_exp,
                                                int mx, int r)
{
    if ((r << 1) <= mx) {
        return inverse_recenter(r, sub_exp);
    } else {
        return mx - 1 - inverse_recenter(mx - 1 - r, sub_exp);
    }
}

static int32_t decode_signed_subexp_with_ref(uint32_t sub_exp, int low,
                                             int high, int r)
{
    int32_t x = decode_unsigned_subexp_with_ref(sub_exp, high - low, r - low);
    return x + low;
}

static void read_global_param(AV1DecContext *s, int type, int ref, int idx)
{
    uint32_t abs_bits, prec_bits, round, prec_diff, sub, mx, r;
    abs_bits = AV1_GM_ABS_ALPHA_BITS;
    prec_bits = AV1_GM_ALPHA_PREC_BITS;

    if (idx < 2) {
        if (type == AV1_WARP_MODEL_TRANSLATION) {
            abs_bits = AV1_GM_ABS_TRANS_ONLY_BITS -
                !s->raw_frame_header.allow_high_precision_mv;
            prec_bits = AV1_GM_TRANS_ONLY_PREC_BITS -
                !s->raw_frame_header.allow_high_precision_mv;
        } else {
            abs_bits = AV1_GM_ABS_TRANS_BITS;
            prec_bits = AV1_GM_TRANS_PREC_BITS;
        }
    }
    round = (idx % 3) == 2 ? (1 << AV1_WARPEDMODEL_PREC_BITS) : 0;
    prec_diff = AV1_WARPEDMODEL_PREC_BITS - prec_bits;
    sub = (idx % 3) == 2 ? (1 << prec_bits) : 0;
    mx = 1 << abs_bits;
    r = (s->ref[s->raw_frame_header.primary_ref_frame].gm_params[ref][idx]
         >> prec_diff) - sub;

    s->cur_frame.gm_params[ref][idx] =
        (decode_signed_subexp_with_ref(s->raw_frame_header.gm_params[ref][idx],
                                       -mx, mx + 1, r) << prec_diff) + round;
}

/**
* update gm type/params, since cbs already implemented part of this funcation,
* so we don't need to full implement spec.
*/
static void global_motion_params(AV1DecContext *s)
{
    const AV1RawFrameHeader *header = &s->raw_frame_header;
    int type;
    int i, j;
    for (i = AV1_REF_FRAME_LAST; i <= AV1_REF_FRAME_ALTREF; i++) {
        s->cur_frame.gm_type[i] = AV1_WARP_MODEL_IDENTITY;
        for (j = 0; j <= 5; j++)
            s->cur_frame.gm_params[i][j] = (j % 3 == 2) ?
                                           1 << AV1_WARPEDMODEL_PREC_BITS : 0;
    }
    if (header->frame_type == AV1_FRAME_KEY ||
        header->frame_type == AV1_FRAME_INTRA_ONLY)
        return;

    for (i = AV1_REF_FRAME_LAST; i <= AV1_REF_FRAME_ALTREF; i++) {
        if (header->is_global[i]) {
            if (header->is_rot_zoom[i]) {
                type = AV1_WARP_MODEL_ROTZOOM;
            } else {
                type = header->is_translation[i] ? AV1_WARP_MODEL_TRANSLATION
                                                 : AV1_WARP_MODEL_AFFINE;
            }
        } else {
            type = AV1_WARP_MODEL_IDENTITY;
        }
        s->cur_frame.gm_type[i] = type;

        if (type >= AV1_WARP_MODEL_ROTZOOM) {
            read_global_param(s, type, i, 2);
            read_global_param(s, type, i, 3);
            if (type == AV1_WARP_MODEL_AFFINE) {
                read_global_param(s, type, i, 4);
                read_global_param(s, type, i, 5);
            } else {
                s->cur_frame.gm_params[i][4] = - s->cur_frame.gm_params[i][3];
                s->cur_frame.gm_params[i][5] = - s->cur_frame.gm_params[i][2];
            }
        }
        if (type >= AV1_WARP_MODEL_TRANSLATION) {
            read_global_param(s, type, i, 0);
            read_global_param(s, type, i, 1);
        }
    }
}

static int get_tiles_info(AVCodecContext *avctx,
                          AV1RawTileGroup *tile_group,
                          uint32_t tile_group_offset)
{
    AV1DecContext *s = avctx->priv_data;
    GetBitContext gb;
    uint16_t tile_num, tile_row, tile_col;
    uint16_t mi_cols, mi_rows, sb_cols, sb_rows, tile_width_sb, tile_height_sb;
    uint32_t size = 0, size_bytes = 0, offset = 0;
    int ret = 0;

    if ((ret = init_get_bits8(&gb,
                              tile_group->tile_data.data,
                              tile_group->tile_data.data_size)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Fail to initialize bitstream reader.\n");
        return ret;
    }

    s->tg_start = tile_group->tg_start;
    s->tg_end = tile_group->tg_end;

    if (s->raw_frame_header.uniform_tile_spacing_flag) {
        mi_cols = 2 * ((s->raw_seq.max_frame_width_minus_1 + 8) >> 3);
        mi_rows = 2 * ((s->raw_seq.max_frame_height_minus_1 + 8) >> 3);
        sb_cols = s->raw_seq.use_128x128_superblock ? ((mi_cols + 31) >> 5)
                                                    : ((mi_cols + 15) >> 4);
        sb_rows = s->raw_seq.use_128x128_superblock ? ((mi_rows + 31) >> 5)
                                                    : ((mi_rows + 15) >> 4);
        tile_width_sb = (sb_cols + (1 << s->raw_frame_header.tile_cols_log2)
                         -1) >> s->raw_frame_header.tile_cols_log2;
        tile_height_sb = (sb_rows + (1 << s->raw_frame_header.tile_rows_log2)
                         -1) >> s->raw_frame_header.tile_rows_log2;
    }

    for (tile_num = tile_group->tg_start; tile_num <= tile_group->tg_end; tile_num++) {
        tile_row = tile_num / s->raw_frame_header.tile_cols;
        tile_col = tile_num % s->raw_frame_header.tile_cols;

        if (tile_num == tile_group->tg_end) {
            s->tile_group_info[tile_num].tile_group_offset = tile_group_offset;
            s->tile_group_info[tile_num].tile_size = get_bits_left(&gb) / 8;
            s->tile_group_info[tile_num].tile_offset = offset;
            s->tile_group_info[tile_num].tile_row = tile_row;
            s->tile_group_info[tile_num].tile_column = tile_col;
            s->tile_group_info[tile_num].tile_size_bytes = size_bytes;
            if (s->raw_frame_header.uniform_tile_spacing_flag) {
                s->tile_group_info[tile_num].tile_width_sbs =
                    (tile_col + 1) == s->raw_frame_header.tile_cols ?
                    sb_cols - tile_col * tile_width_sb :
                    tile_width_sb;
                s->tile_group_info[tile_num].tile_height_sbs =
                    (tile_row + 1) == s->raw_frame_header.tile_rows ?
                    sb_rows - tile_row * tile_height_sb :
                    tile_height_sb;
            } else {
                s->tile_group_info[tile_num].tile_width_sbs =
                    s->raw_frame_header.width_in_sbs_minus_1[tile_col] + 1;
                s->tile_group_info[tile_num].tile_height_sbs =
                    s->raw_frame_header.height_in_sbs_minus_1[tile_row] + 1;
            }
            return 0;
        }
        size_bytes = s->raw_frame_header.tile_size_bytes_minus1 + 1;
        size = get_bits_le(&gb, size_bytes * 8) + 1;
        skip_bits(&gb, size * 8);

        offset += size_bytes;

        s->tile_group_info[tile_num].tile_group_offset = tile_group_offset;
        s->tile_group_info[tile_num].tile_size = size;
        s->tile_group_info[tile_num].tile_offset = offset;
        s->tile_group_info[tile_num].tile_row = tile_row;
        s->tile_group_info[tile_num].tile_column = tile_col;
        s->tile_group_info[tile_num].tile_size_bytes = size_bytes;
        if (s->raw_frame_header.uniform_tile_spacing_flag) {
            s->tile_group_info[tile_num].tile_width_sbs =
                (tile_col + 1) == s->raw_frame_header.tile_cols ?
                sb_cols - tile_col * tile_width_sb :
                tile_width_sb;
            s->tile_group_info[tile_num].tile_height_sbs =
                (tile_row + 1) == s->raw_frame_header.tile_rows ?
                sb_rows - tile_row * tile_height_sb :
                tile_height_sb;
        } else {
            s->tile_group_info[tile_num].tile_width_sbs =
                s->raw_frame_header.width_in_sbs_minus_1[tile_col] + 1;
            s->tile_group_info[tile_num].tile_height_sbs =
                s->raw_frame_header.height_in_sbs_minus_1[tile_row] + 1;
        }
        offset += size;
    }

    return 0;

}

static int get_pixel_format(AVCodecContext *avctx)
{
    AV1DecContext *s = avctx->priv_data;
    const AV1RawSequenceHeader *seq = &s->raw_seq;
    uint8_t bit_depth;
    int ret = 0;
    enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
#define HWACCEL_MAX (0)
    enum AVPixelFormat pix_fmts[HWACCEL_MAX + 2], *fmtp = pix_fmts;

    if (seq->seq_profile == 2 && seq->color_config.high_bitdepth)
        bit_depth = seq->color_config.twelve_bit ? 12 : 10;
    else if (seq->seq_profile <= 2)
        bit_depth = seq->color_config.high_bitdepth ? 10 : 8;
    else {
        av_log(avctx, AV_LOG_ERROR,
               "Unknow av1 profile %d.\n", seq->seq_profile);
        return -1;
    }

    if (!seq->color_config.mono_chrome) {
        // 4:4:4 x:0 y:0, 4:2:2 x:1 y:0, 4:2:0 x:1 y:1
        if (seq->color_config.subsampling_x == 0 &&
            seq->color_config.subsampling_y == 0) {
            if (bit_depth == 8)
                pix_fmt = AV_PIX_FMT_YUV444P;
            else if (bit_depth == 10)
                pix_fmt = AV_PIX_FMT_YUV444P10;
            else if (bit_depth == 12)
                pix_fmt = AV_PIX_FMT_YUV444P12;
            else
                av_log(avctx, AV_LOG_WARNING, "Unknow av1 pixel format.\n");
        } else if (seq->color_config.subsampling_x == 1 &&
                   seq->color_config.subsampling_y == 0) {
            if (bit_depth == 8)
                pix_fmt = AV_PIX_FMT_YUV422P;
            else if (bit_depth == 10)
                pix_fmt = AV_PIX_FMT_YUV422P10;
            else if (bit_depth == 12)
                pix_fmt = AV_PIX_FMT_YUV422P12;
            else
                av_log(avctx, AV_LOG_WARNING, "Unknow av1 pixel format.\n");
        } else if (seq->color_config.subsampling_x == 1 &&
                   seq->color_config.subsampling_y == 1) {
            if (bit_depth == 8)
                pix_fmt = AV_PIX_FMT_YUV420P;
            else if (bit_depth == 10)
                pix_fmt = AV_PIX_FMT_YUV420P10;
            else if (bit_depth == 12)
                pix_fmt = AV_PIX_FMT_YUV420P12;
            else
                av_log(avctx, AV_LOG_WARNING, "Unknow av1 pixel format.\n");
        }
    } else {
        if (seq->color_config.subsampling_x == 1 &&
            seq->color_config.subsampling_y == 1)
            pix_fmt = AV_PIX_FMT_YUV440P;
        else
            av_log(avctx, AV_LOG_WARNING, "Unknow av1 pixel format.\n");
    }

    av_log(avctx, AV_LOG_DEBUG, "Av1 decode get format: %s.\n",
           av_get_pix_fmt_name(pix_fmt));

    if (pix_fmt == AV_PIX_FMT_NONE)
        return -1;
    s->pix_fmt = pix_fmt;

    *fmtp++ = s->pix_fmt;
    *fmtp = AV_PIX_FMT_NONE;
    ret = ff_thread_get_format(avctx, pix_fmts);
    if (ret < 0)
        return ret;

    avctx->pix_fmt = ret;

    return 0;
}

static void av1_frame_unref(AVCodecContext *avctx, AV1Frame *f)
{
    ff_thread_release_buffer(avctx, &f->tf);
    av_buffer_unref(&f->hwaccel_priv_buf);
    f->hwaccel_picture_private = NULL;
}

static int av1_frame_ref(AVCodecContext *avctx, AV1Frame *dst, AV1Frame *src)
{
    int ret = 0;

    ret = ff_thread_ref_frame(&dst->tf, &src->tf);
    if (ret < 0)
        return ret;

    if (src->hwaccel_picture_private) {
        dst->hwaccel_priv_buf = av_buffer_ref(src->hwaccel_priv_buf);
        if (!dst->hwaccel_priv_buf)
            goto fail;
        dst->hwaccel_picture_private = dst->hwaccel_priv_buf->data;
    }

    dst->loop_filter_delta_enabled = src->loop_filter_delta_enabled;
    memcpy(dst->loop_filter_ref_deltas,
           src->loop_filter_ref_deltas,
           AV1_TOTAL_REFS_PER_FRAME * sizeof(int8_t));
    memcpy(dst->loop_filter_mode_deltas,
           src->loop_filter_mode_deltas,
           2 * sizeof(int8_t));
    memcpy(dst->gm_type,
           src->gm_type,
           AV1_TOTAL_REFS_PER_FRAME * sizeof(uint8_t));
    memcpy(dst->gm_params,
           src->gm_params,
           AV1_TOTAL_REFS_PER_FRAME * 6 * sizeof(int8_t));

    return 0;

fail:
    av1_frame_unref(avctx, dst);
    return AVERROR(ENOMEM);
}

static av_cold int av1_decode_free(AVCodecContext *avctx)
{
    AV1DecContext *s = avctx->priv_data;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(s->ref); i++) {
        if (s->ref[i].tf.f->buf[0])
            av1_frame_unref(avctx, &s->ref[i]);
        av_frame_free(&s->ref[i].tf.f);
    }
    if (s->cur_frame.tf.f->buf[0])
        av1_frame_unref(avctx, &s->cur_frame);
    av_frame_free(&s->cur_frame.tf.f);

    ff_cbs_fragment_free(&s->current_obu);
    ff_cbs_close(&s->cbc);

    return 0;
}

static av_cold int av1_decode_init(AVCodecContext *avctx)
{
    AV1DecContext *s = avctx->priv_data;
    int i;
    int ret = 0;

    for (i = 0; i < FF_ARRAY_ELEMS(s->ref); i++) {
        s->ref[i].tf.f = av_frame_alloc();
        if (!s->ref[i].tf.f) {
            av1_decode_free(avctx);
            av_log(avctx, AV_LOG_ERROR,
                   "Fail to allocate frame buffer %d.\n", i);
            return AVERROR(ENOMEM);
        }
    }

        s->cur_frame.tf.f = av_frame_alloc();
        if (!s->cur_frame.tf.f) {
            av1_decode_free(avctx);
            av_log(avctx, AV_LOG_ERROR, "Fail to allocate frame buffer.\n");
            return AVERROR(ENOMEM);
        }

    s->avctx = avctx;
    s->pix_fmt = AV_PIX_FMT_NONE;

    ret = ff_cbs_init(&s->cbc, AV_CODEC_ID_AV1, avctx);
    if (ret < 0)
        return ret;
    return 0;
}

static int av1_frame_alloc(AVCodecContext *avctx, AV1Frame *f)
{
    int ret;
    if ((ret = ff_thread_get_buffer(avctx, &f->tf, AV_GET_BUFFER_FLAG_REF)) < 0)
        return ret;

    if (avctx->hwaccel) {
        const AVHWAccel *hwaccel = avctx->hwaccel;
        if (hwaccel->frame_priv_data_size) {
            f->hwaccel_priv_buf =
                av_buffer_allocz(hwaccel->frame_priv_data_size);
            if (!f->hwaccel_priv_buf)
                goto fail;
            f->hwaccel_picture_private = f->hwaccel_priv_buf->data;
        }
    }
    return 0;

fail:
    av1_frame_unref(avctx, f);
    return AVERROR(ENOMEM);
}

static int set_output_frame(AVFrame *srcframe, AVFrame *frame, AVPacket *pkt)
{
    int ret = 0;
    if ((ret = av_frame_ref(frame, srcframe)) < 0) {
        return ret;
    }

    frame->pts = pkt->pts;
    frame->pkt_dts = pkt->dts;

    return 0;
}

static int update_reference_list (AVCodecContext *avctx)
{
    AV1DecContext *s = avctx->priv_data;
    AV1RawFrameHeader *header= &s->raw_frame_header;
    int i;
    int ret = 0;

    for (i = 0; i < 8; i++) {
        if (header->frame_type == AV1_FRAME_KEY ||
            (header->refresh_frame_flags & (1 << i))) {
            if (s->ref[i].tf.f->buf[0])
                av1_frame_unref(avctx, &s->ref[i]);
            if ((ret = av1_frame_ref(avctx, &s->ref[i], &s->cur_frame)) < 0) {
                av_log(avctx, AV_LOG_DEBUG, "Ref frame error:%d.\n", ret);
                return ret;
            }
        }
    }
    return 0;
}

static int get_current_frame(AVCodecContext *avctx)
{
    AV1DecContext *s = avctx->priv_data;
    int ret = 0;

    if (s->cur_frame.tf.f->buf[0])
        av1_frame_unref(avctx, &s->cur_frame);

    ret = av1_frame_alloc(avctx, &s->cur_frame);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Get current frame fail.\n");
        return ret;
    }

    if (s->raw_frame_header.primary_ref_frame == AV1_PRIMARY_REF_NONE)
        setup_past_independence(&s->cur_frame);
    else
        load_previous_and_update(s);

    global_motion_params(s);

    return ret;
}

static int decode_current_frame(AVCodecContext *avctx)
{
    int ret = 0;

    if (avctx->hwaccel) {
        ret = avctx->hwaccel->start_frame(avctx, NULL, 0);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "HW accel strat frame fail.\n");
            return ret;
        }

        ret = avctx->hwaccel->end_frame(avctx);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "HW accel end frame fail.\n");
            return ret;
        }
    } else {
        av_log(avctx, AV_LOG_ERROR, "Your platform doesn't suppport hardware "
               "acceleration AV1 decode.\n");
        return -1;
    }

    ret = update_reference_list(avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Fail to update reference list.\n");
        return ret;
    }
    return 0;
}

static int av1_decode_frame(AVCodecContext *avctx, void *frame,
                            int *got_frame, AVPacket *pkt)
{
    AV1DecContext *s = avctx->priv_data;
    AV1RawSequenceHeader* raw_seq = NULL;
    AV1RawFrameHeader *raw_frame_header = NULL;
    AV1RawTileGroup *raw_tile_group = NULL;
    uint32_t tile_group_offset = 0;
    int i;
    int ret = 0;

    ret = ff_cbs_read_packet(s->cbc, &s->current_obu, pkt);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Fail to read packet.\n");
        goto end;
    }
    av_log(avctx, AV_LOG_DEBUG, "Total obu for this frame:%d.\n",
           s->current_obu.nb_units);

    for (i = 0; i < s->current_obu.nb_units; i++) {
        CodedBitstreamUnit *unit = &s->current_obu.units[i];
        AV1RawOBU *obu = unit->content;
        av_log(avctx, AV_LOG_DEBUG, "Obu idx:%d, obu type:%d.\n", i, unit->type);

        switch (unit->type) {
        case AV1_OBU_SEQUENCE_HEADER:
            raw_seq = &obu->obu.sequence_header;
            memcpy(&s->raw_seq, raw_seq, sizeof(AV1RawSequenceHeader));
            if (s->pix_fmt == AV_PIX_FMT_NONE) {
                ret = get_pixel_format(avctx);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR,
                           "Fail to get format from sequence header.\n");
                    goto end;
                }
            }
            break;
        case AV1_OBU_FRAME_HEADER:
        case AV1_OBU_REDUNDANT_FRAME_HEADER:
            raw_frame_header = &obu->obu.frame_header;
            memcpy (&s->raw_frame_header,
                    raw_frame_header,
                    sizeof(AV1RawFrameHeader));
            s->tile_num =
                raw_frame_header->tile_cols * raw_frame_header->tile_rows;
            if (raw_frame_header->show_existing_frame) {
                if (set_output_frame(s->ref[raw_frame_header->frame_to_show_map_idx].tf.f,
                                     frame, pkt) < 0) {
                    av_log(avctx, AV_LOG_DEBUG, "Set output frame error:%d.\n",
                           ret);
                    goto end;
                }
                *got_frame = 1;
                goto end;
            } else {
                ret = get_current_frame(avctx);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_DEBUG, "Get current frame error:%d.\n",
                           ret);
                    goto end;
                }
            }
            break;
        case AV1_OBU_FRAME:
            raw_frame_header = &obu->obu.frame.header;
            raw_tile_group = &obu->obu.frame.tile_group;
            memcpy(&s->raw_frame_header,
                   raw_frame_header,
                   sizeof(AV1RawFrameHeader));
            s->tile_num =
                raw_frame_header->tile_cols * raw_frame_header->tile_rows;
            tile_group_offset = raw_tile_group->tile_data.data - pkt->data;
            get_tiles_info(avctx, raw_tile_group, tile_group_offset);

            ret = get_current_frame(avctx);
            if (ret < 0) {
                av_log(avctx, AV_LOG_DEBUG, "Get current frame error:%d.\n",
                       ret);
                goto end;
            }
            if (avctx->hwaccel) {
                ret = avctx->hwaccel->decode_slice(avctx, pkt->data, pkt->size);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR,
                           "HW accel decode slice fail.\n");
                    return ret;
                }
            }
            ret = decode_current_frame(avctx);
            if (ret < 0) {
                av_log(avctx, AV_LOG_DEBUG, "Decode current frame error:%d.\n",
                       ret);
                goto end;
            }
            if (raw_frame_header->show_frame) {
                if (set_output_frame(s->cur_frame.tf.f, frame, pkt) < 0) {
                    av_log(avctx, AV_LOG_DEBUG, "Set output frame error:%d.\n",
                           ret);
                    goto end;
                }

                *got_frame = 1;
            }
            break;
        case AV1_OBU_TILE_GROUP:
            raw_tile_group = &obu->obu.tile_group;
            tile_group_offset = raw_tile_group->tile_data.data - pkt->data;
            get_tiles_info(avctx, raw_tile_group, tile_group_offset);

            if (avctx->hwaccel) {
                ret = avctx->hwaccel->decode_slice(avctx, pkt->data, pkt->size);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR,
                           "HW accel decode slice fail.\n");
                    return ret;
                }
            }
            if (s->tile_num != raw_tile_group->tg_end + 1)
                break;
            ret = decode_current_frame(avctx);
            if (ret < 0) {
                av_log(avctx, AV_LOG_DEBUG, "Decode current frame error:%d.\n",
                       ret);
                goto end;
            }

            if (raw_frame_header->show_frame) {
                if (set_output_frame(s->cur_frame.tf.f, frame, pkt) < 0) {
                    av_log(avctx, AV_LOG_DEBUG, "Set output frame error:%d.\n",
                           ret);
                    goto end;
                }

                *got_frame = 1;
            }
            break;
        case AV1_OBU_TILE_LIST:
        case AV1_OBU_TEMPORAL_DELIMITER:
        case AV1_OBU_PADDING:
        case AV1_OBU_METADATA:
            break;
        default:
            av_log(avctx, AV_LOG_DEBUG,
                   "Un-implemented obu type: %d (%zd bits).\n",
                   unit->type, unit->data_size);
        }
    }

end:
    ff_cbs_fragment_reset(&s->current_obu);
    return ret;
}

static void av1_decode_flush(AVCodecContext *avctx)
{
    AV1DecContext *s = avctx->priv_data;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(s->ref); i++)
        av1_frame_unref(avctx, &s->ref[i]);

    av1_frame_unref(avctx, &s->cur_frame);
}

AVCodec ff_av1_decoder = {
    .name                  = "av1",
    .long_name             = NULL_IF_CONFIG_SMALL("Hardware Acceleration AV1"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_AV1,
    .priv_data_size        = sizeof(AV1DecContext),
    .init                  = av1_decode_init,
    .close                 = av1_decode_free,
    .decode                = av1_decode_frame,
    .capabilities          = AV_CODEC_CAP_DR1,
    .flush                 = av1_decode_flush,
    .profiles              = NULL_IF_CONFIG_SMALL(ff_av1_profiles),
    .hw_configs            = (const AVCodecHWConfigInternal * []) {
        NULL
    },
};

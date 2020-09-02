/*
 * AV1 HW decode acceleration through VA API
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

#include "libavutil/pixdesc.h"
#include "hwconfig.h"
#include "vaapi_decode.h"
#include "av1dec.h"

static VASurfaceID vaapi_av1_surface_id(AV1Frame *vf)
{
    if (vf)
        return ff_vaapi_get_surface_id(vf->tf.f);
    else
        return VA_INVALID_SURFACE;
}

static VAAV1TransformationType vaapi_av1_get_gm_type(uint8_t type)
{
    if (type == AV1_WARP_MODEL_IDENTITY)
       return VAAV1TransformationIdentity;
    else if (type == AV1_WARP_MODEL_TRANSLATION)
       return VAAV1TransformationTranslation;
    else if (type == AV1_WARP_MODEL_ROTZOOM)
       return VAAV1TransformationRotzoom;
    else if (type == AV1_WARP_MODEL_AFFINE)
       return VAAV1TransformationAffine;
    else
       return VAAV1TransformationIdentity;
}

static uint8_t vaapi_av1_get_bit_depth_idx(AVCodecContext *avctx)
{
    AV1DecContext *s = avctx->priv_data;
    const AV1RawSequenceHeader *seq = s->raw_seq;
    uint8_t bit_depth = 8;

    if (seq->seq_profile == 2 && seq->color_config.high_bitdepth)
        bit_depth = seq->color_config.twelve_bit ? 12 : 10;
    else if (seq->seq_profile <= 2)
        bit_depth = seq->color_config.high_bitdepth ? 10 : 8;
    else {
        av_log(avctx, AV_LOG_ERROR,
               "Couldn't get bit depth from profile:%d.\n", seq->seq_profile);
    }
    return bit_depth == 8 ? 0 : bit_depth == 10 ? 1 : 2;
}

static int vaapi_av1_start_frame(AVCodecContext *avctx,
                                 av_unused const uint8_t *buffer,
                                 av_unused uint32_t size)
{
    AV1DecContext *s = avctx->priv_data;
    const AV1RawSequenceHeader *seq = s->raw_seq;
    const AV1RawFrameHeader *frame_header = s->raw_frame_header;
    VAAPIDecodePicture *pic = s->cur_frame.hwaccel_picture_private;
    VADecPictureParameterBufferAV1 pic_param;
    int err = 0;
    uint8_t lr_type[4] = {AV1_RESTORE_NONE, AV1_RESTORE_SWITCHABLE, AV1_RESTORE_WIENER, AV1_RESTORE_SGRPROJ};

    pic->output_surface = vaapi_av1_surface_id(&s->cur_frame);

    memset(&pic_param, 0, sizeof(VADecPictureParameterBufferAV1));
    pic_param = (VADecPictureParameterBufferAV1) {
        .profile = seq->seq_profile,
        .order_hint_bits_minus_1 = seq->order_hint_bits_minus_1,
        .bit_depth_idx = vaapi_av1_get_bit_depth_idx(avctx),
        .seq_info_fields.fields = {
            .still_picture = seq->still_picture,
            .use_128x128_superblock = seq->use_128x128_superblock,
            .enable_filter_intra = seq->enable_filter_intra,
            .enable_intra_edge_filter = seq->enable_intra_edge_filter,
            .enable_interintra_compound = seq->enable_interintra_compound,
            .enable_masked_compound = seq->enable_masked_compound,
            .enable_dual_filter = seq->enable_dual_filter,
            .enable_order_hint = seq->enable_order_hint,
            .enable_jnt_comp = seq->enable_jnt_comp,
            .enable_cdef = seq->enable_cdef,
            .mono_chrome = seq->color_config.mono_chrome,
            .color_range = seq->color_config.color_range,
            .subsampling_x = seq->color_config.subsampling_x,
            .subsampling_y = seq->color_config.subsampling_y,
            .chroma_sample_position = seq->color_config.chroma_sample_position,
            .film_grain_params_present  = seq->film_grain_params_present,
        },
        .current_frame = pic->output_surface,
        .current_display_picture = pic->output_surface,
        .frame_width_minus1 = frame_header->frame_width_minus_1,
        .frame_height_minus1 = frame_header->frame_height_minus_1,
        .primary_ref_frame = frame_header->primary_ref_frame,
        .order_hint = frame_header->order_hint,
        .seg_info.segment_info_fields.bits = {
            .enabled = frame_header->segmentation_enabled,
            .update_map = frame_header->segmentation_update_map,
            .temporal_update = frame_header->segmentation_temporal_update,
            .update_data = frame_header->segmentation_update_data,
        },
        .film_grain_info.film_grain_info_fields.bits = {
            .apply_grain = frame_header->apply_grain,
            .chroma_scaling_from_luma = frame_header->chroma_scaling_from_luma,
            .grain_scaling_minus_8 = frame_header->grain_scaling_minus_8,
            .ar_coeff_lag = frame_header->ar_coeff_lag,
            .ar_coeff_shift_minus_6 = frame_header->ar_coeff_shift_minus_6,
            .grain_scale_shift = frame_header->grain_scale_shift,
            .overlap_flag = frame_header->overlap_flag,
            .clip_to_restricted_range = frame_header->clip_to_restricted_range,
        },
        .tile_cols = frame_header->tile_cols,
        .tile_rows = frame_header->tile_rows,
        .context_update_tile_id = frame_header->context_update_tile_id,
        .pic_info_fields.bits = {
            .frame_type = frame_header->frame_type,
            .show_frame = frame_header->show_frame,
            .showable_frame = frame_header->showable_frame,
            .error_resilient_mode = frame_header->error_resilient_mode,
            .disable_cdf_update = frame_header->disable_cdf_update,
            .allow_screen_content_tools = frame_header->allow_screen_content_tools,
            .force_integer_mv = frame_header->force_integer_mv,
            .allow_intrabc = frame_header->allow_intrabc,
            .use_superres = frame_header->use_superres,
            .allow_high_precision_mv = frame_header->allow_high_precision_mv,
            .is_motion_mode_switchable = frame_header->is_motion_mode_switchable,
            .use_ref_frame_mvs = frame_header->use_ref_frame_mvs,
            .disable_frame_end_update_cdf = frame_header->disable_frame_end_update_cdf,
            .uniform_tile_spacing_flag = frame_header->uniform_tile_spacing_flag,
            .allow_warped_motion = frame_header->allow_warped_motion,
        },
        .interp_filter = frame_header->interpolation_filter,
        .filter_level[0] = frame_header->loop_filter_level[0],
        .filter_level[1] = frame_header->loop_filter_level[1],
        .filter_level_u = frame_header->loop_filter_level[2],
        .filter_level_v = frame_header->loop_filter_level[3],
        .loop_filter_info_fields.bits = {
            .sharpness_level = frame_header->loop_filter_sharpness,
            .mode_ref_delta_enabled = s->cur_frame.loop_filter_delta_enabled,
            .mode_ref_delta_update = frame_header->loop_filter_delta_update,
        },
        .base_qindex = frame_header->base_q_idx,
        .qmatrix_fields.bits.using_qmatrix = frame_header->using_qmatrix,
        .mode_control_fields.bits = {
            .delta_q_present_flag = frame_header->delta_q_present,
            .log2_delta_q_res = frame_header->delta_q_res,
            .tx_mode = frame_header->tx_mode,
            .reference_select = frame_header->reference_select,
            .reduced_tx_set_used = frame_header->reduced_tx_set,
            .skip_mode_present = frame_header->skip_mode_present,
        },
        .cdef_damping_minus_3 = frame_header->cdef_damping_minus_3,
        .cdef_bits = frame_header->cdef_bits,
        .loop_restoration_fields.bits = {
            .yframe_restoration_type = lr_type[frame_header->lr_type[0]],
            .cbframe_restoration_type = lr_type[frame_header->lr_type[1]],
            .crframe_restoration_type = lr_type[frame_header->lr_type[2]],
            .lr_unit_shift = frame_header->lr_unit_shift,
            .lr_uv_shift = frame_header->lr_uv_shift,
        },
    };

    for (int i = 0; i < 8; i++) {
        if (pic_param.pic_info_fields.bits.frame_type == AV1_FRAME_KEY)
            pic_param.ref_frame_map[i] = VA_INVALID_ID;
        else
            pic_param.ref_frame_map[i] = vaapi_av1_surface_id(&s->ref[i]);
        av_log(avctx, AV_LOG_DEBUG, "Vaapi set ref surface id: %d - 0x:%x\n",
               i, pic_param.ref_frame_map[i]);
    }
    for (int i = 0; i < 7; i++) {
        pic_param.ref_frame_idx[i] = frame_header->ref_frame_idx[i];
    }
    for (int i = 0; i < 8; i++) {
        pic_param.ref_deltas[i] = s->cur_frame.loop_filter_ref_deltas[i];
    }
    for (int i = 0; i < 2; i++) {
        pic_param.mode_deltas[i] = s->cur_frame.loop_filter_mode_deltas[i];
    }
    for (int i = 0; i < 8; i++) {
        pic_param.cdef_y_strengths[i] =
            (frame_header->cdef_y_pri_strength[i] << 2) +
                frame_header->cdef_y_sec_strength[i];
        pic_param.cdef_uv_strengths[i] =
            (frame_header->cdef_uv_pri_strength[i] << 2) +
                frame_header->cdef_uv_sec_strength[i];
    }
    for (int i = 0; i < frame_header->tile_cols; i++) {
        pic_param.width_in_sbs_minus_1[i] =
            frame_header->width_in_sbs_minus_1[i];
    }
    for (int i = 0; i < frame_header->tile_rows; i++) {
        pic_param.height_in_sbs_minus_1[i] =
            frame_header->height_in_sbs_minus_1[i];
    }
    for (int i = AV1_REF_FRAME_LAST; i <= AV1_REF_FRAME_ALTREF; i++) {
        pic_param.wm[i - 1].wmtype =
            vaapi_av1_get_gm_type(s->cur_frame.gm_type[i]);
        for (int j=0; j<6; j++)
            pic_param.wm[i - 1].wmmat[j] = s->cur_frame.gm_params[i][j];
    }
    err = ff_vaapi_decode_make_param_buffer(avctx, pic,
                                            VAPictureParameterBufferType,
                                            &pic_param, sizeof(pic_param));
    if (err < 0)
        goto fail;

    return 0;

fail:
    ff_vaapi_decode_cancel(avctx, pic);
    return err;
}

static int vaapi_av1_end_frame(AVCodecContext *avctx)
{
    const AV1DecContext *s = avctx->priv_data;
    VAAPIDecodePicture *pic = s->cur_frame.hwaccel_picture_private;
    return ff_vaapi_decode_issue(avctx, pic);
}

static int vaapi_av1_decode_slice(AVCodecContext *avctx,
                                  const uint8_t *buffer,
                                  uint32_t size)
{
    const AV1DecContext *s = avctx->priv_data;
    VAAPIDecodePicture *pic = s->cur_frame.hwaccel_picture_private;
    VASliceParameterBufferAV1 slice_param;
    uint32_t offset;
    int err = 0;

    for (int i = s->tg_start; i <= s->tg_end; i++) {
        memset(&slice_param, 0, sizeof(VASliceParameterBufferAV1));
        offset = s->tile_group_info[i].tile_offset;

        slice_param = (VASliceParameterBufferAV1) {
            .slice_data_size = s->tile_group_info[i].tile_size,
            .slice_data_offset = 0,
            .slice_data_flag = VA_SLICE_DATA_FLAG_ALL,
            .tile_row = s->tile_group_info[i].tile_row,
            .tile_column = s->tile_group_info[i].tile_column,
            .tg_start = s->tg_start,
            .tg_end = s->tg_end,
        };

        err = ff_vaapi_decode_make_slice_buffer(avctx, pic, &slice_param,
                                                sizeof(VASliceParameterBufferAV1),
                                                buffer + offset,
                                                s->tile_group_info[i].tile_size);
        if (err) {
            ff_vaapi_decode_cancel(avctx, pic);
            return err;
        }
    }

    return 0;
}

const AVHWAccel ff_av1_vaapi_hwaccel = {
    .name                 = "av1_vaapi",
    .type                 = AVMEDIA_TYPE_VIDEO,
    .id                   = AV_CODEC_ID_AV1,
    .pix_fmt              = AV_PIX_FMT_VAAPI,
    .start_frame          = vaapi_av1_start_frame,
    .end_frame            = vaapi_av1_end_frame,
    .decode_slice         = vaapi_av1_decode_slice,
    .frame_priv_data_size = sizeof(VAAPIDecodePicture),
    .init                 = ff_vaapi_decode_init,
    .uninit               = ff_vaapi_decode_uninit,
    .frame_params         = ff_vaapi_common_frame_params,
    .priv_data_size       = sizeof(VAAPIDecodeContext),
    .caps_internal        = HWACCEL_CAP_ASYNC_SAFE,
};

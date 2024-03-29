/*
 * Direct3D 12 HW acceleration video encoder
 *
 * Copyright (c) 2024 Intel Corporation
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

#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext_d3d12va_internal.h"

#include "avcodec.h"
#include "cbs_av1.h"
#include "codec_internal.h"
#include "av1_levels.h"
#include "d3d12va_encode.h"

#define AV1_MAX_QUANT 255

typedef struct D3D12VAEncodeAV1Picture {
    int64_t last_idr_frame;
    int slot;
} D3D12VAEncodeAV1Picture;

typedef struct D3D12VAEncodeAV1Context {
    D3D12VAEncodeContext common;

    AV1RawOBU seq_header;

    int q_idx_idr;
    int q_idx_p;
    int q_idx_b;

    // User options.
    int profile;
    int level;
    int tier;
    int tile_cols, tile_rows;
    int tile_groups;

    D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION_SUPPORT caps;

    CodedBitstreamContext *cbc;
    CodedBitstreamFragment current_obu;

    char sh_data[MAX_PARAM_BUFFER_SIZE]; /**< coded sequence header data. */
    size_t sh_data_len; /**< bit length of sh_data. */
    char fh_data[MAX_PARAM_BUFFER_SIZE]; /**< coded frame header data. */
    size_t fh_data_len; /**< bit length of fh_data. */
} D3D12VAEncodeAV1Context;

static const D3D12_VIDEO_ENCODER_AV1_PROFILE profile_main = D3D12_VIDEO_ENCODER_AV1_PROFILE_MAIN;

#define D3D_PROFILE_DESC(name) \
    { sizeof(D3D12_VIDEO_ENCODER_AV1_PROFILE), { .pAV1Profile = (D3D12_VIDEO_ENCODER_AV1_PROFILE *)&profile_ ## name } }
static const D3D12VAEncodeProfile d3d12va_encode_av1_profiles[] = {
    { AV_PROFILE_AV1_MAIN, 8, 3, 1, 1, D3D_PROFILE_DESC(main) },
    { AV_PROFILE_UNKNOWN },
};

static int d3d12va_encode_av1_add_obu(AVCodecContext *avctx,
                                      CodedBitstreamFragment *au,
                                      uint8_t type,
                                      void *obu_unit)
{
    int ret;

    ret = ff_cbs_insert_unit_content(au, -1,
                                     type, obu_unit, NULL);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to add OBU unit: "
               "type = %d.\n", type);
        return ret;
    }

    return 0;
}

static int d3d12va_encode_av1_write_obu(AVCodecContext *avctx,
                                        char *data, size_t *data_len,
                                        CodedBitstreamFragment *bs)
{
    D3D12VAEncodeAV1Context *priv = avctx->priv_data;
    int ret;

    ret = ff_cbs_write_fragment_data(priv->cbc, bs);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to write packed header.\n");
        return ret;
    }

    if ((size_t)8 * MAX_PARAM_BUFFER_SIZE < 8 * bs->data_size - bs->data_bit_padding) {
        av_log(avctx, AV_LOG_ERROR, "Access unit too large: "
               "%zu < %zu.\n", (size_t)8 * MAX_PARAM_BUFFER_SIZE,
               8 * bs->data_size - bs->data_bit_padding);
        return AVERROR(ENOSPC);
    }

    memcpy(data, bs->data, bs->data_size);
    *data_len = 8 * bs->data_size - bs->data_bit_padding;

    return 0;
}

static int d3d12va_encode_av1_init_sequence_params(AVCodecContext *avctx)
{
    HWBaseEncodeContext *base_ctx = avctx->priv_data;
    D3D12VAEncodeContext     *ctx = avctx->priv_data;
    D3D12VAEncodeAV1Context *priv = avctx->priv_data;
    AV1RawOBU             *sh_obu = &priv->seq_header;
    AV1RawSequenceHeader      *sh = &sh_obu->obu.sequence_header;
    CodedBitstreamFragment   *obu = &priv->current_obu;
    const AVPixFmtDescriptor *desc;
    int err;

    memset(sh_obu, 0, sizeof(*sh_obu));
    sh_obu->header.obu_type = AV1_OBU_SEQUENCE_HEADER;

    desc = av_pix_fmt_desc_get(base_ctx->input_frames->sw_format);
    av_assert0(desc);

    sh->seq_profile = avctx->profile;
    sh->frame_width_bits_minus_1  = av_log2(avctx->width);
    sh->frame_height_bits_minus_1 = av_log2(avctx->height);
    sh->max_frame_width_minus_1   = avctx->width - 1;
    sh->max_frame_height_minus_1  = avctx->height - 1;
    sh->seq_tier[0]               = priv->tier;

    sh->color_config = (AV1RawColorConfig) {
        .high_bitdepth                  = desc->comp[0].depth == 8 ? 0 : 1,
        .color_primaries                = avctx->color_primaries,
        .transfer_characteristics       = avctx->color_trc,
        .matrix_coefficients            = avctx->colorspace,
        .color_description_present_flag = (avctx->color_primaries != AVCOL_PRI_UNSPECIFIED ||
                                           avctx->color_trc       != AVCOL_TRC_UNSPECIFIED ||
                                           avctx->colorspace      != AVCOL_SPC_UNSPECIFIED),
        .color_range                    = avctx->color_range == AVCOL_RANGE_JPEG,
        .subsampling_x                  = desc->log2_chroma_w,
        .subsampling_y                  = desc->log2_chroma_h,
        .separate_uv_delta_q            = 1,
    };

    switch (avctx->chroma_sample_location) {
        case AVCHROMA_LOC_LEFT:
            sh->color_config.chroma_sample_position = AV1_CSP_VERTICAL;
            break;
        case AVCHROMA_LOC_TOPLEFT:
            sh->color_config.chroma_sample_position = AV1_CSP_COLOCATED;
            break;
        default:
            sh->color_config.chroma_sample_position = AV1_CSP_UNKNOWN;
            break;
    }

    if (avctx->level != AV_LEVEL_UNKNOWN) {
        sh->seq_level_idx[0] = avctx->level;
    } else {
        const AV1LevelDescriptor *level;
        float framerate;

        if (avctx->framerate.num > 0 && avctx->framerate.den > 0)
            framerate = avctx->framerate.num / avctx->framerate.den;
        else
            framerate = 0;

        level = ff_av1_guess_level(avctx->bit_rate, priv->tier,
                                   base_ctx->surface_width, base_ctx->surface_height,
                                   priv->tile_rows * priv->tile_cols,
                                   priv->tile_cols, framerate);
        if (level) {
            av_log(avctx, AV_LOG_VERBOSE, "Using level %s.\n", level->name);
            sh->seq_level_idx[0] = level->level_idx;
        } else {
            av_log(avctx, AV_LOG_VERBOSE, "Stream will not conform to "
                   "any normal level, using maximum parameters level by default.\n");
            sh->seq_level_idx[0] = 31;
            sh->seq_tier[0] = 1;
        }

        avctx->level = sh->seq_level_idx[0];
    }

    sh->use_128x128_superblock     = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_128x128_SUPERBLOCK);
    sh->enable_filter_intra        = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FILTER_INTRA);
    sh->enable_intra_edge_filter   = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_EDGE_FILTER);
    sh->enable_interintra_compound = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTERINTRA_COMPOUND);
    sh->enable_masked_compound     = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_MASKED_COMPOUND);
    sh->enable_warped_motion       = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_WARPED_MOTION);
    sh->enable_dual_filter         = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_DUAL_FILTER);
    sh->enable_order_hint          = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ORDER_HINT_TOOLS);
    sh->enable_jnt_comp            = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_JNT_COMP);
    sh->enable_ref_frame_mvs       = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FRAME_REFERENCE_MOTION_VECTORS);
    sh->order_hint_bits_minus_1    = ctx->codec_conf.pAV1Config->OrderHintBitsMinus1;
    sh->enable_superres            = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_SUPER_RESOLUTION);
    sh->enable_cdef                = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CDEF_FILTERING);
    sh->enable_restoration         = !!(ctx->codec_conf.pAV1Config->FeatureFlags &
                                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_LOOP_RESTORATION_FILTER);

    err = d3d12va_encode_av1_add_obu(avctx, obu, AV1_OBU_SEQUENCE_HEADER, &priv->seq_header);
    if (err < 0)
        goto end;

    err = d3d12va_encode_av1_write_obu(avctx, priv->sh_data, &priv->sh_data_len, obu);
    if (err < 0)
        goto end;

end:
    ff_cbs_fragment_reset(obu);
    return err;
}

static int d3d12va_encode_av1_get_encoder_caps(AVCodecContext *avctx)
{
    HWBaseEncodeContext *base_ctx = avctx->priv_data;
    D3D12VAEncodeContext     *ctx = avctx->priv_data;
    D3D12VAEncodeAV1Context *priv = avctx->priv_data;
    D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION *config;
    HRESULT hr;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT codec_caps = {
        .NodeIndex                      = 0,
        .Codec                          = D3D12_VIDEO_ENCODER_CODEC_AV1,
        .Profile                        = ctx->profile->d3d12_profile,
        .CodecSupportLimits.DataSize    = sizeof(D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION_SUPPORT),
        .CodecSupportLimits.pAV1Support = &priv->caps,
    };

    hr = ID3D12VideoDevice3_CheckFeatureSupport(ctx->video_device3, D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT,
                                                &codec_caps, sizeof(codec_caps));
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to check encoder codec configuration support.\n");
        return AVERROR(EINVAL);
    }

    if (!codec_caps.IsSupported) {
        av_log(avctx, AV_LOG_ERROR, "AV1 configuration is not supported on this device.\n");
        return AVERROR(EINVAL);
    }

    ctx->codec_conf.DataSize = sizeof(D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION);
    ctx->codec_conf.pAV1Config = av_mallocz(ctx->codec_conf.DataSize);
    if (!ctx->codec_conf.pAV1Config)
        return AVERROR(ENOMEM);

    config = ctx->codec_conf.pAV1Config;

    /** enable order hint and reserve maximum 8 bits for it by default. */
    config->OrderHintBitsMinus1 = 7;
    config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ORDER_HINT_TOOLS;

    if (ctx->rc.Mode != D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP)
        config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CDEF_FILTERING;

    if (priv->caps.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FORCED_INTEGER_MOTION_VECTORS)
        config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FORCED_INTEGER_MOTION_VECTORS;

    if (priv->caps.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_PALETTE_ENCODING)
        config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_PALETTE_ENCODING;

    if (priv->caps.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_BLOCK_COPY)
        config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_BLOCK_COPY;

    if (priv->caps.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_LOOP_FILTER_DELTAS)
        config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_LOOP_FILTER_DELTAS;

    if (priv->caps.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_QUANTIZATION_MATRIX)
        config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_QUANTIZATION_MATRIX;

    if (priv->caps.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_REDUCED_TX_SET)
        config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_REDUCED_TX_SET;

    if (priv->caps.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_MOTION_MODE_SWITCHABLE)
        config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_MOTION_MODE_SWITCHABLE;

    if (priv->caps.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ALLOW_HIGH_PRECISION_MV)
        config->FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ALLOW_HIGH_PRECISION_MV;

    config->FeatureFlags |= priv->caps.RequiredFeatureFlags;

    base_ctx->surface_width  = FFALIGN(avctx->width,  64);
    base_ctx->surface_height = FFALIGN(avctx->height, 64);

    return 0;
}

static int d3d12va_encode_av1_configure(AVCodecContext *avctx)
{
    HWBaseEncodeContext  *base_ctx = avctx->priv_data;
    D3D12VAEncodeContext      *ctx = avctx->priv_data;
    D3D12VAEncodeAV1Context  *priv = avctx->priv_data;
    int err;

    err = ff_cbs_init(&priv->cbc, AV_CODEC_ID_AV1, avctx);
    if (err < 0)
        return err;

    // Rate control
    if (ctx->rc.Mode == D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP) {
        D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP *cqp_ctl;
        priv->q_idx_p = av_clip(ctx->rc_quality, 0, AV1_MAX_QUANT);
        if (fabs(avctx->i_quant_factor) > 0.0)
            priv->q_idx_idr =
                av_clip((fabs(avctx->i_quant_factor) * priv->q_idx_p  +
                         avctx->i_quant_offset) + 0.5,
                        0, AV1_MAX_QUANT);
        else
            priv->q_idx_idr = priv->q_idx_p;

        if (fabs(avctx->b_quant_factor) > 0.0)
            priv->q_idx_b =
                av_clip((fabs(avctx->b_quant_factor) * priv->q_idx_p  +
                         avctx->b_quant_offset) + 0.5,
                        0, AV1_MAX_QUANT);
        else
            priv->q_idx_b = priv->q_idx_p;

        ctx->rc.ConfigParams.DataSize = sizeof(D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP);
        cqp_ctl = av_mallocz(ctx->rc.ConfigParams.DataSize);
        if (!cqp_ctl)
            return AVERROR(ENOMEM);

        cqp_ctl->ConstantQP_FullIntracodedFrame                  = priv->q_idx_idr;
        cqp_ctl->ConstantQP_InterPredictedFrame_PrevRefOnly      = priv->q_idx_p;
        cqp_ctl->ConstantQP_InterPredictedFrame_BiDirectionalRef = priv->q_idx_b;

        ctx->rc.ConfigParams.pConfiguration_CQP = cqp_ctl;
    } else {
        /** Arbitrary value */
        priv->q_idx_idr = priv->q_idx_p = priv->q_idx_b = 128;
    }

    // GOP
    ctx->gop.DataSize = sizeof(D3D12_VIDEO_ENCODER_AV1_SEQUENCE_STRUCTURE);
    ctx->gop.pAV1SequenceStructure = av_mallocz(ctx->gop.DataSize);
    if (!ctx->gop.pAV1SequenceStructure)
        return AVERROR(ENOMEM);

    ctx->gop.pAV1SequenceStructure->IntraDistance    = base_ctx->gop_size;
    ctx->gop.pAV1SequenceStructure->InterFramePeriod = base_ctx->b_per_p + 1;

    return 0;
}

static int d3d12va_encode_av1_set_level(AVCodecContext *avctx)
{
    D3D12VAEncodeContext     *ctx = avctx->priv_data;
    D3D12VAEncodeAV1Context *priv = avctx->priv_data;
    AV1RawOBU             *sh_obu = &priv->seq_header;
    AV1RawSequenceHeader      *sh = &sh_obu->obu.sequence_header;

    ctx->level.DataSize = sizeof(D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS);
    ctx->level.pAV1LevelSetting = av_mallocz(ctx->level.DataSize);
    if (!ctx->level.pAV1LevelSetting)
        return AVERROR(ENOMEM);

    ctx->level.pAV1LevelSetting->Level = (D3D12_VIDEO_ENCODER_AV1_LEVELS)avctx->level;
    ctx->level.pAV1LevelSetting->Tier  = (D3D12_VIDEO_ENCODER_AV1_TIER)sh->seq_tier[0];

    return 0;
}

static int d3d12va_encode_av1_init_picture_params(AVCodecContext *avctx,
                                                  D3D12VAEncodePicture *pic)
{
    D3D12VAEncodeContext     *ctx = avctx->priv_data;
    D3D12VAEncodeAV1Context *priv = avctx->priv_data;
    HWBaseEncodePicture *base_pic = (HWBaseEncodePicture *)pic;
    D3D12VAEncodeAV1Picture *hpic = base_pic->priv_data;
    D3D12_VIDEO_ENCODER_CODEC_AV1_PICTURE_CONTROL_SUPPORT av1_pic_support;
    HWBaseEncodePicture *ref;
    D3D12VAEncodeAV1Picture *href;
    HRESULT hr;
    uint8_t i, base_q_index;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT pic_support = {
        .NodeIndex = 0,
        .Codec = D3D12_VIDEO_ENCODER_CODEC_AV1,
        .Profile = ctx->profile->d3d12_profile,
        .PictureSupport.DataSize = sizeof(av1_pic_support),
        .PictureSupport.pAV1Support = &av1_pic_support,
    };

    hr = ID3D12VideoDevice3_CheckFeatureSupport(ctx->video_device3, D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT,
                                                &pic_support, sizeof(pic_support));
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to check encoder codec picture control support.\n");
        return AVERROR(EINVAL);
    }

    pic->pic_ctl.DataSize = sizeof(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA);
    pic->pic_ctl.pAV1PicData = av_mallocz(pic->pic_ctl.DataSize);
    if (!pic->pic_ctl.pAV1PicData)
        return AVERROR(ENOMEM);

    switch (base_pic->type)
    {
    case PICTURE_TYPE_IDR:
        pic->pic_ctl.pAV1PicData->FrameType = D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_KEY_FRAME;
        pic->pic_ctl.pAV1PicData->RefreshFrameFlags = 0xFF;
        base_q_index = priv->q_idx_idr;
        hpic->slot = 0;
        hpic->last_idr_frame = base_pic->display_order;
        pic->pic_ctl.pAV1PicData->PrimaryRefFrame = 7; // PRIMARY_REF_NONE
        break;
    case PICTURE_TYPE_P:
        av_assert0(base_pic->nb_refs[0]);
        pic->pic_ctl.pAV1PicData->FrameType = D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTER_FRAME;
        base_q_index = priv->q_idx_p;
        ref = base_pic->refs[0][base_pic->nb_refs[0] - 1];
        href = ref->priv_data;
        hpic->slot = !href->slot;
        hpic->last_idr_frame = href->last_idr_frame;
        pic->pic_ctl.pAV1PicData->RefreshFrameFlags = 1 << hpic->slot;

        /** set the nearest frame in L0 as all reference frame. */
        for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
            pic->pic_ctl.pAV1PicData->ReferenceIndices[i] = href->slot;
        }
        pic->pic_ctl.pAV1PicData->PrimaryRefFrame = href->slot;

        /** set the 2nd nearest frame in L0 as Golden frame. */
        if (base_pic->nb_refs[0] > 1) {
            ref = base_pic->refs[0][base_pic->nb_refs[0] - 2];
            href = ref->priv_data;
            pic->pic_ctl.pAV1PicData->ReferenceIndices[3] = href->slot;
        }
        break;
    case PICTURE_TYPE_B:
        av_assert0(base_pic->nb_refs[0] && base_pic->nb_refs[1]);
        pic->pic_ctl.pAV1PicData->FrameType = D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTER_FRAME;
        base_q_index = priv->q_idx_b;
        pic->pic_ctl.pAV1PicData->RefreshFrameFlags = 0x0;
        ref = base_pic->refs[0][base_pic->nb_refs[0] - 1];
        href = ref->priv_data;
        hpic->last_idr_frame = href->last_idr_frame;
        pic->pic_ctl.pAV1PicData->PrimaryRefFrame = href->slot;

        for (i = 0; i < AV1_REF_FRAME_GOLDEN; i++) {
            pic->pic_ctl.pAV1PicData->ReferenceIndices[i] = href->slot;
        }

        ref = base_pic->refs[1][base_pic->nb_refs[1] - 1];
        href = ref->priv_data;
        for (i = AV1_REF_FRAME_GOLDEN; i < AV1_REFS_PER_FRAME; i++) {
            pic->pic_ctl.pAV1PicData->ReferenceIndices[i] = href->slot;
        }
        break;

    default:
        av_assert0(0 && "invalid picture type");
    }

    if (!(av1_pic_support.SupportedFrameTypes & (1 << pic->pic_ctl.pAV1PicData->FrameType))) {
        av_log(avctx, AV_LOG_ERROR, "FrameType %d is not supported.\n", pic->pic_ctl.pAV1PicData->FrameType);
        return AVERROR(EINVAL);
    }

    pic->pic_ctl.pAV1PicData->Flags = D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_NONE;

    if (base_pic->type == PICTURE_TYPE_IDR && base_pic->display_order <= base_pic->encode_order)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_ERROR_RESILIENT_MODE;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_PALETTE_ENCODING)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_PALETTE_ENCODING;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_SKIP_MODE_PRESENT)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_SKIP_MODE;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FRAME_REFERENCE_MOTION_VECTORS)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_FRAME_REFERENCE_MOTION_VECTORS;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FORCED_INTEGER_MOTION_VECTORS)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_FORCE_INTEGER_MOTION_VECTORS;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_BLOCK_COPY)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ALLOW_INTRA_BLOCK_COPY;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_AUTO_SEGMENTATION)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_FRAME_SEGMENTATION_AUTO;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CUSTOM_SEGMENTATION)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_FRAME_SEGMENTATION_CUSTOM;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_WARPED_MOTION)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_WARPED_MOTION;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_REDUCED_TX_SET)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_REDUCED_TX_SET;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ALLOW_HIGH_PRECISION_MV)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ALLOW_HIGH_PRECISION_MV;

    if (priv->caps.RequiredFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_MOTION_MODE_SWITCHABLE)
        pic->pic_ctl.pAV1PicData->Flags |= D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_MOTION_MODE_SWITCHABLE;

    pic->pic_ctl.pAV1PicData->CompoundPredictionType = base_pic->type == PICTURE_TYPE_B ?
                                                       D3D12_VIDEO_ENCODER_AV1_COMP_PREDICTION_TYPE_COMPOUND_REFERENCE :
                                                       D3D12_VIDEO_ENCODER_AV1_COMP_PREDICTION_TYPE_SINGLE_REFERENCE;

    for (i = D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_EIGHTTAP; i <= D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_SWITCHABLE; i++) {
         if (priv->caps.SupportedFeatureFlags & (1 << i)) {
            pic->pic_ctl.pAV1PicData->InterpolationFilter = (D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS)i;
            break;
         }
    }

    for (i = D3D12_VIDEO_ENCODER_AV1_TX_MODE_SELECT; i >= D3D12_VIDEO_ENCODER_AV1_TX_MODE_LARGEST; i--) {
        if (priv->caps.SupportedTxModes[pic->pic_ctl.pAV1PicData->FrameType] & (1 << i)) {
            pic->pic_ctl.pAV1PicData->TxMode = (D3D12_VIDEO_ENCODER_AV1_TX_MODE)i;
            break;
        }
    }

    pic->pic_ctl.pAV1PicData->OrderHint = base_pic->display_order - hpic->last_idr_frame;
    pic->pic_ctl.pAV1PicData->PictureIndex = base_pic->display_order - hpic->last_idr_frame;
    pic->pic_ctl.pAV1PicData->TemporalLayerIndexPlus1 = 1;
    pic->pic_ctl.pAV1PicData->SpatialLayerIndexPlus1 = 1;
    pic->pic_ctl.pAV1PicData->Quantization.BaseQIndex = base_q_index;

    for (i = 0; i < 8; i++)
        pic->pic_ctl.pAV1PicData->ReferenceFramesReconPictureDescriptors[i].ReconstructedPictureResourceIndex = 255;

    return 0;
}

static int d3d12va_encode_av1_fill_picture_header(AVCodecContext *avctx, D3D12VAEncodePicture *pic,
                                                  D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES *pv)
{
    D3D12VAEncodeContext     *ctx = avctx->priv_data;
    D3D12VAEncodeAV1Context *priv = avctx->priv_data;
    HWBaseEncodePicture *base_pic = &pic->base;
    AV1RawOBU fh_obu = { 0 };
    AV1RawFrameHeader fh = fh_obu.obu.frame_header;
    uint32_t i, j;

    fh_obu.header.obu_type = AV1_OBU_FRAME_HEADER;
    fh_obu.header.obu_has_size_field = 1;

    fh.frame_type = (uint8_t)pic->pic_ctl.pAV1PicData->FrameType;
    fh.show_frame = base_pic->display_order <= base_pic->encode_order;
    fh.showable_frame = pic->pic_ctl.pAV1PicData->FrameType != D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_KEY_FRAME;

    if (pic->pic_ctl.pAV1PicData->FrameType == D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_KEY_FRAME && fh.show_frame) {
        fh.error_resilient_mode = 1;
    }

    fh.allow_screen_content_tools = pic->pic_ctl.pAV1PicData->Flags &
                                    D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_PALETTE_ENCODING;
    fh.force_integer_mv = pic->pic_ctl.pAV1PicData->Flags &
                          D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_FORCE_INTEGER_MOTION_VECTORS;

    fh.order_hint = pic->pic_ctl.pAV1PicData->OrderHint;
    fh.primary_ref_frame = pv->PrimaryRefFrame;
    fh.frame_width_minus_1 = avctx->width - 1;
    fh.frame_height_minus_1 = avctx->height = 1;
    fh.render_width_minus_1 = fh.frame_width_minus_1;
    fh.render_height_minus_1 = fh.frame_height_minus_1;

    fh.refresh_frame_flags = pic->pic_ctl.pAV1PicData->RefreshFrameFlags;
    fh.allow_intrabc = pic->pic_ctl.pAV1PicData->Flags &
                       D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ALLOW_INTRA_BLOCK_COPY;

    for (i = 0; i < FF_ARRAY_ELEMS(pic->pic_ctl.pAV1PicData->ReferenceFramesReconPictureDescriptors); i++) {
        fh.ref_order_hint[i] = pic->pic_ctl.pAV1PicData->ReferenceFramesReconPictureDescriptors[i].OrderHint;
    }

    fh.allow_high_precision_mv = pic->pic_ctl.pAV1PicData->Flags &
                                 D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ALLOW_HIGH_PRECISION_MV;
    fh.is_filter_switchable = pic->pic_ctl.pAV1PicData->InterpolationFilter ==
                              D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_SWITCHABLE ? 1 : 0;
    fh.interpolation_filter = pic->pic_ctl.pAV1PicData->InterpolationFilter;
    fh.is_motion_mode_switchable = pic->pic_ctl.pAV1PicData->Flags &
                                   D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_MOTION_MODE_SWITCHABLE;
    fh.use_ref_frame_mvs = pic->pic_ctl.pAV1PicData->Flags &
                           D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_FRAME_REFERENCE_MOTION_VECTORS;

    fh.disable_frame_end_update_cdf = pic->pic_ctl.pAV1PicData->Flags &
                                      D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_DISABLE_FRAME_END_UPDATE_CDF;

    fh.uniform_tile_spacing_flag = 1; // D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_GRID_PARTITION

    // TODO: add tile info

    // Quantization params
    fh.base_q_idx = pv->Quantization.BaseQIndex;
    fh.delta_q_y_dc = pv->Quantization.YDCDeltaQ;
    fh.diff_uv_delta = pv->Quantization.UDCDeltaQ != pv->Quantization.VDCDeltaQ ||
                       pv->Quantization.UACDeltaQ != pv->Quantization.VACDeltaQ;
    fh.delta_q_u_dc = pv->Quantization.UDCDeltaQ;
    fh.delta_q_u_ac = pv->Quantization.UACDeltaQ;
    fh.delta_q_v_dc = pv->Quantization.VDCDeltaQ;
    fh.delta_q_v_ac = pv->Quantization.VACDeltaQ;
    fh.using_qmatrix = pv->Quantization.UsingQMatrix;
    fh.qm_y = pv->Quantization.QMY;
    fh.qm_u = pv->Quantization.QMU;
    fh.qm_v = pv->Quantization.QMV;

    // Segmentation params
    if (pic->pic_ctl.pAV1PicData->Flags &
        D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_FRAME_SEGMENTATION_AUTO) {
        fh.segmentation_enabled = pv->SegmentationConfig.NumSegments != 0;
        fh.segmentation_update_map = pv->SegmentationConfig.UpdateMap;
        fh.segmentation_temporal_update = pv->SegmentationConfig.TemporalUpdate;
        fh.segmentation_update_data = pv->SegmentationConfig.UpdateData;

        for (i = 0; i < AV1_MAX_SEGMENTS; i++) {
            for (j = 0; j < AV1_SEG_LVL_MAX; j++) {
                fh.feature_enabled[i][j] = (UINT)(1 << j) &
                                           (UINT)(pv->SegmentationConfig.SegmentsData[i].EnabledFeatures) != 0;
                fh.feature_value[i][j] = pv->SegmentationConfig.SegmentsData[i].FeatureValue[j];
            }
        }
    }

    // delta_q_params and delta_lf_params
    fh.delta_q_present  = pv->QuantizationDelta.DeltaQPresent;
    fh.delta_q_res      = pv->QuantizationDelta.DeltaQRes;
    fh.delta_lf_present = pv->LoopFilterDelta.DeltaLFPresent;
    fh.delta_lf_res     = pv->LoopFilterDelta.DeltaLFRes;
    fh.delta_lf_multi   = pv->LoopFilterDelta.DeltaLFMulti;

    // LoopFilter
    fh.loop_filter_level[0]      = pv->LoopFilter.LoopFilterLevel[0];
    fh.loop_filter_level[1]      = pv->LoopFilter.LoopFilterLevel[1];
    fh.loop_filter_level[2]      = pv->LoopFilter.LoopFilterLevelU;
    fh.loop_filter_level[3]      = pv->LoopFilter.LoopFilterLevelV;
    fh.loop_filter_sharpness     = pv->LoopFilter.LoopFilterSharpnessLevel;
    fh.loop_filter_delta_enabled = pv->LoopFilter.LoopFilterDeltaEnabled;
    fh.loop_filter_delta_update  = pv->LoopFilter.UpdateModeDelta || pv->LoopFilter.UpdateRefDelta;
    if (fh.loop_filter_delta_update) {
        for (i = 0; i < AV1_TOTAL_REFS_PER_FRAME; i++) {
            fh.update_ref_delta[i] = pv->LoopFilter.UpdateRefDelta;
            if (fh.update_ref_delta[i])
                fh.loop_filter_ref_deltas[i] = pv->LoopFilter.RefDeltas[i];
        }
        for (i = 0; i < 2; i++) {
            fh.update_mode_delta[i] = pv->LoopFilter.UpdateModeDelta;
            if (fh.update_mode_delta[i])
                fh.loop_filter_mode_deltas[i] = pv->LoopFilter.ModeDeltas[i];
        }
    }

    // Cdef
    fh.cdef_damping_minus_3 = pv->CDEF.CdefDampingMinus3;
    fh.cdef_bits = pv->CDEF.CdefBits;
    for (i = 0; i < (1 << fh.cdef_bits); i++) {
        fh.cdef_y_pri_strength[i] = pv->CDEF.CdefYPriStrength[i];
        fh.cdef_y_sec_strength[i] = pv->CDEF.CdefYSecStrength[i];
        fh.cdef_uv_pri_strength[i] = pv->CDEF.CdefUVPriStrength[i];
        fh.cdef_uv_sec_strength[i] = pv->CDEF.CdefUVSecStrength[i];
    }

    fh.tx_mode = (uint8_t)pic->pic_ctl.pAV1PicData->TxMode;
    fh.reference_select    = pv->CompoundPredictionType == D3D12_VIDEO_ENCODER_AV1_COMP_PREDICTION_TYPE_SINGLE_REFERENCE ?
                          0 : 1;
    fh.skip_mode_present   = pic->pic_ctl.pAV1PicData->Flags &
                             D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_SKIP_MODE;

    fh.allow_warped_motion = pic->pic_ctl.pAV1PicData->Flags &
                             D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_WARPED_MOTION;
    fh.reduced_tx_set      = pic->pic_ctl.pAV1PicData->Flags &
                             D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_REDUCED_TX_SET;

    return 0;
}

static int d3d12va_encode_av1_get_coded_data(AVCodecContext *avctx,
                                             D3D12VAEncodePicture *pic, AVPacket *pkt)
{
    D3D12_VIDEO_ENCODER_OUTPUT_METADATA *meta;
    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA *subregion;
    D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES *tiles;
    D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES *post_values;
    uint8_t *data;
    HRESULT hr;
    int err;

    hr = ID3D12Resource_Map(pic->resolved_metadata, 0, NULL, (void **)&data);
    if (FAILED(hr)) {
        err = AVERROR_UNKNOWN;
        return err;
    }

    meta  = (D3D12_VIDEO_ENCODER_OUTPUT_METADATA *)data;
    data += sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA);

    subregion = (D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA *)data;
    data     += sizeof(D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA) * meta->WrittenSubregionsCount;

    tiles = (D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES *)data;
    data += sizeof(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES);

    post_values = (D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES *)data;

    if (meta->EncodeErrorFlags != D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_NO_ERROR) {
        av_log(avctx, AV_LOG_ERROR, "Encode failed %"PRIu64"\n", meta->EncodeErrorFlags);
        err = AVERROR(EINVAL);
        return err;
    }

    if (meta->EncodedBitstreamWrittenBytesCount == 0) {
        av_log(avctx, AV_LOG_ERROR, "No bytes were written to encoded bitstream\n");
        err = AVERROR(EINVAL);
        return err;
    } else
        av_log(avctx, AV_LOG_DEBUG, "%"PRIu64" bytes were written\n");

    err = d3d12va_encode_av1_fill_picture_header(avctx, pic, post_values);
    if (err < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to fill picture "
               "header: %d.\n", err);
        goto fail;
    }

fail:
    return 0;
}

static const D3D12VAEncodeType d3d12va_encode_type_av1 = {
    .profiles               = d3d12va_encode_av1_profiles,

    .d3d12_codec            = D3D12_VIDEO_ENCODER_CODEC_AV1,

    .flags                  = FLAG_B_PICTURES |
                              FLAG_B_PICTURE_REFERENCES |
                              FLAG_NON_IDR_KEY_PICTURES,

    .default_quality        = 25,

    .get_encoder_caps       = &d3d12va_encode_av1_get_encoder_caps,

    .configure              = &d3d12va_encode_av1_configure,

    .set_level              = &d3d12va_encode_av1_set_level,

    .picture_priv_data_size = sizeof(D3D12VAEncodeAV1Picture),

    .init_sequence_params   = &d3d12va_encode_av1_init_sequence_params,

    .init_picture_params    = &d3d12va_encode_av1_init_picture_params,

    .get_coded_data         = &d3d12va_encode_av1_get_coded_data,
};

static int d3d12va_encode_av1_init(AVCodecContext *avctx)
{
    HWBaseEncodeContext *base_ctx = avctx->priv_data;
    D3D12VAEncodeContext     *ctx = avctx->priv_data;
    D3D12VAEncodeAV1Context *priv = avctx->priv_data;

    ctx->codec = &d3d12va_encode_type_av1;
    ctx->staging_buffer_needed = 1;

    if (avctx->profile == AV_PROFILE_UNKNOWN)
        avctx->profile = priv->profile;
    if (avctx->level == AV_LEVEL_UNKNOWN)
        avctx->level = priv->level;

    if (avctx->level != AV_LEVEL_UNKNOWN && avctx->level & ~0x1f) {
        av_log(avctx, AV_LOG_ERROR, "Invalid level %d\n", avctx->level);
        return AVERROR(EINVAL);
    }

    return ff_d3d12va_encode_init(avctx);
}

static int d3d12va_encode_av1_close(AVCodecContext *avctx)
{
    return 0;
}


#define OFFSET(x) offsetof(D3D12VAEncodeAV1Context, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM)
static const AVOption d3d12va_encode_av1_options[] = {
    HW_BASE_ENCODE_COMMON_OPTIONS,
    D3D12VA_ENCODE_RC_OPTIONS,
    { "profile", "Set profile (seq_profile)",
      OFFSET(profile), AV_OPT_TYPE_INT,
      { .i64 = AV_PROFILE_UNKNOWN }, AV_PROFILE_UNKNOWN, 0xff, FLAGS, .unit = "profile" },

#define PROFILE(name, value)  name, NULL, 0, AV_OPT_TYPE_CONST, \
    { .i64 = value }, 0, 0, FLAGS, .unit = "profile"
    { PROFILE("main",               AV_PROFILE_AV1_MAIN) },
#undef PROFILE

    { "tier", "Set tier (seq_tier)",
      OFFSET(tier), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS, .unit = "tier" },
    { "main", NULL, 0, AV_OPT_TYPE_CONST,
      { .i64 = 0 }, 0, 0, FLAGS, .unit = "tier" },
    { "high", NULL, 0, AV_OPT_TYPE_CONST,
      { .i64 = 1 }, 0, 0, FLAGS, .unit = "tier" },
    { "level", "Set level (seq_level_idx)",
      OFFSET(level), AV_OPT_TYPE_INT,
      { .i64 = AV_LEVEL_UNKNOWN }, AV_LEVEL_UNKNOWN, 0x1f, FLAGS, .unit = "level" },

#define LEVEL(name, value) name, NULL, 0, AV_OPT_TYPE_CONST, \
      { .i64 = value }, 0, 0, FLAGS, .unit = "level"
    { LEVEL("2.0",  0) },
    { LEVEL("2.1",  1) },
    { LEVEL("3.0",  4) },
    { LEVEL("3.1",  5) },
    { LEVEL("4.0",  8) },
    { LEVEL("4.1",  9) },
    { LEVEL("5.0", 12) },
    { LEVEL("5.1", 13) },
    { LEVEL("5.2", 14) },
    { LEVEL("5.3", 15) },
    { LEVEL("6.0", 16) },
    { LEVEL("6.1", 17) },
    { LEVEL("6.2", 18) },
    { LEVEL("6.3", 19) },
#undef LEVEL

    { "tiles", "Tile columns x rows (Use minimal tile column/row number automatically by default)",
      OFFSET(tile_cols), AV_OPT_TYPE_IMAGE_SIZE, { .str = NULL }, 0, 0, FLAGS },
    { "tile_groups", "Number of tile groups for encoding",
      OFFSET(tile_groups), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, AV1_MAX_TILE_ROWS * AV1_MAX_TILE_COLS, FLAGS },

    { NULL },
};

static const FFCodecDefault d3d12va_encode_av1_defaults[] = {
    { "b",              "0"   },
    { "bf",             "2"   },
    { "g",              "120" },
    { "qmin",           "-1"  },
    { "qmax",           "-1"  },
    { NULL },
};

static const AVClass d3d12va_encode_av1_class = {
    .class_name = "av1_d3d12va",
    .item_name  = av_default_item_name,
    .option     = d3d12va_encode_av1_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_av1_d3d12va_encoder = {
    .p.name         = "av1_d3d12va",
    CODEC_LONG_NAME("D3D12VA av1 encoder"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_AV1,
    .priv_data_size = sizeof(D3D12VAEncodeAV1Context),
    .init           = &d3d12va_encode_av1_init,
    FF_CODEC_RECEIVE_PACKET_CB(&ff_hw_base_encode_receive_packet),
    .close          = &d3d12va_encode_av1_close,
    .p.priv_class   = &d3d12va_encode_av1_class,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE |
                      AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
    .defaults       = d3d12va_encode_av1_defaults,
    .p.pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_D3D12,
        AV_PIX_FMT_NONE,
    },
    .hw_configs     = ff_d3d12va_encode_hw_configs,
    .p.wrapper_name = "d3d12va",
};

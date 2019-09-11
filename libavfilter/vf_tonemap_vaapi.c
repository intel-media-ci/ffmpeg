/*
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
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/mastering_display_metadata.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "vaapi_vpp.h"

// ITU-T H.265 Table E.3: Colour Primaries
#define COLOUR_PRIMARY_BT2020            9
#define COLOUR_PRIMARY_BT709             1
// ITU-T H.265 Table E.4 Transfer characteristics
#define TRANSFER_CHARACTERISTICS_BT709   1
#define TRANSFER_CHARACTERISTICS_ST2084  16

typedef enum {
    HDR_VAAPI_H2H,
    HDR_VAAPI_H2S,
} HDRType;

typedef struct HDRVAAPIContext {
    VAAPIVPPContext vpp_ctx; // must be the first field

    int hdr_type;

    char *master_display;
    char *content_light;

    VAHdrMetaDataHDR10  in_metadata;
    VAHdrMetaDataHDR10  out_metadata;

    AVFrameSideData    *src_display;
    AVFrameSideData    *src_light;
} HDRVAAPIContext;

static int tonemap_vaapi_save_metadata(AVFilterContext *avctx, AVFrame *input_frame)
{
    HDRVAAPIContext *ctx = avctx->priv;
    AVMasteringDisplayMetadata *hdr_meta;
    AVContentLightMetadata *light_meta;

    ctx->src_display = av_frame_get_side_data(input_frame,
                                              AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (ctx->src_display) {
        hdr_meta = (AVMasteringDisplayMetadata *)ctx->src_display->data;
        if (!hdr_meta) {
            av_log(avctx, AV_LOG_ERROR, "No mastering display data\n");
            return AVERROR(EINVAL);
        }

        if (hdr_meta->has_luminance) {
            const int luma_den = 10000;
            ctx->in_metadata.max_display_mastering_luminance =
                lrint(luma_den * av_q2d(hdr_meta->max_luminance));
            ctx->in_metadata.min_display_mastering_luminance =
                FFMIN(lrint(luma_den * av_q2d(hdr_meta->min_luminance)),
                      ctx->in_metadata.max_display_mastering_luminance);

            av_log(avctx, AV_LOG_DEBUG,
                   "Mastering Display Metadata(in luminance):\n");
            av_log(avctx, AV_LOG_DEBUG,
                   "min_luminance=%u, max_luminance=%u\n",
                   ctx->in_metadata.min_display_mastering_luminance,
                   ctx->in_metadata.max_display_mastering_luminance);
        }

        if (hdr_meta->has_primaries) {
            int i;
            const int mapping[3] = {1, 2, 0};  //green, blue, red
            const int chroma_den = 50000;

            for (i = 0; i < 3; i++) {
                const int j = mapping[i];
                ctx->in_metadata.display_primaries_x[i] =
                    FFMIN(lrint(chroma_den *
                                av_q2d(hdr_meta->display_primaries[j][0])),
                          chroma_den);
                ctx->in_metadata.display_primaries_y[i] =
                    FFMIN(lrint(chroma_den *
                                av_q2d(hdr_meta->display_primaries[j][1])),
                          chroma_den);
            }

            ctx->in_metadata.white_point_x =
                FFMIN(lrint(chroma_den * av_q2d(hdr_meta->white_point[0])),
                      chroma_den);
            ctx->in_metadata.white_point_y =
                FFMIN(lrint(chroma_den * av_q2d(hdr_meta->white_point[1])),
                      chroma_den);

            av_log(avctx, AV_LOG_DEBUG,
                   "Mastering Display Metadata(in primaries):\n");
            av_log(avctx, AV_LOG_DEBUG,
                   "G(%u,%u) B(%u,%u) R(%u,%u) WP(%u,%u)\n",
                   ctx->in_metadata.display_primaries_x[0],
                   ctx->in_metadata.display_primaries_y[0],
                   ctx->in_metadata.display_primaries_x[1],
                   ctx->in_metadata.display_primaries_y[1],
                   ctx->in_metadata.display_primaries_x[2],
                   ctx->in_metadata.display_primaries_y[2],
                   ctx->in_metadata.white_point_x,
                   ctx->in_metadata.white_point_y);
        }
    } else {
        av_log(avctx, AV_LOG_DEBUG, "No mastering display data from input\n");
    }

    ctx->src_light = av_frame_get_side_data(input_frame,
                                            AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (ctx->src_light) {
        light_meta = (AVContentLightMetadata *)ctx->src_light->data;
        if (!light_meta) {
            av_log(avctx, AV_LOG_ERROR, "No light meta data\n");
            return AVERROR(EINVAL);
        }

        ctx->in_metadata.max_content_light_level = light_meta->MaxCLL;
        ctx->in_metadata.max_pic_average_light_level = light_meta->MaxFALL;

        av_log(avctx, AV_LOG_DEBUG,
               "Mastering Content Light Level (in):\n");
        av_log(avctx, AV_LOG_DEBUG,
               "MaxCLL(%u) MaxFALL(%u)\n",
               ctx->in_metadata.max_content_light_level,
               ctx->in_metadata.max_pic_average_light_level);
    } else {
        av_log(avctx, AV_LOG_DEBUG, "No content light level from input\n");
    }

    return 0;
}

static int tonemap_vaapi_set_filter_params(AVFilterContext *avctx, AVFrame *input_frame)
{
    VAAPIVPPContext *vpp_ctx   = avctx->priv;
    HDRVAAPIContext *ctx       = avctx->priv;
    VAStatus vas;
    VAProcFilterParameterBufferHDRToneMapping *hdrtm_param;

    vas = vaMapBuffer(vpp_ctx->hwctx->display, vpp_ctx->filter_buffers[0],
                      (void**)&hdrtm_param);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to map "
               "buffer (%d): %d (%s).\n",
               vpp_ctx->filter_buffers[0], vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    memcpy(hdrtm_param->data.metadata, &ctx->in_metadata, sizeof(VAHdrMetaDataHDR10));

    vas = vaUnmapBuffer(vpp_ctx->hwctx->display, vpp_ctx->filter_buffers[0]);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to unmap output buffers: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    return 0;
}

static int tonemap_vaapi_build_filter_params(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx   = avctx->priv;
    HDRVAAPIContext *ctx       = avctx->priv;
    VAStatus vas;
    VAProcFilterCapHighDynamicRange hdr_cap;
    int num_query_caps;
    VAProcFilterParameterBufferHDRToneMapping hdrtm_param;

    vas = vaQueryVideoProcFilterCaps(vpp_ctx->hwctx->display,
                                     vpp_ctx->va_context,
                                     VAProcFilterHighDynamicRangeToneMapping,
                                     &hdr_cap, &num_query_caps);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to query HDR caps "
               "context: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    if (hdr_cap.metadata_type == VAProcHighDynamicRangeMetadataNone) {
        av_log(avctx, AV_LOG_ERROR, "VAAPI driver doesn't support HDR\n");
        return AVERROR(EINVAL);
    }

    switch (ctx->hdr_type) {
    case HDR_VAAPI_H2H:
        if (!(VA_TONE_MAPPING_HDR_TO_HDR & hdr_cap.caps_flag)) {
            av_log(avctx, AV_LOG_ERROR,
                   "VAAPI driver doesn't support H2H\n");
            return AVERROR(EINVAL);
        }
        break;
    case HDR_VAAPI_H2S:
        if (!(VA_TONE_MAPPING_HDR_TO_SDR & hdr_cap.caps_flag)) {
            av_log(avctx, AV_LOG_ERROR,
                   "VAAPI driver doesn't support H2S\n");
            return AVERROR(EINVAL);
        }
        break;
    default:
        av_assert0(0);
    }

    memset(&hdrtm_param, 0, sizeof(hdrtm_param));
    memset(&ctx->in_metadata, 0, sizeof(ctx->in_metadata));
    hdrtm_param.type = VAProcFilterHighDynamicRangeToneMapping;
    hdrtm_param.data.metadata_type = VAProcHighDynamicRangeMetadataHDR10;
    hdrtm_param.data.metadata      = &ctx->in_metadata;
    hdrtm_param.data.metadata_size = sizeof(VAHdrMetaDataHDR10);

    ff_vaapi_vpp_make_param_buffers(avctx,
                                    VAProcFilterParameterBufferType,
                                    &hdrtm_param, sizeof(hdrtm_param), 1);

    return 0;
}

static int tonemap_vaapi_update_sidedata(AVFilterContext *avctx, AVFrame *output_frame)
{
    HDRVAAPIContext *ctx = avctx->priv;
    AVFrameSideData *metadata;
    AVMasteringDisplayMetadata *hdr_meta;
    AVFrameSideData *metadata_lt;
    AVContentLightMetadata *hdr_meta_lt;

    metadata = av_frame_get_side_data(output_frame,
                                      AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (metadata) {
        int i;
        const int mapping[3] = {1, 2, 0};  //green, blue, red
        const int chroma_den = 50000;
        const int luma_den   = 10000;

        hdr_meta = (AVMasteringDisplayMetadata *)metadata->data;
        if (!hdr_meta) {
            av_log(avctx, AV_LOG_ERROR, "No mastering display data\n");
            return AVERROR(EINVAL);
        }

        for (i = 0; i < 3; i++) {
            const int j = mapping[i];
            hdr_meta->display_primaries[j][0].num = ctx->out_metadata.display_primaries_x[i];
            hdr_meta->display_primaries[j][0].den = chroma_den;

            hdr_meta->display_primaries[j][1].num = ctx->out_metadata.display_primaries_y[i];
            hdr_meta->display_primaries[j][1].den = chroma_den;
        }

        hdr_meta->white_point[0].num = ctx->out_metadata.white_point_x;
        hdr_meta->white_point[0].den = chroma_den;

        hdr_meta->white_point[0].num = ctx->out_metadata.white_point_y;
        hdr_meta->white_point[0].den = chroma_den;
        hdr_meta->has_primaries = 1;

        hdr_meta->max_luminance.num = ctx->out_metadata.max_display_mastering_luminance;
        hdr_meta->max_luminance.den = luma_den;

        hdr_meta->min_luminance.num = ctx->out_metadata.min_display_mastering_luminance;
        hdr_meta->min_luminance.den = luma_den;
        hdr_meta->has_luminance = 1;

        av_log(avctx, AV_LOG_DEBUG,
               "Mastering Display Metadata(out luminance):\n");
        av_log(avctx, AV_LOG_DEBUG,
               "min_luminance=%u, max_luminance=%u\n",
               ctx->out_metadata.min_display_mastering_luminance,
               ctx->out_metadata.max_display_mastering_luminance);

        av_log(avctx, AV_LOG_DEBUG,
               "Mastering Display Metadata(out primaries):\n");
        av_log(avctx, AV_LOG_DEBUG,
               "G(%u,%u) B(%u,%u) R(%u,%u) WP(%u,%u)\n",
               ctx->out_metadata.display_primaries_x[0],
               ctx->out_metadata.display_primaries_y[0],
               ctx->out_metadata.display_primaries_x[1],
               ctx->out_metadata.display_primaries_y[1],
               ctx->out_metadata.display_primaries_x[2],
               ctx->out_metadata.display_primaries_y[2],
               ctx->out_metadata.white_point_x,
               ctx->out_metadata.white_point_y);
    } else {
        av_log(avctx, AV_LOG_DEBUG, "No mastering display data for output\n");
    }

    metadata_lt = av_frame_get_side_data(output_frame,
                                         AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (metadata_lt) {
        hdr_meta_lt = (AVContentLightMetadata *)metadata_lt->data;
        if (!hdr_meta_lt) {
            av_log(avctx, AV_LOG_ERROR, "No light meta data\n");
            return AVERROR(EINVAL);
        }

        hdr_meta_lt->MaxCLL = FFMIN(ctx->out_metadata.max_content_light_level, 65535);
        hdr_meta_lt->MaxFALL = FFMIN(ctx->out_metadata.max_pic_average_light_level, 65535);

        av_log(avctx, AV_LOG_DEBUG,
               "Mastering Content Light Level (out):\n");
        av_log(avctx, AV_LOG_DEBUG,
               "MaxCLL(%u) MaxFALL(%u)\n",
               ctx->out_metadata.max_content_light_level,
               ctx->out_metadata.max_pic_average_light_level);

    } else {
        av_log(avctx, AV_LOG_DEBUG, "No content light level for output\n");
    }

    return 0;
}

static int tonemap_vaapi_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext *avctx     = inlink->dst;
    AVFilterLink *outlink      = avctx->outputs[0];
    VAAPIVPPContext *vpp_ctx   = avctx->priv;
    HDRVAAPIContext *ctx       = avctx->priv;
    AVFrame *output_frame      = NULL;
    VASurfaceID input_surface, output_surface;
    VARectangle input_region;

    VAProcPipelineParameterBuffer params;
    int err;

    VAHdrMetaData              out_hdr_metadata;

    av_log(avctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_frame->format),
           input_frame->width, input_frame->height, input_frame->pts);

    if (vpp_ctx->va_context == VA_INVALID_ID)
        return AVERROR(EINVAL);

    err = tonemap_vaapi_save_metadata(avctx, input_frame);
    if (err < 0)
        goto fail;

    err = tonemap_vaapi_set_filter_params(avctx, input_frame);
    if (err < 0)
        goto fail;

    input_surface = (VASurfaceID)(uintptr_t)input_frame->data[3];
    av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for tonemap vpp input.\n",
           input_surface);

    output_frame = ff_get_video_buffer(outlink, vpp_ctx->output_width,
                                       vpp_ctx->output_height);
    if (!output_frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    output_surface = (VASurfaceID)(uintptr_t)output_frame->data[3];
    av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for tonemap vpp output.\n",
           output_surface);
    memset(&params, 0, sizeof(params));
    input_region = (VARectangle) {
        .x      = 0,
        .y      = 0,
        .width  = input_frame->width,
        .height = input_frame->height,
    };

    params.filters     = &vpp_ctx->filter_buffers[0];
    params.num_filters = vpp_ctx->nb_filter_buffers;

    err = ff_vaapi_vpp_init_params(avctx, &params,
                                   input_frame, output_frame);
    if (err < 0)
        goto fail;

    switch (ctx->hdr_type)
    {
    case HDR_VAAPI_H2H:
        params.output_color_standard = VAProcColorStandardExplicit;
        params.output_color_properties.colour_primaries = COLOUR_PRIMARY_BT2020;
        params.output_color_properties.transfer_characteristics = TRANSFER_CHARACTERISTICS_ST2084;
        break;
    case HDR_VAAPI_H2S:
        params.output_color_standard = VAProcColorStandardBT709;
        params.output_color_properties.colour_primaries = COLOUR_PRIMARY_BT709;
        params.output_color_properties.transfer_characteristics = TRANSFER_CHARACTERISTICS_BT709;
        break;
    default:
        av_assert0(0);
    }

    if (ctx->hdr_type == HDR_VAAPI_H2H) {
        memset(&out_hdr_metadata, 0, sizeof(out_hdr_metadata));
        if (!ctx->master_display) {
            av_log(avctx, AV_LOG_ERROR,
                   "Option mastering-display input invalid\n");
            return AVERROR(EINVAL);
        }

        if (10 != sscanf(ctx->master_display,
                         "G(%hu|%hu)B(%hu|%hu)R(%hu|%hu)WP(%hu|%hu)L(%u|%u)",
                         &ctx->out_metadata.display_primaries_x[0],
                         &ctx->out_metadata.display_primaries_y[0],
                         &ctx->out_metadata.display_primaries_x[1],
                         &ctx->out_metadata.display_primaries_y[1],
                         &ctx->out_metadata.display_primaries_x[2],
                         &ctx->out_metadata.display_primaries_y[2],
                         &ctx->out_metadata.white_point_x,
                         &ctx->out_metadata.white_point_y,
                         &ctx->out_metadata.min_display_mastering_luminance,
                         &ctx->out_metadata.max_display_mastering_luminance)) {
            av_log(avctx, AV_LOG_ERROR,
                   "Option mastering-display input invalid\n");
            return AVERROR(EINVAL);
        }

        if (!ctx->content_light) {
            av_log(avctx, AV_LOG_ERROR,
                   "Option content-light input invalid\n");
            return AVERROR(EINVAL);
        }

        if (2 != sscanf(ctx->content_light,
                        "CLL(%hu)FALL(%hu)",
                        &ctx->out_metadata.max_content_light_level,
                        &ctx->out_metadata.max_pic_average_light_level)) {
            av_log(avctx, AV_LOG_ERROR,
                   "Option content-light input invalid\n");
            return AVERROR(EINVAL);
        }

        out_hdr_metadata.metadata_type = VAProcHighDynamicRangeMetadataHDR10;
        out_hdr_metadata.metadata      = &ctx->out_metadata;
        out_hdr_metadata.metadata_size = sizeof(VAHdrMetaDataHDR10);

        params.output_hdr_metadata = &out_hdr_metadata;
    }

    err = ff_vaapi_vpp_render_picture(avctx, &params, output_frame);
    if (err < 0)
        goto fail;

    err = av_frame_copy_props(output_frame, input_frame);
    if (err < 0)
        goto fail;

    if (ctx->hdr_type == HDR_VAAPI_H2H) {
        err = tonemap_vaapi_update_sidedata(avctx, output_frame);
        if (err < 0)
            goto fail;
    }

    av_frame_free(&input_frame);

    av_log(avctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output_frame->format),
           output_frame->width, output_frame->height, output_frame->pts);

    return ff_filter_frame(outlink, output_frame);

fail:
    av_frame_free(&input_frame);
    av_frame_free(&output_frame);
    return err;
}

static av_cold int tonemap_vaapi_init(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    HDRVAAPIContext *ctx     = avctx->priv;

    ff_vaapi_vpp_ctx_init(avctx);
    vpp_ctx->build_filter_params = tonemap_vaapi_build_filter_params;
    vpp_ctx->pipeline_uninit = ff_vaapi_vpp_pipeline_uninit;

    if (ctx->hdr_type == HDR_VAAPI_H2H) {
        vpp_ctx->output_format = AV_PIX_FMT_A2R10G10B10;
    } else if (ctx->hdr_type == HDR_VAAPI_H2S) {
        vpp_ctx->output_format = AV_PIX_FMT_ARGB;
    } else {
        av_assert0(0);
    }

    return 0;
}

static int tonemap_vaapi_vpp_query_formats(AVFilterContext *avctx)
{
    int err;

    enum AVPixelFormat pix_in_fmts[] = {
        AV_PIX_FMT_P010,     //Input
    };

    enum AVPixelFormat pix_out_fmts[] = {
        AV_PIX_FMT_A2R10G10B10,   //H2H RGB10
        AV_PIX_FMT_ARGB,          //H2S RGB8
    };

    err = ff_formats_ref(ff_make_format_list(pix_in_fmts),
                         &avctx->inputs[0]->out_formats);
    if (err < 0)
        return err;

    err = ff_formats_ref(ff_make_format_list(pix_out_fmts),
                         &avctx->outputs[0]->in_formats);
    if (err < 0)
        return err;

    return ff_vaapi_vpp_query_formats(avctx);
}

#define OFFSET(x) offsetof(HDRVAAPIContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
static const AVOption tonemap_vaapi_options[] = {
    { "type",    "hdr type",            OFFSET(hdr_type), AV_OPT_TYPE_INT, { .i64 = HDR_VAAPI_H2H }, 0, 2, FLAGS, "type" },
        { "h2h", "vaapi P010 to A2R10G10B10", 0, AV_OPT_TYPE_CONST, {.i64=HDR_VAAPI_H2H}, INT_MIN, INT_MAX, FLAGS, "type" },
        { "h2s", "vaapi P010 to ARGB",        0, AV_OPT_TYPE_CONST, {.i64=HDR_VAAPI_H2S}, INT_MIN, INT_MAX, FLAGS, "type" },
    { "display", "set master display",  OFFSET(master_display), AV_OPT_TYPE_STRING, {.str=NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "light",   "set content light",   OFFSET(content_light),  AV_OPT_TYPE_STRING, {.str=NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
    { NULL }
};


AVFILTER_DEFINE_CLASS(tonemap_vaapi);

static const AVFilterPad tonemap_vaapi_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &tonemap_vaapi_filter_frame,
        .config_props = &ff_vaapi_vpp_config_input,
    },
    { NULL }
};

static const AVFilterPad tonemap_vaapi_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &ff_vaapi_vpp_config_output,
    },
    { NULL }
};

AVFilter ff_vf_tonemap_vaapi = {
    .name           = "tonemap_vaapi",
    .description    = NULL_IF_CONFIG_SMALL("VAAPI VPP for tonemap"),
    .priv_size      = sizeof(HDRVAAPIContext),
    .init           = &tonemap_vaapi_init,
    .uninit         = &ff_vaapi_vpp_ctx_uninit,
    .query_formats  = &tonemap_vaapi_vpp_query_formats,
    .inputs         = tonemap_vaapi_inputs,
    .outputs        = tonemap_vaapi_outputs,
    .priv_class     = &tonemap_vaapi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};

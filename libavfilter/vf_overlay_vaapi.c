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

#include "avfilter.h"
#include "framesync.h"
#include "formats.h"
#include "internal.h"
#include "vaapi_vpp.h"

typedef struct OverlayVAAPIContext {
    VAAPIVPPContext  vpp_ctx; // must be the first field
    FFFrameSync      fs;
    int              x;
    int              y;
    int              w;
    int              h;
    float            alpha;
} OverlayVAAPIContext;

static int overlay_vaapi_query_formats(AVFilterContext *ctx)
{
    int ret;
    enum {
        MAIN    = 0,
        OVERLAY = 1,
    };

    static const enum AVPixelFormat main_in_fmts[] = {
        AV_PIX_FMT_VAAPI,
        AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat out_pix_fmts[] = {
        AV_PIX_FMT_VAAPI,
        AV_PIX_FMT_NONE
    };

    ret = ff_formats_ref(ff_make_format_list(main_in_fmts), &ctx->inputs[MAIN]->out_formats);
    if (ret < 0)
      return ret;

    ret = ff_formats_ref(ff_make_format_list(main_in_fmts), &ctx->inputs[OVERLAY]->out_formats);
    if (ret < 0)
      return ret;

    ret = ff_formats_ref(ff_make_format_list(out_pix_fmts), &ctx->outputs[0]->in_formats);
    if (ret < 0)
        return ret;

    return 0;
}

static int overlay_vaapi_build_filter_params(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx   = avctx->priv;
    VAStatus vas;
    int support_flag;
    VAProcPipelineCaps pipeline_caps;

    memset(&pipeline_caps, 0, sizeof(pipeline_caps));
    vas = vaQueryVideoProcPipelineCaps(vpp_ctx->hwctx->display,
                                       vpp_ctx->va_context,
                                       NULL, 0,
                                       &pipeline_caps);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to query pipeline "
               "caps: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    if (!pipeline_caps.blend_flags) {
        av_log(avctx, AV_LOG_ERROR, "VAAPI driver doesn't support overlay\n");
        return AVERROR(EINVAL);
    }

    support_flag = pipeline_caps.blend_flags & VA_BLEND_GLOBAL_ALPHA;
    if (!support_flag) {
      av_log(avctx, AV_LOG_ERROR, "VAAPI driver doesn't support global alpha blending\n");
        return AVERROR(EINVAL);
    }

    return 0;
}


static int overlay_vaapi_render(AVFilterContext *avctx,
                                VAProcPipelineParameterBuffer *params,
                                VAProcPipelineParameterBuffer *subpic_params,
                                VASurfaceID output_surface)
{
    VABufferID params_id;
    VABufferID subpic_params_id;
    VAStatus vas;
    int err = 0;
    VAAPIVPPContext *ctx   = avctx->priv;

    vas = vaBeginPicture(ctx->hwctx->display,
                         ctx->va_context, output_surface);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to attach new picture: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail;
    }

    vas = vaCreateBuffer(ctx->hwctx->display, ctx->va_context,
                         VAProcPipelineParameterBufferType,
                         sizeof(*params), 1, params, &params_id);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create parameter buffer: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_begin;
    }
    av_log(avctx, AV_LOG_DEBUG, "Pipeline parameter buffer is %#x.\n",
           params_id);


    vas = vaCreateBuffer(ctx->hwctx->display, ctx->va_context,
                         VAProcPipelineParameterBufferType,
                         sizeof(*subpic_params), 1, subpic_params, &subpic_params_id);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create parameter buffer: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_begin;
    }
    av_log(avctx, AV_LOG_DEBUG, "Pipeline subpic parameter buffer is %#x.\n",
           subpic_params_id);

    vas = vaRenderPicture(ctx->hwctx->display, ctx->va_context,
                          &params_id, 1);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to render parameter buffer: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_begin;
    }

    vas = vaRenderPicture(ctx->hwctx->display, ctx->va_context,
                          &subpic_params_id, 1);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to render subpic parameter buffer: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_begin;
    }

    vas = vaEndPicture(ctx->hwctx->display, ctx->va_context);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to start picture processing: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_render;
    }

    if (CONFIG_VAAPI_1 || ctx->hwctx->driver_quirks &
        AV_VAAPI_DRIVER_QUIRK_RENDER_PARAM_BUFFERS) {
        vas = vaDestroyBuffer(ctx->hwctx->display, params_id);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to free parameter buffer: "
                   "%d (%s).\n", vas, vaErrorStr(vas));
            // And ignore.
        }
    }

    return 0;

    // We want to make sure that if vaBeginPicture has been called, we also
    // call vaRenderPicture and vaEndPicture.  These calls may well fail or
    // do something else nasty, but once we're in this failure case there
    // isn't much else we can do.
fail_after_begin:
    vaRenderPicture(ctx->hwctx->display, ctx->va_context, &params_id, 1);
fail_after_render:
    vaEndPicture(ctx->hwctx->display, ctx->va_context);
fail:
    return err;
}

static int overlay_vaapi_blend(FFFrameSync *fs)
{
    AVFilterContext    *avctx = fs->parent;
    AVFilterLink     *outlink = avctx->outputs[0];
    OverlayVAAPIContext *ctx  = avctx->priv;
    VAAPIVPPContext *vpp_ctx  = avctx->priv;
    AVFrame *input_main, *input_overlay;
    AVFrame *output;
    int err;
    VASurfaceID main_surface, overlay_surface, output_surface;
    VARectangle overlay_region, output_region;
    VAProcPipelineParameterBuffer params, subpic_params;
    VABlendState blend_state; // Blend State

    err = overlay_vaapi_build_filter_params(avctx);
    if (err < 0)
        return err;

    err = ff_framesync_get_frame(fs, 0, &input_main, 0);
    if (err < 0)
        return err;
    err = ff_framesync_get_frame(fs, 1, &input_overlay, 0);
    if (err < 0)
        return err;

    av_log(avctx, AV_LOG_DEBUG, "Filter main: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_main->format),
           input_main->width, input_main->height, input_main->pts);

    av_log(avctx, AV_LOG_DEBUG, "Filter overlay: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_overlay->format),
           input_overlay->width, input_overlay->height, input_overlay->pts);

    if (vpp_ctx->va_context == VA_INVALID_ID)
        return AVERROR(EINVAL);

    main_surface = (VASurfaceID)(uintptr_t)input_main->data[3];
    av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for overlay vpp main.\n",
           main_surface);

    overlay_surface = (VASurfaceID)(uintptr_t)input_overlay->data[3];
    av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for overlay vpp overlay.\n",
           overlay_surface);

    output = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!output) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    output_surface = (VASurfaceID)(uintptr_t)output->data[3];
    av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for overlay vpp output.\n",
           output_surface);

    memset(&params, 0, sizeof(params));
    memset(&subpic_params, 0, sizeof(subpic_params));

    overlay_region = (VARectangle) {
        .x      = ctx->x,
        .y      = ctx->y,
        .width  = input_overlay->width,
        .height = input_overlay->height,
    };

    output_region = (VARectangle) {
        .x      = 0,
        .y      = 0,
        .width  = output->width,
        .height = output->height,
    };

    if (ctx->w && ctx->h) {
        overlay_region.width = ctx->w;
        overlay_region.height = ctx->h;
    }

    if (overlay_region.width > input_main->width ||
        overlay_region.height > input_main->height) {
        av_log(ctx, AV_LOG_ERROR,
               "Overlay width > main width or overlay height > main height is not supported.\n");
        err =  AVERROR(EINVAL);
        goto fail;
    }

    params.filters     = &vpp_ctx->filter_buffers[0];
    params.num_filters = vpp_ctx->nb_filter_buffers;

    err = ff_vaapi_vpp_init_params(avctx, &params,
                                   input_main, output);
    if (err < 0)
        goto fail;

    params.output_region = &output_region;
    params.output_background_color = VAAPI_VPP_BACKGROUND_BLACK;
    params.output_color_standard = params.surface_color_standard;

    memcpy(&subpic_params, &params, sizeof(subpic_params));

    blend_state.flags = VA_BLEND_GLOBAL_ALPHA;
    blend_state.global_alpha = ctx->alpha;
    subpic_params.blend_state = &blend_state;

    subpic_params.surface = overlay_surface;
    subpic_params.output_region = &overlay_region;

    err = overlay_vaapi_render(avctx, &params, &subpic_params, output_surface);
    if (err < 0)
        goto fail;

    err = av_frame_copy_props(output, input_main);
    if (err < 0)
        goto fail;

    av_log(avctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output->format),
           output->width, output->height, output->pts);

    return ff_filter_frame(outlink, output);

fail:
    av_frame_free(&output);
    return err;
}

static int overlay_vaapi_init_framesync(AVFilterContext *avctx)
{
    OverlayVAAPIContext *ctx = avctx->priv;
    int ret, i;

    ctx->fs.on_event = overlay_vaapi_blend;
    ctx->fs.opaque   = ctx;
    ret = ff_framesync_init(&ctx->fs, avctx, avctx->nb_inputs);
    if (ret < 0)
        return ret;

    for (i = 0; i < avctx->nb_inputs; i++) {
        FFFrameSyncIn *in = &ctx->fs.in[i];
        in->before    = EXT_STOP;
        in->after     = EXT_INFINITY;
        in->sync      = i ? 1 : 2;
        in->time_base = avctx->inputs[i]->time_base;
    }

    return ff_framesync_configure(&ctx->fs);
}

static int overlay_vaapi_config_output(AVFilterLink *outlink)
{
    AVFilterContext  *avctx  = outlink->src;
    OverlayVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    AVFilterLink        *in0 = avctx->inputs[0];
    AVFilterLink        *in1 = avctx->inputs[1];
    int err;

    av_log(ctx, AV_LOG_DEBUG, "Output is of %s.\n", av_get_pix_fmt_name(outlink->format));
    if ((in0->format == AV_PIX_FMT_VAAPI && in1->format != AV_PIX_FMT_VAAPI) ||
        (in0->format != AV_PIX_FMT_VAAPI && in1->format == AV_PIX_FMT_VAAPI)) {
        av_log(ctx, AV_LOG_ERROR, "Mixing hardware and software pixel formats is not supported.\n");
        return AVERROR(EINVAL);
    }

    err = overlay_vaapi_init_framesync(avctx);
    if (err < 0)
        return err;

    vpp_ctx->output_width  = avctx->inputs[0]->w;
    vpp_ctx->output_height = avctx->inputs[0]->h;

    err = ff_vaapi_vpp_config_output(outlink);
    if (err < 0)
        return err;

    err = ff_framesync_init_dualinput(&ctx->fs, avctx);
    if (err < 0)
        return err;

    return ff_framesync_configure(&ctx->fs);
}

static av_cold int overlay_vaapi_init(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx = avctx->priv;

    ff_vaapi_vpp_ctx_init(avctx);
    vpp_ctx->output_format = AV_PIX_FMT_NONE;

    return 0;
}

static int overlay_vaapi_activate(AVFilterContext *avctx)
{
    OverlayVAAPIContext *ctx = avctx->priv;

    return ff_framesync_activate(&ctx->fs);
}

static av_cold void overlay_vaapi_uninit(AVFilterContext *avctx)
{
    OverlayVAAPIContext *ctx = avctx->priv;

    ff_framesync_uninit(&ctx->fs);
}

#define OFFSET(x) offsetof(OverlayVAAPIContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption overlay_vaapi_options[] = {
    { "x", "Overlay x position",
      OFFSET(x), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
    { "y", "Overlay y position",
      OFFSET(y), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
    { "w", "Overlay width",
      OFFSET(w), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
    { "h", "Overlay height",
      OFFSET(h), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
    { "alpha", "Overlay global alpha",
      OFFSET(alpha), AV_OPT_TYPE_FLOAT, { .dbl = 0.0}, 0.0, 1.0, .flags = FLAGS},
    { NULL },
};

AVFILTER_DEFINE_CLASS(overlay_vaapi);

static const AVFilterPad overlay_vaapi_inputs[] = {
    {
        .name             = "main",
        .type             = AVMEDIA_TYPE_VIDEO,
        .get_video_buffer = ff_default_get_video_buffer,
        .config_props     = &ff_vaapi_vpp_config_input,
        .needs_fifo       = 1,
    },
    {
        .name             = "overlay",
        .type             = AVMEDIA_TYPE_VIDEO,
        .get_video_buffer = ff_default_get_video_buffer,
        .needs_fifo       = 1,
    },
    { NULL }
};

static const AVFilterPad overlay_vaapi_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = &overlay_vaapi_config_output,
    },
    { NULL }
};

AVFilter ff_vf_overlay_vaapi = {
    .name            = "overlay_vaapi",
    .description     = NULL_IF_CONFIG_SMALL("Overlay one video on top of another"),
    .priv_size       = sizeof(OverlayVAAPIContext),
    .priv_class      = &overlay_vaapi_class,
    .init            = &overlay_vaapi_init,
    .uninit          = &overlay_vaapi_uninit,
    .query_formats   = &overlay_vaapi_query_formats,
    .activate        = &overlay_vaapi_activate,
    .inputs          = overlay_vaapi_inputs,
    .outputs         = overlay_vaapi_outputs,
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
};

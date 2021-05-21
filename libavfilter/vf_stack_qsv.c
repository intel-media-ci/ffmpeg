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

/**
 * @file
 * Hardware accelerated hstack, vstack and xstack filters based on Intel Quick Sync Video VPP
 */

#include "libavutil/opt.h"
#include "libavutil/common.h"
#include "libavutil/pixdesc.h"
#include "libavutil/eval.h"
#include "libavutil/hwcontext.h"
#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/parseutils.h"

#include "internal.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

#include "framesync.h"
#include "qsvvpp.h"

#define OFFSET(x) offsetof(QSVStackContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

enum {
    QSV_STACK_H = 0,
    QSV_STACK_V = 1,
    QSV_STACK_X = 2
};

typedef struct QSVStackContext {
    const AVClass *class;
    QSVVPPContext *qsv;
    QSVVPPParam qsv_param;
    mfxExtVPPComposite comp_conf;
    int mode;
    FFFrameSync fs;

    /* Options */
    int nb_inputs;
    int shortest;
    double scale;
    char *layout;
    uint8_t fillcolor[4];
    char *fillcolor_str;
    int fillcolor_enable;
} QSVStackContext;

static void rgb2yuv(float r, float g, float b, int *y, int *u, int *v, int depth)
{
    *y = ((0.21260*219.0/255.0) * r + (0.71520*219.0/255.0) * g +
         (0.07220*219.0/255.0) * b) * ((1 << depth) - 1);
    *u = (-(0.11457*224.0/255.0) * r - (0.38543*224.0/255.0) * g +
         (0.50000*224.0/255.0) * b + 0.5) * ((1 << depth) - 1);
    *v = ((0.50000*224.0/255.0) * r - (0.45415*224.0/255.0) * g -
         (0.04585*224.0/255.0) * b + 0.5) * ((1 << depth) - 1);
}

static int process_frame(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    QSVStackContext *sctx = fs->opaque;
    AVFrame  *frame = NULL;
    int ret = 0, i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        ret = ff_framesync_get_frame(fs, i, &frame, 0);
        if (ret == 0)
            ret = ff_qsvvpp_filter_frame(sctx->qsv, ctx->inputs[i], frame);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            break;
    }

    if (ret == 0 && sctx->qsv->got_frame == 0) {
        for (i = 0; i < ctx->nb_inputs; i++)
            FF_FILTER_FORWARD_WANTED(ctx->outputs[0], ctx->inputs[i]);

        ret = FFERROR_NOT_READY;
    }

    return ret;
}

static int init_framesync(AVFilterContext *ctx)
{
    QSVStackContext *sctx = ctx->priv;
    int ret, i;

    ret = ff_framesync_init(&sctx->fs, ctx, ctx->nb_inputs);

    if (ret < 0)
        return ret;

    sctx->fs.on_event = process_frame;
    sctx->fs.opaque = sctx;

    for (i = 0; i < ctx->nb_inputs; i++) {
        FFFrameSyncIn *in = &sctx->fs.in[i];
        in->before = EXT_STOP;
        in->after = sctx->shortest ? EXT_STOP : EXT_INFINITY;
        in->sync = i ? 1 : 2;
        in->time_base = ctx->inputs[i]->time_base;
    }

    return ff_framesync_configure(&sctx->fs);
}

#define SET_INPUT_STREAM(is, x, y, w, h) do {   \
        is->DstX = x;                           \
        is->DstY = y;                           \
        is->DstW = w;                           \
        is->DstH = h;                           \
        is->GlobalAlpha = 255;                  \
        is->GlobalAlphaEnable = 1;              \
        is->PixelAlphaEnable = 0;               \
    } while (0)

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    QSVStackContext *sctx = ctx->priv;
    AVFilterLink *inlink0 = ctx->inputs[0];
    int width, height, i, ret;
    enum AVPixelFormat in_format;
    int depth = 8;

    if (inlink0->format == AV_PIX_FMT_QSV) {
         if (!inlink0->hw_frames_ctx || !inlink0->hw_frames_ctx->data)
             return AVERROR(EINVAL);

         in_format = ((AVHWFramesContext*)inlink0->hw_frames_ctx->data)->sw_format;
    } else
        in_format = inlink0->format;

    sctx->qsv_param.out_sw_format = in_format;

    for (i = 1; i < sctx->nb_inputs; i++) {
        AVFilterLink *inlink = ctx->inputs[i];

        if (inlink0->format == AV_PIX_FMT_QSV) {
            AVHWFramesContext *hwfc0 = (AVHWFramesContext *)inlink0->hw_frames_ctx->data;
            AVHWFramesContext *hwfc = (AVHWFramesContext *)inlink->hw_frames_ctx->data;

            if (inlink0->format != inlink->format) {
                av_log(ctx, AV_LOG_ERROR, "Mixing hardware and software pixel formats is not supported.\n");

                return AVERROR(EINVAL);
            } else if (hwfc0->device_ctx != hwfc->device_ctx) {
                av_log(ctx, AV_LOG_ERROR, "Inputs with different underlying QSV devices are forbidden.\n");

                return AVERROR(EINVAL);
            }
        }
    }

    if (in_format == AV_PIX_FMT_P010)
        depth = 10;

    if (sctx->fillcolor_enable) {
        int Y, U, V;

        rgb2yuv(sctx->fillcolor[0] / 255.0, sctx->fillcolor[1] / 255.0,
                sctx->fillcolor[2] / 255.0, &Y, &U, &V, depth);
        sctx->comp_conf.Y = Y;
        sctx->comp_conf.U = U;
        sctx->comp_conf.V = V;
    }

    if (sctx->mode == QSV_STACK_H) {
        height = inlink0->h * sctx->scale;
        width = 0;

        for (i = 0; i < sctx->nb_inputs; i++) {
            AVFilterLink *inlink = ctx->inputs[i];
            mfxVPPCompInputStream *is = &sctx->comp_conf.InputStream[i];

            if (inlink0->h != inlink->h) {
                av_log(ctx, AV_LOG_ERROR, "Input %d height %d does not match input %d height %d.\n", i, inlink->h, 0, inlink0->h);
                return AVERROR(EINVAL);
            }

            SET_INPUT_STREAM(is, width, 0, inlink->w * sctx->scale, inlink->h * sctx->scale);
            width += inlink->w * sctx->scale;
        }
    } else if (sctx->mode == QSV_STACK_V) {
        height = 0;
        width = inlink0->w * sctx->scale;

        for (i = 0; i < sctx->nb_inputs; i++) {
            AVFilterLink *inlink = ctx->inputs[i];
            mfxVPPCompInputStream *is = &sctx->comp_conf.InputStream[i];

            if (inlink0->w != inlink->w) {
                av_log(ctx, AV_LOG_ERROR, "Input %d width %d does not match input %d width %d.\n", i, inlink->w, 0, inlink0->w);
                return AVERROR(EINVAL);
            }

            SET_INPUT_STREAM(is, 0, height, inlink->w * sctx->scale, inlink->h * sctx->scale);
            height += inlink->h * sctx->scale;
        }
    } else {
        char *arg, *p = sctx->layout, *saveptr = NULL;
        char *arg2, *p2, *saveptr2 = NULL;
        char *arg3, *p3, *saveptr3 = NULL;
        int inw, inh, size, j;

        width = ctx->inputs[0]->w * sctx->scale;
        height = ctx->inputs[0]->h * sctx->scale;

        for (i = 0; i < sctx->nb_inputs; i++) {
            AVFilterLink *inlink = ctx->inputs[i];
            mfxVPPCompInputStream *is = &sctx->comp_conf.InputStream[i];

            if (!(arg = av_strtok(p, "|", &saveptr)))
                return AVERROR(EINVAL);

            p = NULL;
            p2 = arg;
            inw = inh = 0;

            for (j = 0; j < 2; j++) {
                if (!(arg2 = av_strtok(p2, "_", &saveptr2)))
                    return AVERROR(EINVAL);

                p2 = NULL;
                p3 = arg2;

                while ((arg3 = av_strtok(p3, "+", &saveptr3))) {
                    p3 = NULL;
                    if (sscanf(arg3, "w%d", &size) == 1) {
                        if (size == i || size < 0 || size >= sctx->nb_inputs)
                            return AVERROR(EINVAL);

                        if (!j)
                            inw += ctx->inputs[size]->w * sctx->scale;
                        else
                            inh += ctx->inputs[size]->w * sctx->scale;
                    } else if (sscanf(arg3, "h%d", &size) == 1) {
                        if (size == i || size < 0 || size >= sctx->nb_inputs)
                            return AVERROR(EINVAL);

                        if (!j)
                            inw += ctx->inputs[size]->h * sctx->scale;
                        else
                            inh += ctx->inputs[size]->h * sctx->scale;
                    } else if (sscanf(arg3, "%d", &size) == 1) {
                        if (size < 0)
                            return AVERROR(EINVAL);

                        if (!j)
                            inw += size;
                        else
                            inh += size;
                    } else {
                        return AVERROR(EINVAL);
                    }
                }
            }

            SET_INPUT_STREAM(is, inw, inh, inlink->w * sctx->scale, inlink->h * sctx->scale);
            width = FFMAX(width, inlink->w * sctx->scale + inw);
            height = FFMAX(height, inlink->h * sctx->scale + inh);
        }

    }

    outlink->w = width;
    outlink->h = height;
    outlink->frame_rate = inlink0->frame_rate;
    outlink->time_base = av_inv_q(outlink->frame_rate);
    outlink->sample_aspect_ratio = inlink0->sample_aspect_ratio;

    ret = init_framesync(ctx);

    if (ret < 0)
        return ret;

    return ff_qsvvpp_create(ctx, &sctx->qsv, &sctx->qsv_param);
}

/*
 * Callback for qsvvpp
 * @Note: qsvvpp composition does not generate PTS for result frame.
 *        so we assign the PTS from framesync to the output frame.
 */

static int filter_callback(AVFilterLink *outlink, AVFrame *frame)
{
    QSVStackContext *sctx = outlink->src->priv;

    frame->pts = av_rescale_q(sctx->fs.pts,
                              sctx->fs.time_base, outlink->time_base);
    return ff_filter_frame(outlink, frame);
}


static int stack_qsv_init(AVFilterContext *ctx)
{
    QSVStackContext *sctx = ctx->priv;
    int i, ret;

    if (!strcmp(ctx->filter->name, "hstack_qsv"))
        sctx->mode = QSV_STACK_H;
    else if (!strcmp(ctx->filter->name, "vstack_qsv"))
        sctx->mode = QSV_STACK_V;
    else {
        av_assert0(strcmp(ctx->filter->name, "xstack_qsv") == 0);
        sctx->mode = QSV_STACK_X;

        if (strcmp(sctx->fillcolor_str, "none") &&
            av_parse_color(sctx->fillcolor, sctx->fillcolor_str, -1, ctx) >= 0) {
            sctx->fillcolor_enable = 1;
        } else {
            sctx->fillcolor_enable = 0;
        }

        if (!sctx->layout) {
            if (sctx->nb_inputs == 2) {
                sctx->layout = av_strdup("0_0|w0_0");

                if (!sctx->layout)
                    return AVERROR(ENOMEM);
            } else {
                av_log(ctx, AV_LOG_ERROR, "No layout specified.\n");

                return AVERROR(EINVAL);
            }
        }
    }

    for (i = 0; i < sctx->nb_inputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_VIDEO;
        pad.name = av_asprintf("input%d", i);

        if (!pad.name)
            return AVERROR(ENOMEM);

        if ((ret = ff_insert_inpad(ctx, i, &pad)) < 0) {
            av_freep(&pad.name);

            return ret;
        }
    }

    /* fill composite config */
    sctx->comp_conf.Header.BufferId = MFX_EXTBUFF_VPP_COMPOSITE;
    sctx->comp_conf.Header.BufferSz = sizeof(sctx->comp_conf);
    sctx->comp_conf.NumInputStream = sctx->nb_inputs;
    sctx->comp_conf.InputStream = av_mallocz_array(sctx->nb_inputs,
                                                  sizeof(*sctx->comp_conf.InputStream));
    if (!sctx->comp_conf.InputStream)
        return AVERROR(ENOMEM);

    /* initialize QSVVPP params */
    sctx->qsv_param.filter_frame = filter_callback;
    sctx->qsv_param.ext_buf = av_mallocz(sizeof(*sctx->qsv_param.ext_buf));

    if (!sctx->qsv_param.ext_buf)
        return AVERROR(ENOMEM);

    sctx->qsv_param.ext_buf[0] = (mfxExtBuffer *)&sctx->comp_conf;
    sctx->qsv_param.num_ext_buf = 1;
    sctx->qsv_param.num_crop = 0;

    return 0;
}

static av_cold void stack_qsv_uninit(AVFilterContext *ctx)
{
    QSVStackContext *sctx = ctx->priv;
    int i;

    ff_qsvvpp_free(&sctx->qsv);
    ff_framesync_uninit(&sctx->fs);
    av_freep(&sctx->comp_conf.InputStream);
    av_freep(&sctx->qsv_param.ext_buf);

    for (i = 0; i < ctx->nb_inputs; i++)
        av_freep(&ctx->input_pads[i].name);
}

static int stack_qsv_activate(AVFilterContext *ctx)
{
    QSVStackContext *sctx = ctx->priv;
    return ff_framesync_activate(&sctx->fs);
}

static int stack_qsv_query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pixel_formats[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_P010,
        AV_PIX_FMT_QSV,
        AV_PIX_FMT_NONE,
    };
    AVFilterFormats *pix_fmts = ff_make_format_list(pixel_formats);

    return ff_set_common_formats(ctx, pix_fmts);
}

static const AVFilterPad stack_qsv_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },

    { NULL }
};

static const AVOption stack_qsv_options[] = {
    { "inputs", "set number of inputs", OFFSET(nb_inputs), AV_OPT_TYPE_INT, { .i64 = 2 }, 2, 72, .flags = FLAGS },
    { "shortest", "force termination when the shortest input terminates", OFFSET(shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "scale", "set scale factor", OFFSET(scale),  AV_OPT_TYPE_DOUBLE, { .dbl = 1.0  }, 0.125, 8, FLAGS },
    { NULL }
};

#define hstack_qsv_options stack_qsv_options
AVFILTER_DEFINE_CLASS(hstack_qsv);

const AVFilter ff_vf_hstack_qsv = {
    .name           = "hstack_qsv",
    .description    = NULL_IF_CONFIG_SMALL("Quick Sync Video hstack."),
    .priv_size      = sizeof(QSVStackContext),
    .priv_class     = &hstack_qsv_class,
    .query_formats  = stack_qsv_query_formats,
    .outputs        = stack_qsv_outputs,
    .init           = stack_qsv_init,
    .uninit         = stack_qsv_uninit,
    .activate       = stack_qsv_activate,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags          = AVFILTER_FLAG_DYNAMIC_INPUTS,
};

#define vstack_qsv_options stack_qsv_options
AVFILTER_DEFINE_CLASS(vstack_qsv);

const AVFilter ff_vf_vstack_qsv = {
    .name           = "vstack_qsv",
    .description    = NULL_IF_CONFIG_SMALL("Quick Sync Video vstack."),
    .priv_size      = sizeof(QSVStackContext),
    .priv_class     = &vstack_qsv_class,
    .query_formats  = stack_qsv_query_formats,
    .outputs        = stack_qsv_outputs,
    .init           = stack_qsv_init,
    .uninit         = stack_qsv_uninit,
    .activate       = stack_qsv_activate,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags          = AVFILTER_FLAG_DYNAMIC_INPUTS,
};

static const AVOption xstack_qsv_options[] = {
    { "inputs", "set number of inputs", OFFSET(nb_inputs), AV_OPT_TYPE_INT, { .i64 = 2 }, 2, 72, .flags = FLAGS },
    { "layout", "set custom layout", OFFSET(layout), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, .flags = FLAGS },
    { "shortest", "force termination when the shortest input terminates", OFFSET(shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "fill",  "set the color for unused pixels", OFFSET(fillcolor_str), AV_OPT_TYPE_STRING, {.str = "none"}, .flags = FLAGS },
    { "scale", "set scale factor", OFFSET(scale),  AV_OPT_TYPE_DOUBLE, { .dbl = 1.0  }, 0.125, 8, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(xstack_qsv);

const AVFilter ff_vf_xstack_qsv = {
    .name           = "xstack_qsv",
    .description    = NULL_IF_CONFIG_SMALL("Quick Sync Video xstack."),
    .priv_size      = sizeof(QSVStackContext),
    .priv_class     = &xstack_qsv_class,
    .query_formats  = stack_qsv_query_formats,
    .outputs        = stack_qsv_outputs,
    .init           = stack_qsv_init,
    .uninit         = stack_qsv_uninit,
    .activate       = stack_qsv_activate,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags          = AVFILTER_FLAG_DYNAMIC_INPUTS,
};

/*
 * Copyright (c) 2021
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

/**
 * @file
 * implementing an object detecting filter using deep learning networks.
 */

#include "libavformat/avio.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "filters.h"
#include "dnn_filter_common.h"
#include "formats.h"
#include "internal.h"
#include "libavutil/time.h"

typedef struct DnnDetectContext {
    const AVClass *class;
    DnnContext dnnctx;
} DnnDetectContext;

#define OFFSET(x) offsetof(DnnDetectContext, dnnctx.x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption dnn_detect_options[] = {
    { "dnn_backend", "DNN backend",                OFFSET(backend_type),     AV_OPT_TYPE_INT,       { .i64 = 2 },    INT_MIN, INT_MAX, FLAGS, "backend" },
#if (CONFIG_LIBOPENVINO == 1)
    { "openvino",    "openvino backend flag",      0,                        AV_OPT_TYPE_CONST,     { .i64 = 2 },    0, 0, FLAGS, "backend" },
#endif
    DNN_COMMON_OPTIONS
    { NULL }
};

AVFILTER_DEFINE_CLASS(dnn_detect);

static int dnn_detect_post_proc(AVFrame *frame, DNNData *model, AVFilterContext *filter_ctx)
{
    // read from model output and fill the sidedata in AVFrame
    return 0;
}

static av_cold int dnn_detect_init(AVFilterContext *context)
{
    DnnDetectContext *ctx = context->priv;
    int ret = ff_dnn_init(&ctx->dnnctx, context);
    if (ret < 0)
        return ret;
    ff_dnn_set_proc(&ctx->dnnctx, NULL, dnn_detect_post_proc);
    return 0;
}

static int dnn_detect_query_formats(AVFilterContext *context)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAYF32,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    return ff_set_common_formats(context, fmts_list);
}

static int dnn_detect_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *context  = inlink->dst;
    AVFilterLink *outlink = context->outputs[0];
    DnnDetectContext *ctx = context->priv;
    DNNReturnType dnn_result;

    dnn_result = ff_dnn_execute_model(&ctx->dnnctx, in, NULL);
    if (dnn_result != DNN_SUCCESS){
        av_log(ctx, AV_LOG_ERROR, "failed to execute model\n");
        av_frame_free(&in);
        return AVERROR(EIO);
    }

    return ff_filter_frame(outlink, in);
}

static int dnn_detect_activate_sync(AVFilterContext *filter_ctx)
{
    AVFilterLink *inlink = filter_ctx->inputs[0];
    AVFilterLink *outlink = filter_ctx->outputs[0];
    AVFrame *in = NULL;
    int64_t pts;
    int ret, status;
    int got_frame = 0;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    do {
        // drain all input frames
        ret = ff_inlink_consume_frame(inlink, &in);
        if (ret < 0)
            return ret;
        if (ret > 0) {
            ret = dnn_detect_filter_frame(inlink, in);
            if (ret < 0)
                return ret;
            got_frame = 1;
        }
    } while (ret > 0);

    // if frame got, schedule to next filter
    if (got_frame)
        return 0;

    if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        if (status == AVERROR_EOF) {
            ff_outlink_set_status(outlink, status, pts);
            return ret;
        }
    }

    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

static int dnn_detect_flush_frame(AVFilterLink *outlink, int64_t pts, int64_t *out_pts)
{
    DnnDetectContext *ctx = outlink->src->priv;
    int ret;
    DNNAsyncStatusType async_state;

    ret = ff_dnn_flush(&ctx->dnnctx);
    if (ret != DNN_SUCCESS) {
        return -1;
    }

    do {
        AVFrame *in_frame = NULL;
        AVFrame *out_frame = NULL;
        async_state = ff_dnn_get_async_result(&ctx->dnnctx, &in_frame, &out_frame);
        if (out_frame) {
            av_assert0(in_frame == out_frame);
            ret = ff_filter_frame(outlink, out_frame);
            if (ret < 0)
                return ret;
            if (out_pts)
                *out_pts = out_frame->pts + pts;
        }
        av_usleep(5000);
    } while (async_state >= DAST_NOT_READY);

    return 0;
}

static int dnn_detect_activate_async(AVFilterContext *filter_ctx)
{
    AVFilterLink *inlink = filter_ctx->inputs[0];
    AVFilterLink *outlink = filter_ctx->outputs[0];
    DnnDetectContext *ctx = filter_ctx->priv;
    AVFrame *in = NULL;
    int64_t pts;
    int ret, status;
    int got_frame = 0;
    int async_state;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    do {
        // drain all input frames
        ret = ff_inlink_consume_frame(inlink, &in);
        if (ret < 0)
            return ret;
        if (ret > 0) {
            if (ff_dnn_execute_model_async(&ctx->dnnctx, in, NULL) != DNN_SUCCESS) {
                return AVERROR(EIO);
            }
        }
    } while (ret > 0);

    // drain all processed frames
    do {
        AVFrame *in_frame = NULL;
        AVFrame *out_frame = NULL;
        async_state = ff_dnn_get_async_result(&ctx->dnnctx, &in_frame, &out_frame);
        if (out_frame) {
            av_assert0(in_frame == out_frame);
            ret = ff_filter_frame(outlink, out_frame);
            if (ret < 0)
                return ret;
            got_frame = 1;
        }
    } while (async_state == DAST_SUCCESS);

    // if frame got, schedule to next filter
    if (got_frame)
        return 0;

    if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        if (status == AVERROR_EOF) {
            int64_t out_pts = pts;
            ret = dnn_detect_flush_frame(outlink, pts, &out_pts);
            ff_outlink_set_status(outlink, status, out_pts);
            return ret;
        }
    }

    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return 0;
}

static int dnn_detect_activate(AVFilterContext *filter_ctx)
{
    DnnDetectContext *ctx = filter_ctx->priv;

    if (ctx->dnnctx.async)
        return dnn_detect_activate_async(filter_ctx);
    else
        return dnn_detect_activate_sync(filter_ctx);
}

static av_cold void dnn_detect_uninit(AVFilterContext *ctx)
{
    DnnDetectContext *context = ctx->priv;
    ff_dnn_uninit(&context->dnnctx);
}

static const AVFilterPad dnn_detect_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

static const AVFilterPad dnn_detect_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_dnn_detect = {
    .name          = "dnn_detect",
    .description   = NULL_IF_CONFIG_SMALL("Apply DNN detect filter to the input."),
    .priv_size     = sizeof(DnnDetectContext),
    .init          = dnn_detect_init,
    .uninit        = dnn_detect_uninit,
    .query_formats = dnn_detect_query_formats,
    .inputs        = dnn_detect_inputs,
    .outputs       = dnn_detect_outputs,
    .priv_class    = &dnn_detect_class,
    .activate      = dnn_detect_activate,
};

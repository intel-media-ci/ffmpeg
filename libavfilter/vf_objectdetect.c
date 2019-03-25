/*
 * Copyright (c) 2019 Guo Yejun
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
 * Filter implementing object detection framework using deep convolutional networks.
 * models available at https://github.com/tensorflow/models/blob/master/research/object_detection/g3doc/detection_model_zoo.md
 */

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavformat/avio.h"
#include "dnn_interface.h"

typedef struct ObjectDetectContext {
    const AVClass *class;

    char *model_filename;
    DNNModule *dnn_module;
    DNNModel *model;
    DNNInputData input;
    DNNData output;
} ObjectDetectContext;

#define OFFSET(x) offsetof(ObjectDetectContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption objectdetect_options[] = {
    { "model", "path to model file specifying network architecture and its parameters", OFFSET(model_filename), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(objectdetect);

#define MODEL_OUTPUT_NB 4

static av_cold int init(AVFilterContext *context)
{
    ObjectDetectContext *od_context = context->priv;
    if (!od_context->model_filename){
        av_log(context, AV_LOG_ERROR, "model file for network was not specified\n");
        return AVERROR(EIO);
    }

    od_context->dnn_module = ff_get_dnn_module(DNN_TF);
    if (!od_context->dnn_module){
        av_log(context, AV_LOG_ERROR, "could not create DNN module for tensorflow backend\n");
        return AVERROR(ENOMEM);
    }

    od_context->model = (od_context->dnn_module->load_model)(od_context->model_filename);
    if (!od_context->model){
        av_log(context, AV_LOG_ERROR, "could not load DNN model\n");
        return AVERROR(EIO);
    }

    return 0;
}

static int query_formats(AVFilterContext *context)
{
    const enum AVPixelFormat pixel_formats[] = {AV_PIX_FMT_RGB24, AV_PIX_FMT_NONE};
    AVFilterFormats *formats_list;

    formats_list = ff_make_format_list(pixel_formats);
    if (!formats_list){
        av_log(context, AV_LOG_ERROR, "could not create formats list\n");
        return AVERROR(ENOMEM);
    }

    return ff_set_common_formats(context, formats_list);
}

static int config_props(AVFilterLink *inlink)
{
    AVFilterContext *context = inlink->dst;
    ObjectDetectContext *od_context = context->priv;
    DNNReturnType result;
    const char *model_output_names[] = {"num_detections",
                                        "detection_scores",
                                        "detection_classes",
                                        "detection_boxes"};
    av_assert0(MODEL_OUTPUT_NB == sizeof(model_output_names)/sizeof(model_output_names[0]));
    od_context->input.width = inlink->w;
    od_context->input.height = inlink->h;
    od_context->input.channels = 3;
    od_context->input.dt = DNN_UINT8;

    result = (od_context->model->set_input_output)(od_context->model->model,
                                                   &od_context->input, "image_tensor",
                                                   model_output_names, MODEL_OUTPUT_NB);
    if (result != DNN_SUCCESS){
        av_log(context, AV_LOG_ERROR, "could not set input and output for the model\n");
        return AVERROR(EIO);
    }

    return 0;
}

static int draw_box(AVFrame *in, int x0, int y0, int x1, int y1)
{
    x0 = av_clip(x0, 0, in->width);
    x1 = av_clip(x1, 0, in->width);
    y0 = av_clip(y0, 0, in->height);
    y1 = av_clip(y1, 0, in->height);
    for (int j = x0; j < x1; ++j) {
        for (int i = y0; i < y1; ++i) {
            in->data[0][i * in->linesize[0] + j * 3]     = 0xFF;
            in->data[0][i * in->linesize[0] + j * 3 + 1] = 0;
            in->data[0][i * in->linesize[0] + j * 3 + 2] = 0;
        }
    }
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *context = inlink->dst;
    AVFilterLink *outlink = context->outputs[0];
    ObjectDetectContext *ctx = context->priv;
    DNNReturnType dnn_result;
    DNNData outputs[MODEL_OUTPUT_NB];

    uint8_t *dnn_input = ctx->input.data;
    for (int i = 0; i < in->height; ++i) {
        for (int j = 0; j < in->width; ++j) {
            dnn_input[i * ctx->input.width * ctx->input.channels + j * 3]     = in->data[0][i * in->linesize[0] + j * 3];
            dnn_input[i * ctx->input.width * ctx->input.channels + j * 3 + 1] = in->data[0][i * in->linesize[0] + j * 3 + 1];
            dnn_input[i * ctx->input.width * ctx->input.channels + j * 3 + 2] = in->data[0][i * in->linesize[0] + j * 3 + 2];
        }
    }

    dnn_result = (ctx->dnn_module->execute_model)(ctx->model, outputs, MODEL_OUTPUT_NB);
    if (dnn_result != DNN_SUCCESS){
        av_log(context, AV_LOG_ERROR, "failed to execute loaded model\n");
        return AVERROR(EIO);
    }

    for (uint32_t i = 0; i < *outputs[0].data; ++i) {
        float score = outputs[1].data[i];
        int y0 = (int)(outputs[3].data[i*4] * in->height);
        int x0 = (int)(outputs[3].data[i*4+1] * in->width);
        int y1 = (int)(outputs[3].data[i*4+2] * in->height);
        int x1 = (int)(outputs[3].data[i*4+3] * in->width);
        // int class_id = (int)(outputs[2].data[i];
        int half_width = 1;
        if (score < 0.8f)
            continue;

        // can we transfer data between filters?
        // for example, I want to invoke draw_text/draw_box here,
        // but unable to pass data to vf_drawtext/vf_drawbox.
        // Or, can filters export general interface for its functionaily?
        // so we can utilize the filter functions flexibly.
        // Now, have to write code here for a simple draw_box function.
        draw_box(in, x0 - half_width, y0 - half_width, x1 + half_width, y0 + half_width);
        draw_box(in, x0 - half_width, y0 - half_width, x0 + half_width, y1 + half_width);
        draw_box(in, x1 - half_width, y0 - half_width, x1 + half_width, y1 + half_width);
        draw_box(in, x0 - half_width, y1 - half_width, x1 + half_width, y1 + half_width);
    }

    return ff_filter_frame(outlink, in);
}

static av_cold void uninit(AVFilterContext *context)
{
    ObjectDetectContext *od_context = context->priv;

    if (od_context->dnn_module)
        (od_context->dnn_module->free_model)(&od_context->model);
    av_freep(&od_context->dnn_module);
}

static const AVFilterPad objectdetect_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad objectdetect_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_objectdetect = {
    .name          = "objectdetect",
    .description   = NULL_IF_CONFIG_SMALL("Object detection using deep convolutional networks"),
    .priv_size     = sizeof(ObjectDetectContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = objectdetect_inputs,
    .outputs       = objectdetect_outputs,
    .priv_class    = &objectdetect_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};

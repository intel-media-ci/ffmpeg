/*
 * Copyright (c) 2023
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
 * DNN OpenVINO backend implementation.
 */

#include "dnn_backend_openvino2.h"
#include "dnn_io_proc.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/detection_bbox.h"
#include "libavutil/cpu.h"
#include "libavutil/opt.h"
#include "dnn_backend_common.h"
#include "../internal.h"
#include "safe_queue.h"
#include "openvino/c/openvino.h"

typedef struct OV2Options {
    char *device_type;
    int nireq;
    uint8_t async;
    int batch_size;
    int input_resizable;
} OV2Options;

typedef struct OV2Context {
    const AVClass *class;
    OV2Options options;
} OV2Context;

typedef struct OV2Model {
    OV2Context ctx;
    DNNModel *model;
    ov_core_t *core;
    ov_model_t *ov_model;
    ov_compiled_model_t *compiled_model;
    ov_output_const_port_t* input_port;
    ov_preprocess_input_info_t* input_info;
    ov_output_const_port_t* output_port;
    ov_preprocess_output_info_t* output_info;
    ov_preprocess_prepostprocessor_t* preprocess;

    SafeQueue *request_queue;
    Queue *lltask_queue;
    Queue *task_queue;
} OV2Model;

typedef struct OV2RequestItem {
    ov_infer_request_t *infer_request;
    LastLevelTaskItem **lltasks;
    uint32_t lltask_count;
    ov_callback_t callback;
} OV2RequestItem;

#define OFFSET(x) offsetof(OV2Context, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM
static const AVOption dnn_openvino2_options[] = {
    { "device", "device to run model", OFFSET(options.device_type), AV_OPT_TYPE_STRING, { .str = "CPU" }, 0, 0, FLAGS },
    DNN_BACKEND_COMMON_OPTIONS
    { "batch_size",  "batch size per request", OFFSET(options.batch_size),  AV_OPT_TYPE_INT,    { .i64 = 1 },     1, 1000, FLAGS},
    { "input_resizable", "can input be resizable or not", OFFSET(options.input_resizable), AV_OPT_TYPE_BOOL,   { .i64 = 0 },     0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(dnn_openvino2);

static DNNDataType port_datatype_to_dnn_datatype(ov_element_type_e port_datatype)
{
    switch (port_datatype)
    {
    case F32:
        return DNN_FLOAT;
    case U8:
        return DNN_UINT8;
    default:
        av_assert0(!"Not supported yet.");
        return DNN_FLOAT;
    }
}

static int fill_model_input_ov2(OV2Model *ov2_model, OV2RequestItem *request);
static void infer_completion_callback(void *args);

static int execute_model_ov2(OV2RequestItem *request, Queue *inferenceq)
{
    ov_status_e status;
    LastLevelTaskItem *lltask;
    int ret = 0;
    TaskItem *task;
    OV2Context *ctx;
    OV2Model *ov2_model;

    if (ff_queue_size(inferenceq) == 0) {
        ov_infer_request_free(request->infer_request);
        av_freep(&request);
        return 0;
    }

    lltask = ff_queue_peek_front(inferenceq);
    task = lltask->task;
    ov2_model = task->model;
    ctx = &ov2_model->ctx;

    if (task->async) {
        ret = fill_model_input_ov2(ov2_model, request);
        if (ret != 0) {
            goto err;
        }

        status = ov_infer_request_set_callback(request->infer_request, &request->callback);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to set completion callback for inference\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }

        ov_infer_request_start_async(request->infer_request);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to start async inference\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }
        return 0;
    } else {
        ret = fill_model_input_ov2(ov2_model, request);
        if (ret != 0) {
            goto err;
        }
        status = ov_infer_request_infer(request->infer_request);
        if (status != OK) {
            av_log(NULL, AV_LOG_ERROR, "Failed to start synchronous model inference for OV2\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }
        infer_completion_callback(request);
        return (task->inference_done == task->inference_todo) ? 0 : DNN_GENERIC_ERROR;
    }

err:
    return ret;
}

static int get_input_ov2(void *model, DNNData *input, const char *input_name)
{
    OV2Model *ov2_model = model;
    OV2Context *ctx = &ov2_model->ctx;
    ov_shape_t input_shape = {0};
    ov_element_type_e tensor_type;
    int64_t* dims;
    ov_status_e status;
    int input_resizable = ctx->options.input_resizable;

    if (!ov_model_is_dynamic(ov2_model->ov_model)) {
        status = ov_model_const_input_by_name(ov2_model->ov_model, input_name, &ov2_model->input_port);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get input port shape.\n");
            return DNN_GENERIC_ERROR;
        }

        status = ov_const_port_get_shape(ov2_model->input_port, &input_shape);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get input port shape.\n");
            return DNN_GENERIC_ERROR;
        }
        dims = input_shape.dims;

        status = ov_port_get_element_type(ov2_model->input_port, &tensor_type);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get input port data type.\n");
            return DNN_GENERIC_ERROR;
        }
    } else {
        avpriv_report_missing_feature(ctx, "Do not support dynamic model now.");
        return DNN_GENERIC_ERROR;
    }

    input->channels = dims[1];
    input->height   = input_resizable ? -1 : dims[2];
    input->width    = input_resizable ? -1 : dims[3];
    input->dt       = port_datatype_to_dnn_datatype(tensor_type);

    return 0;
}

static int contain_valid_detection_bbox(AVFrame *frame)
{
    AVFrameSideData *sd;
    const AVDetectionBBoxHeader *header;
    const AVDetectionBBox *bbox;

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DETECTION_BBOXES);
    if (!sd) { // this frame has nothing detected
        return 0;
    }

    if (!sd->size) {
        return 0;
    }

    header = (const AVDetectionBBoxHeader *)sd->data;
    if (!header->nb_bboxes) {
        return 0;
    }

    for (uint32_t i = 0; i < header->nb_bboxes; i++) {
        bbox = av_get_detection_bbox(header, i);
        if (bbox->x < 0 || bbox->w < 0 || bbox->x + bbox->w >= frame->width) {
            return 0;
        }
        if (bbox->y < 0 || bbox->h < 0 || bbox->y + bbox->h >= frame->width) {
            return 0;
        }

        if (bbox->classify_count == AV_NUM_DETECTION_BBOX_CLASSIFY) {
            return 0;
        }
    }

    return 1;
}

static int extract_lltask_from_task(DNNFunctionType func_type, TaskItem *task, Queue *lltask_queue, DNNExecBaseParams *exec_params)
{
    switch (func_type) {
    case DFT_PROCESS_FRAME:
    case DFT_ANALYTICS_DETECT:
    {
        LastLevelTaskItem *lltask = av_malloc(sizeof(*lltask));
        if (!lltask) {
            return AVERROR(ENOMEM);
        }
        task->inference_todo = 1;
        task->inference_done = 0;
        lltask->task = task;
        if (ff_queue_push_back(lltask_queue, lltask) < 0) {
            av_freep(&lltask);
            return AVERROR(ENOMEM);
        }
        return 0;
    }
    case DFT_ANALYTICS_CLASSIFY:
    {
        const AVDetectionBBoxHeader *header;
        AVFrame *frame = task->in_frame;
        AVFrameSideData *sd;
        DNNExecClassificationParams *params = (DNNExecClassificationParams *)exec_params;

        task->inference_todo = 0;
        task->inference_done = 0;

        if (!contain_valid_detection_bbox(frame)) {
            return 0;
        }

        sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DETECTION_BBOXES);
        header = (const AVDetectionBBoxHeader *)sd->data;

        for (uint32_t i = 0; i < header->nb_bboxes; i++) {
            LastLevelTaskItem *lltask;
            const AVDetectionBBox *bbox = av_get_detection_bbox(header, i);

            if (params->target) {
                if (av_strncasecmp(bbox->detect_label, params->target, sizeof(bbox->detect_label)) != 0) {
                    continue;
                }
            }

            lltask = av_malloc(sizeof(*lltask));
            if (!lltask) {
                return AVERROR(ENOMEM);
            }
            task->inference_todo++;
            lltask->task = task;
            lltask->bbox_index = i;
            if (ff_queue_push_back(lltask_queue, lltask) < 0) {
                av_freep(&lltask);
                return AVERROR(ENOMEM);
            }
        }
        return 0;
    }
    default:
        av_assert0(!"Do not support now");
        return AVERROR(EINVAL);
    }
}

static int init_model_ov2(OV2Model *ov2_model, const char *input_name, const char*output_name)
{
    int ret = 0;
    OV2Context *ctx = &ov2_model->ctx;
    ov_status_e status;
    ov_preprocess_input_tensor_info_t* input_tensor_info;
    ov_model_t *tmp_ov_model;
    ov_layout_t* input_layout = NULL;
    const char* input_layout_desc = "NHWC";

    if (ctx->options.batch_size <= 0) {
        ctx->options.batch_size = 1;
    }

    if (ctx->options.batch_size > 1) {
        avpriv_report_missing_feature(ctx, "Do not support batch_size > 1 for now.\n");
        ret = DNN_GENERIC_ERROR;
        goto err;
    }

    if (ov2_model->model->func_type != DFT_PROCESS_FRAME) {
        //set precision only for detect and classify
        status = ov_preprocess_prepostprocessor_create(ov2_model->ov_model, &ov2_model->preprocess);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create preprocess for ov2_model.\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }

        status = ov_preprocess_prepostprocessor_get_input_info_by_name(ov2_model->preprocess, input_name, &ov2_model->input_info);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get input info from preprocess.\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }

        status = ov_preprocess_input_info_get_tensor_info(ov2_model->input_info, &input_tensor_info);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get tensor info from input.\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }

        status = ov_preprocess_input_tensor_info_set_element_type(input_tensor_info, U8);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to set input tensor to UINT8\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }

        //set input layout
        status = ov_layout_create(input_layout_desc, &input_layout);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create layout for input.\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }

        status = ov_preprocess_input_tensor_info_set_layout(input_tensor_info, input_layout);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get model_input_info\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }

        //update model
        if(ov2_model->ov_model) {
            tmp_ov_model = ov2_model->ov_model;
        }
        status = ov_preprocess_prepostprocessor_build(ov2_model->preprocess, &ov2_model->ov_model);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to update OV model\n");
            ret = DNN_GENERIC_ERROR;
            goto err;
        }
        ov_model_free(tmp_ov_model);
    }

    //get output port
    if (!ov2_model->output_port) {
        status = ov_model_const_output_by_index(ov2_model->ov_model, 0, &ov2_model->output_port);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get output port.\n");
            goto err;
        }
    }

    //compile network
    status = ov_core_compile_model(ov2_model->core, ov2_model->ov_model, "CPU", 0, &ov2_model->compiled_model);
    if (status != OK) {
        ret = DNN_GENERIC_ERROR;
        goto err;
    }

    //create infer_requests
    if (ctx->options.nireq <= 0) {
        ctx->options.nireq = av_cpu_count() / 2 + 1;
    }
    ov2_model->request_queue = ff_safe_queue_create();
    if (!ov2_model->request_queue) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    for (int i = 0; i < ctx->options.nireq; i++) {
        OV2RequestItem *item = av_mallocz(sizeof(*item));
        if (!item) {
            ret = AVERROR(ENOMEM);
            goto err;
        }

        item->callback.callback_func = infer_completion_callback;
        item->callback.args = item;
        if (ff_safe_queue_push_back(ov2_model->request_queue, item) < 0) {
            av_freep(&item);
            ret = AVERROR(ENOMEM);
            goto err;
        }

        status = ov_compiled_model_create_infer_request(ov2_model->compiled_model, &item->infer_request);
        item->lltasks = av_malloc_array(ctx->options.batch_size, sizeof(*item->lltasks));
        if (!item->lltasks) {
            ret = AVERROR(ENOMEM);
            goto err;
        }
        item->lltask_count = 0;
    }

    ov2_model->task_queue = ff_queue_create();
    if (!ov2_model->task_queue) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    ov2_model->lltask_queue = ff_queue_create();
    if (!ov2_model->lltask_queue) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    return 0;


err:
    ff_dnn_free_model_ov2(&ov2_model->model);
    return ret;
}

static int get_output_ov2(void *model, const char *input_name, int input_width, int input_height, const char *output_name, int *output_width, int *output_height)
{
    int ret;
    ov_dimension_t dims[4] = {{1, 1}, {1, 1}, {input_height, input_height}, {input_width, input_width}};
    OV2Model *ov2_model = model;
    OV2Context *ctx = &ov2_model->ctx;
    TaskItem task;
    OV2RequestItem *request;
    ov_status_e status;
    ov_shape_t input_shape = {0};
    ov_partial_shape_t partial_shape;
    DNNExecBaseParams exec_params = {
        .input_name   = input_name,
        .output_names = &output_name,
        .nb_output    = 1,
        .in_frame     = NULL,
        .out_frame    = NULL,
    };

    if (ov2_model->model->func_type != DFT_PROCESS_FRAME) {
        av_log(NULL, AV_LOG_ERROR, "Get output dim only when processing frame.\n");
        return AVERROR(EINVAL);
    }

    if (ctx->options.input_resizable) {
        if (!ov_model_is_dynamic(ov2_model->ov_model)) {
            status = ov_partial_shape_create(4, dims, &partial_shape);
            status = ov_const_port_get_shape(ov2_model->input_port, &input_shape);
            input_shape.dims[2] = input_height;
            input_shape.dims[3] = input_width;
            if (status != OK) {
                av_log(ctx, AV_LOG_ERROR, "Failed create shape for model input resize.\n");
                goto err;
            }

            status = ov_shape_to_partial_shape(input_shape, &partial_shape);
            if (status != OK) {
                av_log(ctx, AV_LOG_ERROR, "Failed create partial shape for model input resize.\n");
                goto err;
            }

            status = ov_model_reshape_single_input(ov2_model->ov_model, partial_shape);
            if (status != OK) {
                av_log(ctx, AV_LOG_ERROR, "Failed to reszie model input.\n");
                goto err;
            }
        } else {
            avpriv_report_missing_feature(ctx, "Do not support dynamic model.");
            goto err;
        }
    }

    status = ov_model_const_output_by_index(ov2_model->ov_model, 0, &ov2_model->output_port);
    if (status != OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get output port.\n");
        goto err;
    }

    init_model_ov2(ov2_model, input_name, output_name);

    ret = ff_dnn_fill_gettingoutput_task(&task, &exec_params, ov2_model, input_height, input_width, ctx);
    if (ret != 0) {
        goto err;
    }

    ret = extract_lltask_from_task(ov2_model->model->func_type, &task, ov2_model->lltask_queue, NULL);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "unable to extract inference from task.\n");
        goto err;
    }

    request = ff_safe_queue_pop_front(ov2_model->request_queue);
    if (!request) {
        av_log(ctx, AV_LOG_ERROR, "unable to get infer request.\n");
        ret = AVERROR(EINVAL);
        goto err;
    }

    ret = execute_model_ov2(request, ov2_model->lltask_queue);
    *output_width = task.out_frame->width;
    *output_height = task.out_frame->height;


err:
    av_frame_free(&task.out_frame);
    av_frame_free(&task.in_frame);
    return ret;
}

DNNModel *ff_dnn_load_model_ov2(const char *model_filename, DNNFunctionType func_type, const char *options, AVFilterContext *filter_ctx)
{
    DNNModel *dnn_model = NULL;
    OV2Model *ov2_model = NULL;
    OV2Context *ctx = NULL;
    ov_core_t* core = NULL;
    ov_model_t* ov_model = NULL;
    ov_status_e status;

    dnn_model = av_mallocz(sizeof(DNNModel));
    if (!dnn_model) {
        return NULL;
    }
    ov2_model = av_mallocz(sizeof(OV2Model));
    if (!ov2_model) {
        av_freep(&dnn_model);
        return NULL; 
    }
    dnn_model->model = ov2_model;
    ov2_model->model = dnn_model;
    ov2_model->ctx.class = &dnn_openvino2_class;
    ctx = &ov2_model->ctx;

    av_opt_set_defaults(ctx);
    if (av_opt_set_from_string(ctx, options, NULL, "=", "&") < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to parse options \"%s\"\n", options);
        goto err;
    }
    status = ov_core_create(&core);
    if (status != OK) {
        goto err;
    }

    status = ov_core_read_model(core, model_filename, NULL, &ov_model);
    if (status != OK) {
        ov_version_t ver;
        status = ov_get_openvino_version(&ver);
        av_log(NULL, AV_LOG_ERROR, "Failed to read the network from model file %s,\n"
                                  "Please check if the model version matches the runtime OpenVINO Version:\n",
                                   model_filename);
        if (status == OK) {
            av_log(NULL, AV_LOG_ERROR, "BuildNumber: %s\n", ver.buildNumber);
        }
        ov_version_free(&ver);
        goto err;
    }
    ov2_model->ov_model = ov_model;
    ov2_model->core     = core;

    dnn_model->get_input = &get_input_ov2;
    dnn_model->get_output = &get_output_ov2;
    dnn_model->options = options;
    dnn_model->filter_ctx = filter_ctx;
    dnn_model->func_type = func_type;
    return dnn_model;

err:
    ff_dnn_free_model_ov2(&dnn_model);
    return NULL;
}

int ff_dnn_execute_model_ov2(const DNNModel *model, DNNExecBaseParams *exec_params) {
    OV2Model *ov2_model = model->model;
    OV2Context *ctx = &ov2_model->ctx;
    OV2RequestItem *request;
    TaskItem *task;
    int ret;

    ret = ff_check_exec_params(ctx, DNN_OV2, model->func_type, exec_params);
    if (ret != 0) {
        return ret;
    }

    if (!ov2_model->compiled_model) {
        ret = init_model_ov2(ov2_model, exec_params->input_name, exec_params->output_names[0]);
        if (ret != 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed init OpenVINO exectuable network or inference request\n");
            return ret;
        }
    }

    task = av_malloc(sizeof(*task));
    if (!task) {
        av_log(ctx, AV_LOG_ERROR, "unable to alloc memory for task item.\n");
        return AVERROR(ENOMEM);
    }

    ret = ff_dnn_fill_task(task, exec_params, ov2_model, ctx->options.async, 1);
    if (ret != 0) {
        av_freep(&task);
        return ret;
    }

    if (ff_queue_push_back(ov2_model->task_queue, task) < 0) {
        av_freep(&task);
        av_log(ctx, AV_LOG_ERROR, "unable to push back task_queue.\n");
        return AVERROR(ENOMEM);
    }

    ret = extract_lltask_from_task(model->func_type, task, ov2_model->lltask_queue, exec_params);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "unable to extract inference from task.\n");
        return ret;
    }

    if (ctx->options.async) {
        while (ff_queue_size(ov2_model->lltask_queue) >= ctx->options.batch_size) {
            request = ff_safe_queue_pop_front(ov2_model->request_queue);
            if (!request) {
                av_log(ctx, AV_LOG_ERROR, "unable to get infer request.\n");
                return AVERROR(EINVAL);
            }

            ret = execute_model_ov2(request, ov2_model->lltask_queue);
            if (ret != 0) {
                return ret;
            }
        }

        return 0;
    } else {
        if (model->func_type == DFT_ANALYTICS_CLASSIFY) {
            avpriv_report_missing_feature(ctx, "only support dnn_processing now");
            return AVERROR(ENOSYS);
        }

        if (ctx->options.batch_size > 1) {
            avpriv_report_missing_feature(ctx, "batch mode for sync execution");
            return AVERROR(ENOSYS);
        }

        request = ff_safe_queue_pop_front(ov2_model->request_queue);
        if (!request) {
            av_log(ctx, AV_LOG_ERROR, "unable to get infer request.\n");
            return AVERROR(EINVAL);
        }
        return execute_model_ov2(request, ov2_model->lltask_queue);
    }
    
    return 0;
}
DNNAsyncStatusType ff_dnn_get_result_ov2(const DNNModel *model, AVFrame **in, AVFrame **out) {
    OV2Model *ov2_model = model->model;
    return ff_dnn_get_result_common(ov2_model->task_queue, in, out);
}

static int get_datatype_size(DNNDataType dt)
{
    switch (dt)
    {
    case DNN_FLOAT:
        return sizeof(float);
    case DNN_UINT8:
        return sizeof(uint8_t);
    default:
        av_assert0(!"not supported yet.");
        return 1;
    }
}

static int fill_model_input_ov2(OV2Model *ov2_model, OV2RequestItem *request) {
    int64_t* dims;
    OV2Context *ctx = &ov2_model->ctx;
    ov_status_e status;
    ov_tensor_t* tensor = NULL;
    ov_shape_t input_shape = {0};
    ov_element_type_e tensor_type;
    DNNData input;
    LastLevelTaskItem *lltask;
    TaskItem *task;

    lltask = ff_queue_peek_front(ov2_model->lltask_queue);
    av_assert0(lltask);
    task = lltask->task;

    if (ov2_model->ctx.options.input_resizable) {
        dims = av_malloc(4 * sizeof(int64_t));
        if (dims == NULL) {
            av_log(ctx, AV_LOG_ERROR, "Failed to malloc memory to dims\n");
            return AVERROR(ENOMEM);;
        }
        dims[0] = 1;
        dims[1] = 1;
        dims[2] = task->in_frame->height;
        dims[3] = task->in_frame->width;
        ov_shape_create(4, dims, &input_shape);
    } else {
        if (ov2_model->input_port == NULL) {
            status = ov_model_const_input_by_name(ov2_model->ov_model, task->input_name, &ov2_model->input_port);
            if (status != OK) {
                av_log(ctx, AV_LOG_ERROR, "Failed to get input port shape.\n");
                return DNN_GENERIC_ERROR;
            }
        }
        status = ov_const_port_get_shape(ov2_model->input_port, &input_shape);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get input port shape.\n");
            return DNN_GENERIC_ERROR;
        }
        dims = input_shape.dims;
    }
    if (!ov_model_is_dynamic(ov2_model->ov_model)) {
        status = ov_port_get_element_type(ov2_model->input_port, &tensor_type);
        if (status != OK) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get input port data type.\n");
            return DNN_GENERIC_ERROR;
        }
    } else {
        avpriv_report_missing_feature(ctx, "Do not support dynamic model.");
        return DNN_GENERIC_ERROR;
    }
    input.height = dims[2];
    input.width = dims[3];
    input.channels = dims[1];
    input.dt = port_datatype_to_dnn_datatype(tensor_type);
    input.data = av_malloc(input.height * input.width * input.channels * get_datatype_size(input.dt));
    input.order = DCO_BGR;

    for (int i = 0; i < ctx->options.batch_size; ++i) {
        lltask = ff_queue_pop_front(ov2_model->lltask_queue);
        if (!lltask) {
            break;
        }
        request->lltasks[i] = lltask;
        request->lltask_count = i + 1;
        task = lltask->task;
        switch (ov2_model->model->func_type) {
        case DFT_PROCESS_FRAME:
            if (task->do_ioproc) {
                if (ov2_model->model->frame_pre_proc != NULL) {
                    ov2_model->model->frame_pre_proc(task->in_frame, &input, ov2_model->model->filter_ctx);
                } else {
                    ff_proc_from_frame_to_dnn(task->in_frame, &input, ctx);
                }
            }
            break;
        case DFT_ANALYTICS_DETECT:
            ff_frame_to_dnn_detect(task->in_frame, &input, ctx);
            break;
        case DFT_ANALYTICS_CLASSIFY:
            ff_frame_to_dnn_classify(task->in_frame, &input, lltask->bbox_index, ctx);
            break;
        default:
            av_assert0(!"should not reach here");
            break;
        }
        ov_tensor_create_from_host_ptr(tensor_type, input_shape, input.data, &tensor);
        ov_infer_request_set_input_tensor(request->infer_request, tensor);
        input.data = (uint8_t *)input.data
                     + input.width * input.height * input.channels * get_datatype_size(input.dt);
    }

    return 0;
}

static void infer_completion_callback(void *args)
{
    int64_t* dims;
    ov_status_e status;
    OV2RequestItem *request = args;
    LastLevelTaskItem *lltask = request->lltasks[0];
    TaskItem *task = lltask->task;
    OV2Model *ov2_model = task->model;
    SafeQueue *requestq = ov2_model->request_queue;
    ov_tensor_t *output_tensor;
    ov_shape_t output_shape = {0};
    ov_element_type_e tensor_type;
    DNNData output;
    OV2Context *ctx = &ov2_model->ctx;
    size_t output_size;

    status = ov_infer_request_get_output_tensor_by_index(request->infer_request, 0, &output_tensor);
    if (status != OK) {
        av_log(ctx, AV_LOG_ERROR,
               "Failed to get output tensor.");
        return;
    }

    status = ov_tensor_get_size(output_tensor, &output_size);
    if (status != OK) {
        av_log(ctx, AV_LOG_ERROR,
               "Failed to get output tensor size.");
        return;
    }

    status = ov_tensor_data(output_tensor, &output.data);
    if (status != OK) {
        av_log(ctx, AV_LOG_ERROR,
               "Failed to get output data.");
        return;
    }

    status = ov_tensor_get_shape(output_tensor, &output_shape);
    if (status != OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get output port shape.\n");
        return;
    }
    dims = output_shape.dims;

    status = ov_port_get_element_type(ov2_model->output_port, &tensor_type);
    if (status != OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get output port data type.\n");
        return;
    }

    output.channels = dims[1];
    output.height   = dims[2];
    output.width    = dims[3];
    output.dt       = port_datatype_to_dnn_datatype(tensor_type);

    av_assert0(request->lltask_count <= dims[0]);
    av_assert0(request->lltask_count >= 1);
    for (int i = 0; i < request->lltask_count; ++i) {
        task = request->lltasks[i]->task;

        switch (ov2_model->model->func_type) {
        case DFT_PROCESS_FRAME:
            if (task->do_ioproc) {
                if (ov2_model->model->frame_post_proc != NULL) {
                    ov2_model->model->frame_post_proc(task->out_frame, &output, ov2_model->model->filter_ctx);
                } else {
                    ff_proc_from_dnn_to_frame(task->out_frame, &output, ctx);
                }
            } else {
                task->out_frame->width = output.width;
                task->out_frame->height = output.height;
            }
            break;
        case DFT_ANALYTICS_DETECT:
            if (!ov2_model->model->detect_post_proc) {
                av_log(ctx, AV_LOG_ERROR, "detect filter needs to provide post proc\n");
                return;
            }
            ov2_model->model->detect_post_proc(task->in_frame, &output, 1, ov2_model->model->filter_ctx);
            break;
        case DFT_ANALYTICS_CLASSIFY:
            if (!ov2_model->model->classify_post_proc) {
                av_log(ctx, AV_LOG_ERROR, "classify filter needs to provide post proc\n");
                return;
            }
            ov2_model->model->classify_post_proc(task->in_frame, &output, request->lltasks[i]->bbox_index, ov2_model->model->filter_ctx);
            break;
        default:
            av_assert0(!"Not supported yet");
            break;
        }

        task->inference_done++;
        av_freep(&request->lltasks[i]);
        output.data = (uint8_t *)output.data
                      + output.width * output.height * output.channels * get_datatype_size(output.dt);
    }

    request->lltask_count = 0;
    if (ff_safe_queue_push_back(requestq, request) < 0) {
        av_freep(&request);
        av_log(ctx, AV_LOG_ERROR, "Failed to push back request_queue.\n");
        return;
    }

}

int ff_dnn_flush_ov2(const DNNModel *model) {
    OV2Model *ov2_model = model->model;
    OV2Context *ctx = &ov2_model->ctx;
    OV2RequestItem *request;
    ov_status_e status;
    int ret;

    if (ff_queue_size(ov2_model->lltask_queue) == 0) {
        // no pending task need to flush
        return 0;
    }

    request = ff_safe_queue_pop_front(ov2_model->request_queue);
    if (!request) {
        av_log(ctx, AV_LOG_ERROR, "unable to get infer request.\n");
        return AVERROR(EINVAL);
    }

    ret = fill_model_input_ov2(ov2_model, request);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to fill model input.\n");
        return ret;
    }

    status = ov_infer_request_infer(request->infer_request);
    if (status != OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to start sync inference for OV2\n");
        return DNN_GENERIC_ERROR;
    }

    return 0;
}

void ff_dnn_free_model_ov2(DNNModel **model) {
    if (*model){
        OV2Model *ov2_model = (*model)->model;
        while (ff_safe_queue_size(ov2_model->request_queue) != 0) {
            OV2RequestItem *item = ff_safe_queue_pop_front(ov2_model->request_queue);
            if (item && item->infer_request) {
                ov_infer_request_free(item->infer_request);
            }
            av_freep(&item->lltasks);
            av_freep(&item);
        }
        ff_safe_queue_destroy(ov2_model->request_queue);

        while (ff_queue_size(ov2_model->lltask_queue) != 0) {
            LastLevelTaskItem *item = ff_queue_pop_front(ov2_model->lltask_queue);
            av_freep(&item);
        }
        ff_queue_destroy(ov2_model->lltask_queue);

        while (ff_queue_size(ov2_model->task_queue) != 0) {
            TaskItem *item = ff_queue_pop_front(ov2_model->task_queue);
            av_frame_free(&item->in_frame);
            av_frame_free(&item->out_frame);
            av_freep(&item);
        }
        ff_queue_destroy(ov2_model->task_queue);

        if (ov2_model->preprocess)
            ov_preprocess_prepostprocessor_free(ov2_model->preprocess);
        if (ov2_model->input_info)
            ov_preprocess_input_info_free(ov2_model->input_info);
        if (ov2_model->output_info)
            ov_preprocess_output_info_free(ov2_model->output_info);
        if (ov2_model->input_port)
            ov_output_const_port_free(ov2_model->input_port);
        if (ov2_model->output_port)
            ov_output_const_port_free(ov2_model->output_port);
        if (ov2_model->compiled_model)
            ov_compiled_model_free(ov2_model->compiled_model);
        if (ov2_model->ov_model)
            ov_model_free(ov2_model->ov_model);
        if (ov2_model->core)
            ov_core_free(ov2_model->core);
        av_freep(&ov2_model);
        av_freep(model);
    }
}
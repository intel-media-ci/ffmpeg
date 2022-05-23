/*
 * Copyright (c) 2022
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
 * DNN Torch backend implementation.
 */

#include <torch/torch.h>
#include <torch/script.h>
#include "dnn_backend_torch.h"

extern "C" {
#include "dnn_io_proc.h"
#include "../internal.h"
#include "dnn_backend_common.h"
#include "libavutil/opt.h"
#include "queue.h"
#include "safe_queue.h"
}

typedef struct THOptions{
    char *device_name;
    c10::DeviceType device_type;
} THOptions;

typedef struct THContext {
    const AVClass *c_class;
    THOptions options;
} THContext;

typedef struct THModel {
    THContext ctx;
    DNNModel *model;
    torch::jit::Module jit_model;
    SafeQueue *request_queue;
    Queue *task_queue;
    Queue *lltask_queue;
} THModel;

typedef struct THInferRequest {
    torch::Tensor *output;
    torch::Tensor *input_tensor;
} THInferRequest;

typedef struct THRequestItem {
    THInferRequest *infer_request;
    LastLevelTaskItem *lltask;
    DNNAsyncExecModule exec_module;
} THRequestItem;


#define OFFSET(x) offsetof(THContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM
static const AVOption dnn_th_options[] = {
    { "device", "device to run model", OFFSET(options.device_name), AV_OPT_TYPE_STRING, { .str = "cpu" }, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(dnn_th);

static int execute_model_th(THRequestItem *request, Queue *lltask_queue);
static int th_start_inference(void *args);
static void infer_completion_callback(void *args);

static int extract_lltask_from_task(TaskItem *task, Queue *lltask_queue)
{
    THModel *th_model = (THModel *)task->model;
    THContext *ctx = &th_model->ctx;
    LastLevelTaskItem *lltask = (LastLevelTaskItem *)av_malloc(sizeof(*lltask));
    if (!lltask) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate memory for LastLevelTaskItem\n");
        return AVERROR(ENOMEM);
    }
    task->inference_todo = 1;
    task->inference_done = 0;
    lltask->task = task;
    if (ff_queue_push_back(lltask_queue, lltask) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to push back lltask_queue.\n");
        av_freep(&lltask);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int get_input_th(void *model, DNNData *input, const char *input_name)
{
    input->dt = DNN_FLOAT;
    input->order = DCO_RGB_PLANAR;
    input->height = -1;
    input->width = -1;
    input->channels = 3;
    return 0;
}

static int get_output_th(void *model, const char *input_name, int input_width, int input_height,
                                   const char *output_name, int *output_width, int *output_height)
{
    int ret = 0;
    THModel *th_model = (THModel*) model;
    THContext *ctx = &th_model->ctx;
    TaskItem task;
    THRequestItem *request;
    DNNExecBaseParams exec_params = {
        .input_name     = input_name,
        .output_names   = &output_name,
        .nb_output      = 1,
        .in_frame       = NULL,
        .out_frame      = NULL,
    };
    ret = ff_dnn_fill_gettingoutput_task(&task, &exec_params, th_model, input_height, input_width, ctx);
    if ( ret != 0) {
        goto err;
    }

    ret = extract_lltask_from_task(&task, th_model->lltask_queue);
    if ( ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "unable to extract last level task from task.\n");
        goto err;
    }

    request = (THRequestItem*) ff_safe_queue_pop_front(th_model->request_queue);
    if (!request) {
        av_log(ctx, AV_LOG_ERROR, "unable to get infer request.\n");
        ret = AVERROR(EINVAL);
        goto err;
    }

    ret = execute_model_th(request, th_model->lltask_queue);
    *output_width = task.out_frame->width;
    *output_height = task.out_frame->height;

err:
    av_frame_free(&task.out_frame);
    av_frame_free(&task.in_frame);
    return ret;
}

static void th_free_request(THInferRequest *request)
{
    if (!request)
        return;
    if (request->output) {
        delete(request->output);
        request->output = NULL;
    }
    if (request->input_tensor) {
        delete(request->input_tensor);
        request->input_tensor = NULL;
    }
    return;
}

static inline void destroy_request_item(THRequestItem **arg)
{
    THRequestItem *item;
    if (!arg || !*arg) {
        return;
    }
    item = *arg;
    th_free_request(item->infer_request);
    av_freep(&item->infer_request);
    av_freep(&item->lltask);
    ff_dnn_async_module_cleanup(&item->exec_module);
    av_freep(arg);
}

static THInferRequest *th_create_inference_request(void)
{
    THInferRequest *request = (THInferRequest *)av_malloc(sizeof(THInferRequest));
    if (!request) {
        return NULL;
    }
    request->input_tensor = NULL;
    request->output = NULL;
    return request;
}

DNNModel *ff_dnn_load_model_th(const char *model_filename, DNNFunctionType func_type, const char *options, AVFilterContext *filter_ctx)
{
    DNNModel *model = NULL;
    THModel *th_model = NULL;
    THRequestItem *item = NULL;
    THContext *ctx;

    model = (DNNModel *)av_mallocz(sizeof(DNNModel));
    if (!model) {
        return NULL;
    }

    th_model = (THModel *)av_mallocz(sizeof(THModel));
    if (!th_model) {
        av_freep(&model);
        return NULL;
    }

    th_model->ctx.c_class = &dnn_th_class;
    ctx = &th_model->ctx;
    //parse options
    av_opt_set_defaults(ctx);
    if (av_opt_set_from_string(ctx, options, NULL, "=", "&") < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to parse options \"%s\"\n", options);
        return NULL;
    }

    c10::Device device = c10::Device(ctx->options.device_name);
    if (device.is_cpu()) {
        ctx->options.device_type = torch::kCPU;
    } else if (device.is_xpu()) {
        if (!at::hasXPU()) {
            av_log(ctx, AV_LOG_ERROR, "No XPU device found\n");
            return NULL;
        }
        at::detail::getXPUHooks().initXPU();
        at::deviceExclusiveCheck();
        ctx->options.device_type = torch::kXPU;
    } else {
        av_log(ctx, AV_LOG_ERROR, "Not supported device:\"%s\"\n", ctx->options.device_name);
        goto fail;
    }

    try {
        th_model->jit_model = torch::jit::load(model_filename, device);
    } catch (const c10::Error& e) {
        av_log(ctx, AV_LOG_ERROR, "Failed to load torch model\n");
        goto fail;
    }

    th_model->request_queue = ff_safe_queue_create();
    if (!th_model->request_queue) {
        goto fail;
    }

    item = (THRequestItem *)av_mallocz(sizeof(THRequestItem));
    if (!item) {
        goto fail;
    }
    item->lltask = NULL;
    item->infer_request = th_create_inference_request();
    if (!item->infer_request) {
        av_log(NULL, AV_LOG_ERROR, "Failed to allocate memory for Torch inference request\n");
        goto fail;
    }
    item->exec_module.start_inference = &th_start_inference;
    item->exec_module.callback = &infer_completion_callback;
    item->exec_module.args = item;

    if (ff_safe_queue_push_back(th_model->request_queue, item) < 0) {
        goto fail;
    }

    th_model->task_queue = ff_queue_create();
    if (!th_model->task_queue) {
        goto fail;
    }

    th_model->lltask_queue = ff_queue_create();
    if (!th_model->lltask_queue) {
        goto fail;
    }

    th_model->model = model;
    model->model = th_model;
    model->get_input = &get_input_th;
    model->get_output = &get_output_th;
    model->options = NULL;
    model->filter_ctx = filter_ctx;
    model->func_type = func_type;
    return model;

fail:
    destroy_request_item(&item);
    ff_queue_destroy(th_model->task_queue);
    ff_queue_destroy(th_model->lltask_queue);
    ff_safe_queue_destroy(th_model->request_queue);
    av_freep(&th_model);
    av_freep(&model);
    av_freep(&item);
    return NULL;
}

static int fill_model_input_th(THModel *th_model, THRequestItem *request)
{
    LastLevelTaskItem *lltask = NULL;
    TaskItem *task = NULL;
    THInferRequest *infer_request = NULL;
    DNNData input;
    THContext *ctx = &th_model->ctx;
    int ret;

    lltask = (LastLevelTaskItem *)ff_queue_pop_front(th_model->lltask_queue);
    if (!lltask) {
        ret = AVERROR(EINVAL);
        goto err;
    }
    request->lltask = lltask;
    task = lltask->task;
    infer_request = request->infer_request;

    ret = get_input_th(th_model, &input, NULL);
    if ( ret != 0) {
        goto err;
    }

    input.height = task->in_frame->height;
    input.width = task->in_frame->width;
    input.data = malloc(input.height * input.width * 3 * sizeof(float));
    if (!input.data)
        return AVERROR(ENOMEM);
    infer_request->input_tensor = new torch::Tensor();
    infer_request->output = new torch::Tensor();

    switch (th_model->model->func_type) {
    case DFT_PROCESS_FRAME:
        if (task->do_ioproc) {
            if (th_model->model->frame_pre_proc != NULL) {
                th_model->model->frame_pre_proc(task->in_frame, &input, th_model->model->filter_ctx);
            } else {
                ff_proc_from_frame_to_dnn(task->in_frame, &input, ctx);
            }
        }
        break;
    default:
        avpriv_report_missing_feature(NULL, "model function type %d", th_model->model->func_type);
        break;
    }
    *infer_request->input_tensor = torch::from_blob(input.data, {1, 1, 3, input.height, input.width},
                                                    torch::kFloat32);
    if (infer_request->input_tensor->device() != ctx->options.device_type)
            *infer_request->input_tensor = infer_request->input_tensor->to(ctx->options.device_type);
    return 0;

err:
    th_free_request(infer_request);
    return ret;
}

static int th_start_inference(void *args)
{
    THRequestItem *request = (THRequestItem *)args;
    THInferRequest *infer_request = NULL;
    LastLevelTaskItem *lltask = NULL;
    TaskItem *task = NULL;
    THModel *th_model = NULL;
    THContext *ctx = NULL;
    std::vector<torch::jit::IValue> inputs;

    if (!request) {
        av_log(NULL, AV_LOG_ERROR, "THRequestItem is NULL\n");
        return AVERROR(EINVAL);
    }
    infer_request = request->infer_request;
    lltask = request->lltask;
    task = lltask->task;
    th_model = (THModel *)task->model;
    ctx = &th_model->ctx;

    if (!infer_request->input_tensor || !infer_request->output) {
        av_log(ctx, AV_LOG_ERROR, "input or output tensor is NULL\n");
        return DNN_GENERIC_ERROR;
    }
    inputs.push_back(*infer_request->input_tensor);

    auto parameters = th_model->jit_model.parameters();
    auto para = *(parameters.begin());

    *infer_request->output = th_model->jit_model.forward(inputs).toTensor();

    return 0;
}

static void infer_completion_callback(void *args) {
    THRequestItem *request = (THRequestItem*)args;
    LastLevelTaskItem *lltask = request->lltask;
    TaskItem *task = lltask->task;
    DNNData outputs;
    THInferRequest *infer_request = request->infer_request;
    THModel *th_model = (THModel *)task->model;
    torch::Tensor *output = infer_request->output;

    c10::IntArrayRef sizes = output->sizes();
    assert(sizes.size == 5);
    outputs.order = DCO_RGB_PLANAR;
    outputs.height = sizes.at(3);
    outputs.width = sizes.at(4);
    outputs.dt = DNN_FLOAT;
    outputs.channels = 3;

    switch (th_model->model->func_type) {
    case DFT_PROCESS_FRAME:
        if (task->do_ioproc) {
            //post process can only deal with CPU memory.
            if (output->device() != torch::kCPU)
                *output = output->to(torch::kCPU);
            outputs.data = output->data_ptr();
            if (th_model->model->frame_post_proc != NULL) {
                th_model->model->frame_post_proc(task->out_frame, &outputs, th_model->model->filter_ctx);
            } else {
                ff_proc_from_dnn_to_frame(task->out_frame, &outputs, &th_model->ctx);
            }
        } else {
            task->out_frame->width = outputs.width;
            task->out_frame->height = outputs.height;
        }
        break;
    default:
        avpriv_report_missing_feature(&th_model->ctx, "model function type %d", th_model->model->func_type);
        goto err;
    }
    task->inference_done++;
err:
    th_free_request(infer_request);

    if (ff_safe_queue_push_back(th_model->request_queue, request) < 0) {
        destroy_request_item(&request);
        av_log(&th_model->ctx, AV_LOG_ERROR, "Unable to push back request_queue when failed to start inference.\n");
    }
}

static int execute_model_th(THRequestItem *request, Queue *lltask_queue)
{
    THModel *th_model = NULL;
    LastLevelTaskItem *lltask;
    TaskItem *task = NULL;
    int ret = 0;

    if (ff_queue_size(lltask_queue) == 0) {
        destroy_request_item(&request);
        return 0;
    }

    lltask = (LastLevelTaskItem *)ff_queue_peek_front(lltask_queue);
    if (lltask == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Failed to get LastLevelTaskItem\n");
        ret = AVERROR(EINVAL);
        goto err;
    }
    task = lltask->task;
    th_model = (THModel *)task->model;

    ret = fill_model_input_th(th_model, request);
    if ( ret != 0) {
        goto err;
    }
    if (task->async) {
        avpriv_report_missing_feature(&th_model->ctx, "LibTorch async");
    } else {
        ret = th_start_inference((void *)(request));
        if (ret != 0) {
            goto err;
        }
        infer_completion_callback(request);
        return (task->inference_done == task->inference_todo) ? 0 : DNN_GENERIC_ERROR;
    }

err:
    th_free_request(request->infer_request);
    if (ff_safe_queue_push_back(th_model->request_queue, request) < 0) {
        destroy_request_item(&request);
    }
    return ret;
}

int ff_dnn_execute_model_th(const DNNModel *model, DNNExecBaseParams *exec_params)
{
    THModel *th_model = (THModel *)model->model;
    THContext *ctx = &th_model->ctx;
    TaskItem *task;
    THRequestItem *request;
    int ret = 0;

    ret = ff_check_exec_params(ctx, DNN_TH, model->func_type, exec_params);
    if (ret != 0) {
        return ret;
    }

    task = (TaskItem *)av_malloc(sizeof(TaskItem));
    if (!task) {
        av_log(ctx, AV_LOG_ERROR, "unable to alloc memory for task item.\n");
        return AVERROR(ENOMEM);
    }

    ret = ff_dnn_fill_task(task, exec_params, th_model, 0, 1);
    if (ret != 0) {
        av_freep(&task);
        av_log(ctx, AV_LOG_ERROR, "unable to fill task.\n");
        return ret;
    }

    ret = ff_queue_push_back(th_model->task_queue, task);
    if (ret < 0) {
        av_freep(&task);
        av_log(ctx, AV_LOG_ERROR, "unable to push back task_queue.\n");
        return ret;
    }

    ret = extract_lltask_from_task(task, th_model->lltask_queue);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "unable to extract last level task from task.\n");
        return ret;
    }

    request = (THRequestItem *)ff_safe_queue_pop_front(th_model->request_queue);
    if (!request) {
        av_log(ctx, AV_LOG_ERROR, "unable to get infer request.\n");
        return AVERROR(EINVAL);
    }

    return execute_model_th(request, th_model->lltask_queue);
}


int ff_dnn_flush_th(const DNNModel *model)
{
    THModel *th_model = (THModel *)model->model;
    THRequestItem *request;

    if (ff_queue_size(th_model->lltask_queue) == 0) {
        // no pending task need to flush
        return 0;
    }
    request = (THRequestItem *)ff_safe_queue_pop_front(th_model->request_queue);
    if (!request) {
        av_log(&th_model->ctx, AV_LOG_ERROR, "unable to get infer request.\n");
        return AVERROR(EINVAL);
    }

    return execute_model_th(request, th_model->lltask_queue);
}

DNNAsyncStatusType ff_dnn_get_result_th(const DNNModel *model, AVFrame **in, AVFrame **out)
{
    THModel *th_model = (THModel *)model->model;
    return ff_dnn_get_result_common(th_model->task_queue, in, out);
}

void ff_dnn_free_model_th(DNNModel **model)
{
    THModel *th_model;
    if(*model) {
        th_model = (THModel *) (*model)->model;
        while (ff_safe_queue_size(th_model->request_queue) != 0) {
            THRequestItem *item = (THRequestItem *)ff_safe_queue_pop_front(th_model->request_queue);
            destroy_request_item(&item);
        }
        ff_safe_queue_destroy(th_model->request_queue);

        while (ff_queue_size(th_model->lltask_queue) != 0) {
            LastLevelTaskItem *item = (LastLevelTaskItem *)ff_queue_pop_front(th_model->lltask_queue);
            av_freep(&item);
        }
        ff_queue_destroy(th_model->lltask_queue);

        while (ff_queue_size(th_model->task_queue) != 0) {
            TaskItem *item = (TaskItem *)ff_queue_pop_front(th_model->task_queue);
            av_frame_free(&item->in_frame);
            av_frame_free(&item->out_frame);
            av_freep(&item);
        }
    }
    av_freep(&th_model);
    av_freep(model);
}

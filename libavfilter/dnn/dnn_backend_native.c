/*
 * Copyright (c) 2018 Sergey Lavrushkin
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
 * DNN native backend implementation.
 */

#include "dnn_backend_native.h"
#include "libavutil/avassert.h"
#include "dnn_backend_native_layer_conv2d.h"
#include "dnn_backend_native_layers.h"
#include "dnn_io_proc.h"
#include "dnn_backend_common.h"

typedef struct NativeRequestItem {
    DnnOperand *operands;
    InferenceItem *inference;
} NativeRequestItem;

#define OFFSET(x) offsetof(NativeContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM
static const AVOption dnn_native_options[] = {
    { "conv2d_threads", "threads num for conv2d layer", OFFSET(options.conv2d_threads), AV_OPT_TYPE_INT,  { .i64 = 0 }, INT_MIN, INT_MAX, FLAGS },
    DNN_BACKEND_COMMON_OPTIONS
    { NULL },
};

static const AVClass dnn_native_class = {
    .class_name = "dnn_native",
    .item_name  = av_default_item_name,
    .option     = dnn_native_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_FILTER,
};

static DNNReturnType execute_model_native(NativeRequestItem *request, Queue *inference_queue);

static DnnOperand* copy_operands(NativeModel *native_model)
{
    DnnOperand *original, *duplicate;

    original = native_model->operands;
    duplicate = av_mallocz(native_model->operands_num * sizeof(DnnOperand));
    if (!duplicate) {
        return NULL;
    }

    for (int32_t i = 0; i < native_model->operands_num; ++i){
        DnnOperand *oprd, *src;

        oprd = &duplicate[i];
        src = &original[i];

        oprd->data = NULL;
        oprd->type = src->type;
        oprd->length = src->length;
        oprd->isNHWC = src->isNHWC;
        oprd->data_type = src->data_type;
        oprd->usedNumbersLeft = src->usedNumbersLeft;

        for (int32_t dim = 0; dim < 4; ++dim) {
            oprd->dims[dim] = src->dims[dim];
        }

        for (int32_t index = 0; index < 128; ++index) {
            oprd->name[index] = src->name[index];
        }
    }
    return duplicate;
}

static DNNReturnType extract_inference_from_task(TaskItem *task, Queue *inference_queue)
{
    NativeModel *native_model = task->model;
    NativeContext *ctx = &native_model->ctx;
    InferenceItem *inference = av_malloc(sizeof(*inference));

    if (!inference) {
        av_log(ctx, AV_LOG_ERROR, "Unable to allocate space for InferenceItem\n");
        return DNN_ERROR;
    }
    task->inference_todo = 1;
    task->inference_done = 0;
    inference->task = task;

    if (ff_queue_push_back(inference_queue, inference) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to push back inference_queue.\n");
        av_freep(&inference);
        return DNN_ERROR;
    }
    return DNN_SUCCESS;
}

static DNNReturnType get_input_native(void *model, DNNData *input, const char *input_name)
{
    NativeModel *native_model = model;
    NativeContext *ctx = &native_model->ctx;

    for (int i = 0; i < native_model->operands_num; ++i) {
        DnnOperand *oprd = &native_model->operands[i];
        if (strcmp(oprd->name, input_name) == 0) {
            if (oprd->type != DOT_INPUT) {
                av_log(ctx, AV_LOG_ERROR, "Found \"%s\" in model, but it is not input node\n", input_name);
                return DNN_ERROR;
            }
            input->dt = oprd->data_type;
            av_assert0(oprd->dims[0] == 1);
            input->height = oprd->dims[1];
            input->width = oprd->dims[2];
            input->channels = oprd->dims[3];
            return DNN_SUCCESS;
        }
    }

    // do not find the input operand
    av_log(ctx, AV_LOG_ERROR, "Could not find \"%s\" in model\n", input_name);
    return DNN_ERROR;
}

static DNNReturnType get_output_native(void *model, const char *input_name, int input_width, int input_height,
                                       const char *output_name, int *output_width, int *output_height)
{
    DNNReturnType ret;
    NativeModel *native_model = model;
    NativeContext *ctx = &native_model->ctx;
    AVFrame *in_frame = av_frame_alloc();
    AVFrame *out_frame = NULL;
    NativeRequestItem *request;
    TaskItem task;

    if (!in_frame) {
        av_log(ctx, AV_LOG_ERROR, "Could not allocate memory for input frame\n");
        return DNN_ERROR;
    }

    out_frame = av_frame_alloc();

    if (!out_frame) {
        av_log(ctx, AV_LOG_ERROR, "Could not allocate memory for output frame\n");
        av_frame_free(&in_frame);
        return DNN_ERROR;
    }

    in_frame->width = input_width;
    in_frame->height = input_height;

    task.do_ioproc = 0;
    task.async = 0;
    task.input_name = input_name;
    task.in_frame = in_frame;
    task.output_names = &output_name;
    task.out_frame = out_frame;
    task.model = native_model;
    task.nb_output = 1;

    if (extract_inference_from_task(&task, native_model->inference_queue) != DNN_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "unable to extract inference from task.\n");
        av_frame_free(&out_frame);
        av_frame_free(&in_frame);
        return DNN_ERROR;
    }

    request = ff_safe_queue_pop_front(native_model->request_queue);
    if (!request) {
        av_log(ctx, AV_LOG_ERROR, "unable to get infer request.\n");
        return DNN_ERROR;
    }
    ret = execute_model_native(request, native_model->inference_queue);
    *output_width = out_frame->width;
    *output_height = out_frame->height;
    av_frame_free(&out_frame);
    av_frame_free(&in_frame);
    return ret;
}

static DNNReturnType fill_model_input_native(NativeModel *native_model, NativeRequestItem *request) {
    NativeContext *ctx = &native_model->ctx;
    DNNData input;
    TaskItem *task = NULL;
    DnnOperand *oprd = NULL;
    InferenceItem *inference = NULL;

    inference = ff_queue_pop_front(native_model->inference_queue);
    if (!inference) {
        av_log(NULL, AV_LOG_ERROR, "Failed to get inference item\n");
        return DNN_ERROR;
    }
    task = inference->task;
    request->inference = inference;

    if (native_model->layers_num <= 0 || native_model->operands_num <= 0) {
        av_log(ctx, AV_LOG_ERROR, "No operands or layers in model\n");
        return DNN_ERROR;
    }

    for (int i = 0; i < native_model->operands_num; ++i) {
        oprd = &request->operands[i];
        if (strcmp(oprd->name, task->input_name) == 0) {
            if (oprd->type != DOT_INPUT) {
                av_log(ctx, AV_LOG_ERROR, "Found \"%s\" in model, but it is not input node\n", task->input_name);
                return DNN_ERROR;
            }
            break;
        }
        oprd = NULL;
    }
    if (!oprd) {
        av_log(ctx, AV_LOG_ERROR, "Could not find \"%s\" in model\n", task->input_name);
        return DNN_ERROR;
    }

    oprd->dims[1] = task->in_frame->height;
    oprd->dims[2] = task->in_frame->width;

    av_freep(&oprd->data);
    oprd->length = ff_calculate_operand_data_length(oprd);
    if (oprd->length <= 0) {
        av_log(ctx, AV_LOG_ERROR, "The input data length overflow\n");
        return DNN_ERROR;
    }
    oprd->data = av_malloc(oprd->length);
    if (!oprd->data) {
        av_log(ctx, AV_LOG_ERROR, "Failed to malloc memory for input data\n");
        return DNN_ERROR;
    }

    input.height = oprd->dims[1];
    input.width = oprd->dims[2];
    input.channels = oprd->dims[3];
    input.data = oprd->data;
    input.dt = oprd->data_type;
    if (task->do_ioproc) {
        if (native_model->model->frame_pre_proc != NULL) {
            native_model->model->frame_pre_proc(task->in_frame, &input, native_model->model->filter_ctx);
        } else {
            ff_proc_from_frame_to_dnn(task->in_frame, &input, ctx);
        }
    }

    if (task->nb_output != 1) {
        // currently, the filter does not need multiple outputs,
        // so we just pending the support until we really need it.
        avpriv_report_missing_feature(ctx, "multiple outputs");
        return DNN_ERROR;
    }
    return DNN_SUCCESS;
}

static void infer_completion_callback(void *args)
{
    NativeRequestItem *request = args;
    InferenceItem *inference = request->inference;
    TaskItem *task = inference->task;
    DNNData output;
    NativeModel *native_model = task->model;
    NativeContext *ctx = &native_model->ctx;

    for (uint32_t i = 0; i < task->nb_output; ++i) {
        DnnOperand *oprd = NULL;
        const char *output_name = task->output_names[i];
        for (int j = 0; j < native_model->operands_num; ++j) {
            if (strcmp(request->operands[j].name, output_name) == 0) {
                oprd = &request->operands[j];
                break;
            }
        }
        if (oprd == NULL) {
            av_log(ctx, AV_LOG_ERROR, "Could not find output in model\n");
            return;
        }

        output.data = oprd->data;
        output.height = oprd->dims[1];
        output.width = oprd->dims[2];
        output.channels = oprd->dims[3];
        output.dt = oprd->data_type;

        if (task->do_ioproc) {
            if (native_model->model->frame_post_proc != NULL) {
                native_model->model->frame_post_proc(task->out_frame, &output, native_model->model->filter_ctx);
            } else {
                ff_proc_from_dnn_to_frame(task->out_frame, &output, ctx);
            }
        } else {
            task->out_frame->width = output.width;
            task->out_frame->height = output.height;
        }
    }
    task->inference_done++;

    if (ff_safe_queue_push_back(native_model->request_queue, request) < 0) {
        av_free(&request->operands);
        av_freep(&request);
        av_log(ctx, AV_LOG_ERROR, "Failed to push back request_queue.\n");
    }
}

// Loads model and its parameters that are stored in a binary file with following structure:
// layers_num,layer_type,layer_parameterss,layer_type,layer_parameters...
// For CONV layer: activation_function, input_num, output_num, kernel_size, kernel, biases
// For DEPTH_TO_SPACE layer: block_size
DNNModel *ff_dnn_load_model_native(const char *model_filename, DNNFunctionType func_type, const char *options, AVFilterContext *filter_ctx)
{
#define DNN_NATIVE_MAGIC "FFMPEGDNNNATIVE"
    DNNModel *model = NULL;
    // sizeof - 1 to skip the terminating '\0' which is not written in the file
    char buf[sizeof(DNN_NATIVE_MAGIC) - 1];
    int version, header_size, major_version_expected = 1;
    NativeModel *native_model = NULL;
    AVIOContext *model_file_context;
    int file_size, dnn_size, parsed_size;
    int32_t layer;
    NativeContext *ctx = NULL;
    DNNLayerType layer_type;

    if (avio_open(&model_file_context, model_filename, AVIO_FLAG_READ) < 0){
        return NULL;
    }
    file_size = avio_size(model_file_context);

    model = av_mallocz(sizeof(DNNModel));
    if (!model){
        goto fail;
    }

    /**
     * check file header with string and version
     */
    if (avio_read(model_file_context, buf, sizeof(buf)) != sizeof(buf) ||
        memcmp(buf, DNN_NATIVE_MAGIC, sizeof(buf)))
        goto fail;
    dnn_size = sizeof(buf);

    version = (int32_t)avio_rl32(model_file_context);
    dnn_size += 4;
    if (version != major_version_expected) {
        goto fail;
    }

    // currently no need to check minor version
    version = (int32_t)avio_rl32(model_file_context);
    dnn_size += 4;
    header_size = dnn_size;

    native_model = av_mallocz(sizeof(NativeModel));
    if (!native_model){
        goto fail;
    }
    model->model = native_model;
    ctx = &native_model->ctx;

    ctx->class = &dnn_native_class;
    model->options = options;
    ctx->options.async = 0;
    if (av_opt_set_from_string(&native_model->ctx, model->options, NULL, "=", "&") < 0)
        goto fail;
    native_model->model = model;

    if (ctx->options.async) {
        av_log(ctx, AV_LOG_WARNING, "Async not supported. Rolling back to sync\n");
        native_model->ctx.options.async = 0;
    }

#if !HAVE_PTHREAD_CANCEL
    if (ctx->options.conv2d_threads > 1){
        av_log(ctx, AV_LOG_WARNING, "'conv2d_threads' option was set but it is not supported "
                       "on this build (pthread support is required)\n");
    }
#endif

    avio_seek(model_file_context, file_size - 8, SEEK_SET);
    native_model->layers_num = (int32_t)avio_rl32(model_file_context);
    native_model->operands_num = (int32_t)avio_rl32(model_file_context);
    dnn_size += 8;
    avio_seek(model_file_context, header_size, SEEK_SET);

    native_model->layers = av_mallocz(native_model->layers_num * sizeof(Layer));
    if (!native_model->layers){
        goto fail;
    }

    native_model->operands = av_mallocz(native_model->operands_num * sizeof(DnnOperand));
    if (!native_model->operands){
        goto fail;
    }

    if (ctx->options.nireq <= 0) {
        ctx->options.nireq = av_cpu_count() / 2 + 1;
    }
    native_model->request_queue = ff_safe_queue_create();
    if (!native_model->request_queue) {
        goto fail;
    }

    native_model->task_queue = ff_queue_create();
    if (!native_model->task_queue) {
        goto fail;
    }

    native_model->inference_queue = ff_queue_create();
    if (!native_model->inference_queue) {
        goto fail;
    }

    for (layer = 0; layer < native_model->layers_num; ++layer){
        layer_type = (int32_t)avio_rl32(model_file_context);
        dnn_size += 4;

        if (layer_type >= DLT_COUNT) {
            goto fail;
        }

        native_model->layers[layer].type = layer_type;
        parsed_size = ff_layer_funcs[layer_type].pf_load(&native_model->layers[layer], model_file_context, file_size, native_model->operands_num);
        if (!parsed_size) {
            goto fail;
        }
        dnn_size += parsed_size;
    }

    for (int32_t i = 0; i < native_model->operands_num; ++i){
        DnnOperand *oprd;
        int32_t name_len;
        int32_t operand_index = (int32_t)avio_rl32(model_file_context);
        dnn_size += 4;

        if (operand_index >= native_model->operands_num) {
            goto fail;
        }

        oprd = &native_model->operands[operand_index];
        name_len = (int32_t)avio_rl32(model_file_context);
        dnn_size += 4;

        avio_get_str(model_file_context, name_len, oprd->name, sizeof(oprd->name));
        dnn_size += name_len;

        oprd->type = (int32_t)avio_rl32(model_file_context);
        dnn_size += 4;

        oprd->data_type = (int32_t)avio_rl32(model_file_context);
        dnn_size += 4;

        for (int32_t dim = 0; dim < 4; ++dim) {
            oprd->dims[dim] = (int32_t)avio_rl32(model_file_context);
            dnn_size += 4;
        }
        if (oprd->type == DOT_INPUT && oprd->dims[0] != 1)
            goto fail;

        oprd->isNHWC = 1;
    }

    avio_closep(&model_file_context);

    for (int i = 0; i < ctx->options.nireq; i++) {
        NativeRequestItem *item = av_mallocz(sizeof(*item));
        if (!item) {
            goto fail;
        }
        item->operands = copy_operands(native_model);
        if (!item->operands) {
            av_log(ctx, AV_LOG_ERROR, "Failed to allocate memory for operands in NativeRequestItem\n");
            av_freep(&item);
            goto fail;
        }
        if (ff_safe_queue_push_back(native_model->request_queue, item) < 0) {
            av_freep(&item->operands);
            av_freep(&item);
            goto fail;
        }
    }

    if (dnn_size != file_size){
        ff_dnn_free_model_native(&model);
        return NULL;
    }

    model->get_input = &get_input_native;
    model->get_output = &get_output_native;
    model->filter_ctx = filter_ctx;
    model->func_type = func_type;

    return model;

fail:
    ff_dnn_free_model_native(&model);
    avio_closep(&model_file_context);
    return NULL;
}

static DNNReturnType execute_model_native(NativeRequestItem *request, Queue *inference_queue)
{
    NativeModel *native_model = NULL;
    NativeContext *ctx = NULL;
    int32_t layer;
    InferenceItem *inference = NULL;
    TaskItem *task = NULL;

    inference = ff_queue_peek_front(inference_queue);
    if (!inference) {
        av_log(NULL, AV_LOG_ERROR, "Failed to get inference item\n");
        return DNN_ERROR;
    }
    task = inference->task;
    native_model = task->model;
    ctx = &native_model->ctx;

    if (fill_model_input_native(native_model, request) != DNN_SUCCESS) {
        return DNN_ERROR;
    }

    for (layer = 0; layer < native_model->layers_num; ++layer){
        DNNLayerType layer_type = native_model->layers[layer].type;
        if (ff_layer_funcs[layer_type].pf_exec(request->operands,
                                            native_model->layers[layer].input_operand_indexes,
                                            native_model->layers[layer].output_operand_index,
                                            native_model->layers[layer].params,
                                            &native_model->ctx) == DNN_ERROR) {
            av_log(ctx, AV_LOG_ERROR, "Failed to execute model\n");
            return DNN_ERROR;
        }
    }
    infer_completion_callback(request);

    return (task->inference_done == task->inference_todo) ? DNN_SUCCESS : DNN_ERROR;
}

DNNReturnType ff_dnn_execute_model_native(const DNNModel *model, DNNExecBaseParams *exec_params)
{
    NativeModel *native_model = model->model;
    NativeContext *ctx = &native_model->ctx;
    NativeRequestItem *request;
    TaskItem *task;

    if (ff_check_exec_params(ctx, DNN_NATIVE, model->func_type, exec_params) != 0) {
        return DNN_ERROR;
    }

    task = av_malloc(sizeof(*task));
    if (!task) {
        av_log(ctx, AV_LOG_ERROR, "unable to alloc memory for task item.\n");
        return DNN_ERROR;
    }

    if (ff_dnn_fill_task(task, exec_params, native_model, ctx->options.async, 1) != DNN_SUCCESS) {
        av_freep(&task);
        return DNN_ERROR;
    }

    if (ff_queue_push_back(native_model->task_queue, task) < 0) {
        av_freep(&task);
        av_log(ctx, AV_LOG_ERROR, "unable to push back task_queue.\n");
        return DNN_ERROR;
    }

    if (extract_inference_from_task(task, native_model->inference_queue) != DNN_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "unable to extract inference from task.\n");
        return DNN_ERROR;
    }

    request = ff_safe_queue_pop_front(native_model->request_queue);
    if (!request) {
        av_log(ctx, AV_LOG_ERROR, "unable to get infer request.\n");
        return DNN_ERROR;
    }
    return execute_model_native(request, native_model->inference_queue);
}

DNNReturnType ff_dnn_flush_native(const DNNModel *model)
{
    NativeModel *native_model = model->model;
    NativeRequestItem *request;

    if (ff_queue_size(native_model->inference_queue) == 0) {
        // no pending task need to flush
        return DNN_SUCCESS;
    }

    request = ff_safe_queue_pop_front(native_model->request_queue);
    if (!request) {
        av_log(NULL, AV_LOG_ERROR, "unable to get infer request.\n");
        return DNN_ERROR;
    }
    // for now, use sync node with flush operation
    // Switch to async when it is supported
    return execute_model_native(request, native_model->inference_queue);
}

DNNAsyncStatusType ff_dnn_get_result_native(const DNNModel *model, AVFrame **in, AVFrame **out)
{
    NativeModel *native_model = model->model;
    return dnn_get_async_result(native_model->task_queue, in, out);
}

int32_t ff_calculate_operand_dims_count(const DnnOperand *oprd)
{
    int32_t result = 1;
    for (int i = 0; i < 4; ++i)
        result *= oprd->dims[i];

    return result;
}

int32_t ff_calculate_operand_data_length(const DnnOperand* oprd)
{
    // currently, we just support DNN_FLOAT
    uint64_t len = sizeof(float);
    for (int i = 0; i < 4; i++) {
        len *= oprd->dims[i];
        if (len > INT32_MAX)
            return 0;
    }
    return len;
}

void ff_dnn_free_model_native(DNNModel **model)
{
    NativeModel *native_model;
    ConvolutionalParams *conv_params;
    int32_t layer;

    if (*model)
    {
        if ((*model)->model) {
            native_model = (*model)->model;
            if (native_model->layers) {
                for (layer = 0; layer < native_model->layers_num; ++layer){
                    if (native_model->layers[layer].type == DLT_CONV2D){
                        conv_params = (ConvolutionalParams *)native_model->layers[layer].params;
                        av_freep(&conv_params->kernel);
                        av_freep(&conv_params->biases);
                    }
                    av_freep(&native_model->layers[layer].params);
                }
                av_freep(&native_model->layers);
            }

            if (native_model->operands) {
                for (uint32_t operand = 0; operand < native_model->operands_num; ++operand)
                    av_freep(&native_model->operands[operand].data);
                av_freep(&native_model->operands);
            }

            while (ff_safe_queue_size(native_model->request_queue) != 0) {
                NativeRequestItem *item = ff_safe_queue_pop_front(native_model->request_queue);
                av_freep(&item->operands);
                av_freep(&item);
            }
            ff_safe_queue_destroy(native_model->request_queue);

            while (ff_queue_size(native_model->inference_queue) != 0) {
                InferenceItem *item = ff_queue_pop_front(native_model->inference_queue);
                av_freep(&item);
            }
            ff_queue_destroy(native_model->inference_queue);

            while (ff_queue_size(native_model->task_queue) != 0) {
                TaskItem *item = ff_queue_pop_front(native_model->task_queue);
                av_frame_free(&item->in_frame);
                av_frame_free(&item->out_frame);
                av_freep(&item);
            }
            ff_queue_destroy(native_model->task_queue);

            av_freep(&native_model);
        }
        av_freep(model);
    }
}

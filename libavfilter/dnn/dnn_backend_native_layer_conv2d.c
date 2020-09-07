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

#include "libavutil/avassert.h"
#include "libavutil/thread.h"
#include "libavutil/cpu.h"
#include "dnn_backend_native_layer_conv2d.h"

#define CLAMP_TO_EDGE(x, w) ((x) < 0 ? 0 : ((x) >= (w) ? (w - 1) : (x)))

//struct to pass parameters
typedef struct thread_common_param{
    DnnOperand *operands;
    const int32_t *input_operand_indexes;
    int32_t output_operand_index;
    const void *parameters;
    NativeContext *ctx;
    int thread_num;
} thread_common_param;

typedef struct thread_param{
    thread_common_param *thread_common_param;
    int thread_index;
} thread_param;

typedef struct execute_param{
    int thread_start, thread_end, input_num, output_num, kernel_size, padding_method, dilation;
    int pad_size, width, height, radius, src_linesize, filter_size, filter_linesize;
    float *input;
    float *output;
    float *kernel;
} execute_param;

void ff_dnn_execute_layer_conv2d_c(const execute_param *exe_param);

int dnn_load_layer_conv2d(Layer *layer, AVIOContext *model_file_context, int file_size, int operands_num)
{
    ConvolutionalParams *conv_params;
    int kernel_size;
    int dnn_size = 0;
    conv_params = av_malloc(sizeof(*conv_params));
    if (!conv_params)
        return 0;

    conv_params->dilation = (int32_t)avio_rl32(model_file_context);
    conv_params->padding_method = (int32_t)avio_rl32(model_file_context);
    conv_params->activation = (int32_t)avio_rl32(model_file_context);
    conv_params->input_num = (int32_t)avio_rl32(model_file_context);
    conv_params->output_num = (int32_t)avio_rl32(model_file_context);
    conv_params->kernel_size = (int32_t)avio_rl32(model_file_context);
    conv_params->has_bias = (int32_t)avio_rl32(model_file_context);
    dnn_size += 28;

    kernel_size = conv_params->input_num * conv_params->output_num *
                      conv_params->kernel_size * conv_params->kernel_size;
    dnn_size += kernel_size * 4;
    if (conv_params->has_bias)
        dnn_size += conv_params->output_num * 4;

    if (dnn_size > file_size || conv_params->input_num <= 0 ||
        conv_params->output_num <= 0 || conv_params->kernel_size <= 0){
        av_freep(&conv_params);
        return 0;
    }

    conv_params->kernel = av_malloc(kernel_size * sizeof(float));
    if (!conv_params->kernel) {
        av_freep(&conv_params);
        return 0;
    }
    for (int i = 0; i < kernel_size; ++i) {
        conv_params->kernel[i] = av_int2float(avio_rl32(model_file_context));
    }

    conv_params->biases = NULL;
    if (conv_params->has_bias) {
        conv_params->biases = av_malloc(conv_params->output_num * sizeof(float));
        if (!conv_params->biases){
            av_freep(&conv_params->kernel);
            av_freep(&conv_params);
            return 0;
        }
        for (int i = 0; i < conv_params->output_num; ++i){
            conv_params->biases[i] = av_int2float(avio_rl32(model_file_context));
        }
    }

    layer->params = conv_params;

    layer->input_operand_indexes[0] = (int32_t)avio_rl32(model_file_context);
    layer->output_operand_index = (int32_t)avio_rl32(model_file_context);
    dnn_size += 8;

    if (layer->input_operand_indexes[0] >= operands_num || layer->output_operand_index >= operands_num) {
        return 0;
    }

    return dnn_size;
}

void ff_dnn_execute_layer_conv2d_c(const execute_param *exe_param){
    int thread_start = exe_param->thread_start;
    int thread_end = exe_param->thread_end;
    float *input = exe_param->input;
    float *output = exe_param->output;
    float *kernel = exe_param->kernel;
    int input_num = exe_param->input_num;
    int output_num = exe_param->output_num;
    int kernel_size = exe_param->kernel_size;
    int padding_method = exe_param->padding_method;
    int dilation = exe_param->dilation;
    int pad_size = exe_param->pad_size;
    int width = exe_param->width;
    int height = exe_param->height;
    int radius = exe_param->radius;
    int src_linesize = exe_param->src_linesize;
    int filter_size = exe_param->filter_size;
    int filter_linesize = exe_param->filter_linesize;

    for (int y = thread_start; y < thread_end; ++y) {
        for (int x = pad_size; x < width - pad_size; ++x) {
            for (int n_filter = 0; n_filter < output_num; ++n_filter) {
                output[n_filter] = 0.0f;
                for (int ch = 0; ch < input_num; ++ch) {
                    for (int kernel_y = 0; kernel_y < kernel_size; ++kernel_y) {
                        for (int kernel_x = 0; kernel_x < kernel_size; ++kernel_x) {
                            float input_pel;
                            if (padding_method == SAME_CLAMP_TO_EDGE) {
                                int y_pos = CLAMP_TO_EDGE(y + (kernel_y - radius) * dilation, height);
                                int x_pos = CLAMP_TO_EDGE(x + (kernel_x - radius) * dilation, width);
                                input_pel = input[y_pos * src_linesize + x_pos * input_num + ch];
                            } else {
                                int y_pos = y + (kernel_y - radius) * dilation;
                                int x_pos = x + (kernel_x - radius) * dilation;
                                input_pel = (x_pos < 0 || x_pos >= width || y_pos < 0 || y_pos >= height) ? 0.0 :
                                                input[y_pos * src_linesize + x_pos * input_num + ch];
                            }

                            output[n_filter] += input_pel * kernel[n_filter * filter_size + kernel_y * filter_linesize +
                                                                                kernel_x * input_num + ch];
                        }
                    }
                }
            }
            output += output_num;
        }
    }
}

static void * dnn_execute_layer_conv2d_thread(void *threadarg)
{
    //pass parameters
    thread_param *thread_param = (struct thread_param *)threadarg;
    thread_common_param *thread_common_param = thread_param->thread_common_param;
    DnnOperand *operands = thread_common_param->operands;
    float *output;
    int32_t input_operand_index = thread_common_param->input_operand_indexes[0];
    int number = operands[input_operand_index].dims[0];
    int height = operands[input_operand_index].dims[1];
    int width = operands[input_operand_index].dims[2];
    int channel = operands[input_operand_index].dims[3];
    const float *input = operands[input_operand_index].data;
    const ConvolutionalParams *conv_params = (const ConvolutionalParams *)(thread_common_param->parameters);

    int radius = conv_params->kernel_size >> 1;
    int src_linesize = width * conv_params->input_num;
    int filter_linesize = conv_params->kernel_size * conv_params->input_num;
    int filter_size = conv_params->kernel_size * filter_linesize;
    int pad_size = (conv_params->padding_method == VALID) ? (conv_params->kernel_size - 1) / 2 * conv_params->dilation : 0;

    int thread_stride = (height - pad_size * 2) / thread_common_param->thread_num;
    int thread_start = thread_stride * thread_param->thread_index + pad_size;
    int thread_end = (thread_param->thread_index == thread_common_param->thread_num - 1) ? (height - pad_size) : (thread_start + thread_stride);

    DnnOperand *output_operand = &operands[thread_common_param->output_operand_index];
    output_operand->dims[0] = number;
    output_operand->dims[1] = height - pad_size * 2;
    output_operand->dims[2] = width - pad_size * 2;
    output_operand->dims[3] = conv_params->output_num;
    output_operand->data_type = operands[input_operand_index].data_type;
    output_operand->length = calculate_operand_data_length(output_operand);
    if (output_operand->length <= 0) {
        av_log(thread_common_param->ctx, AV_LOG_ERROR, "The output data length overflow\n");
        return (void *)DNN_ERROR;
    }
    output_operand->data = av_realloc(output_operand->data, output_operand->length);
    if (!output_operand->data) {
        av_log(thread_common_param->ctx, AV_LOG_ERROR, "Failed to reallocate memory for output\n");
        return (void *)DNN_ERROR;
    }

    output = output_operand->data;
    output += (conv_params->output_num) * (width - 2 * pad_size) * (thread_start - pad_size);

    av_assert0(channel == conv_params->input_num);

    struct execute_param exe_param;
    exe_param.thread_start = thread_start;
    exe_param.thread_end = thread_end;
    exe_param.input = input;
    exe_param.output = output;
    exe_param.kernel = conv_params->kernel;
    exe_param.input_num = conv_params->input_num;
    exe_param.output_num = conv_params->output_num;
    exe_param.kernel_size = conv_params->kernel_size;
    exe_param.padding_method = conv_params->padding_method;
    exe_param.dilation = conv_params->dilation;
    exe_param.pad_size = pad_size;
    exe_param.width = width;
    exe_param.height = height;
    exe_param.radius = radius;
    exe_param.src_linesize = src_linesize;
    exe_param.filter_size = filter_size;
    exe_param.filter_linesize = filter_linesize;

    ff_dnn_execute_layer_conv2d_c(&exe_param);

    output = output_operand->data;
    output += (conv_params->output_num) * (width - 2 * pad_size) * (thread_start - pad_size);
    for (int y = thread_start; y < thread_end; ++y) {
        for (int x = pad_size; x < width - pad_size; ++x) {
            for (int n_filter = 0; n_filter < conv_params->output_num; ++n_filter) {
                if (conv_params->has_bias)
                    output[n_filter] += conv_params->biases[n_filter];

                switch (conv_params->activation){
                case RELU:
                    output[n_filter] = FFMAX(output[n_filter], 0.0);
                    break;
                case TANH:
                    output[n_filter] = 2.0f  / (1.0f + exp(-2.0f * output[n_filter])) - 1.0f;
                    break;
                case SIGMOID:
                    output[n_filter] = 1.0f / (1.0f + exp(-output[n_filter]));
                    break;
                case NONE:
                    break;
                case LEAKY_RELU:
                    output[n_filter] = FFMAX(output[n_filter], 0.0) + 0.2 * FFMIN(output[n_filter], 0.0);
                }
            }
            output += conv_params->output_num;
        }
    }
    return (void *)DNN_SUCCESS;
}


int dnn_execute_layer_conv2d(DnnOperand *operands, const int32_t *input_operand_indexes,
                             int32_t output_operand_index, const void *parameters, NativeContext *ctx)
{
    int thread_num = (ctx->options.conv2d_threads <= 0 || ctx->options.conv2d_threads > av_cpu_count())
        ? (av_cpu_count() + 1) : (ctx->options.conv2d_threads);
#if HAVE_PTHREAD_CANCEL
    pthread_t *thread_id = av_malloc(thread_num * sizeof(pthread_t));
#endif
    thread_param **thread_param = av_malloc(thread_num * sizeof(*thread_param));
    void *res;
    int error_flag = DNN_SUCCESS;

    //struct used to pass parameters
    thread_common_param thread_common_param;
    thread_common_param.operands = operands;
    thread_common_param.input_operand_indexes = input_operand_indexes;
    thread_common_param.output_operand_index = output_operand_index;
    thread_common_param.parameters = parameters;
    thread_common_param.ctx = ctx;

#if HAVE_PTHREAD_CANCEL
    thread_common_param.thread_num = thread_num;

    //create threads
    for (int i = 0; i < thread_num; i++){
        thread_param[i] = av_malloc(sizeof(thread_param));
        thread_param[i]->thread_common_param = &thread_common_param;
        thread_param[i]->thread_index = i;
        pthread_create(&thread_id[i], NULL, dnn_execute_layer_conv2d_thread, (void *)thread_param[i]);
    }

    //join threads, res gets function return
    for (int i = 0; i < thread_num; i++){
        pthread_join(thread_id[i], &res);
        if ((int)res != DNN_SUCCESS)
            error_flag = (int)res;
    }

    //release memory
    av_free(thread_id);

    for (int i = 0; i < thread_num; i++){
        av_free(thread_param[i]);
    }
#else
    thread_common_param.thread_num = 1;
    thread_param[0] = av_malloc(sizeof(thread_param));
    thread_param[0]->thread_common_param = &thread_common_param;
    thread_param[0]->thread_index = 0;
    res = dnn_execute_layer_conv2d_thread((void *)thread_param[0]);
    if ((int)res != DNN_SUCCESS)
        error_flag = (int)res;
    av_free(thread_param[0]);
#endif

    av_free(thread_param);
    return error_flag;
}

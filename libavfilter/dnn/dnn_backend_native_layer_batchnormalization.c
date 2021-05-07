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
 * DNN native backend implementation.
 */

#include "dnn_backend_native.h"
#include "libavutil/avassert.h"
#include "dnn_backend_native_layer_batchnormalization.h"

int ff_dnn_load_layer_batchnormalization(Layer *layer, AVIOContext *model_file_context, int file_size, int operands_num)
{
    DnnLayerBatchnormalizationParams *params;
    int dnn_size = 0;
    params = av_malloc(sizeof(*params));
    if (!params)
        return 0;

    params->channel = avio_rl32(model_file_context);
    dnn_size += 4;

    params->mean = av_malloc_array(params->channel, sizeof(*params->mean));
    if (!params->mean) {
        av_freep(&params);
        return 0;
    }

    params->variance = av_malloc_array(params->channel, sizeof(*params->variance));
    if (!params->variance) {
        av_freep(&params->mean);
        av_freep(&params);
        return 0;
    }

    for (int32_t i = 0; i < params->channel; ++i) {
        params->mean[i] = av_int2float(avio_rl32(model_file_context));
    }
    for (int32_t i = 0; i < params->channel; ++i) {
        params->variance[i] = av_int2float(avio_rl32(model_file_context));
    }
    dnn_size += params->channel * 4 * 2;

    params->variance_eps = av_int2float(avio_rl32(model_file_context));
    params->scale = av_int2float(avio_rl32(model_file_context));
    params->offset = av_int2float(avio_rl32(model_file_context));
    dnn_size += 12;

    layer->params = params;
    layer->input_operand_indexes[0] = (int32_t)avio_rl32(model_file_context);
    layer->output_operand_index = (int32_t)avio_rl32(model_file_context);
    dnn_size += 8;
    if (layer->input_operand_indexes[0] >= operands_num || layer->output_operand_index >= operands_num) {
        return 0;
    }

    return dnn_size;
}

int ff_dnn_execute_layer_batchnormalization(DnnOperand *operands, const int32_t *input_operand_indexes,
                                            int32_t output_operand_index, const void *parameters, NativeContext *ctx)
{
    const DnnOperand *input = &operands[input_operand_indexes[0]];
    DnnOperand *output = &operands[output_operand_index];
    const DnnLayerBatchnormalizationParams *params = parameters;
    int dims_count;
    const float *src;
    float *dst;
    int32_t channel_num = params->channel;
    const float *mean = params->mean;
    const float *variance = params->variance;
    float variance_eps = params->variance_eps;
    float scale = params->scale;
    float offset = params->offset;

    if (params->channel != input->dims[3])
        return DNN_ERROR;

    for (int i = 0; i < 4; ++i)
        output->dims[i] = input->dims[i];

    output->data_type = input->data_type;
    output->length = ff_calculate_operand_data_length(output);
    if (output->length <= 0) {
        av_log(ctx, AV_LOG_ERROR, "The output data length overflow\n");
        return DNN_ERROR;
    }
    output->data = av_realloc(output->data, output->length);
    if (!output->data) {
        av_log(ctx, AV_LOG_ERROR, "Failed to reallocate memory for output\n");
        return DNN_ERROR;
    }

    dims_count = ff_calculate_operand_dims_count(output);
    src = input->data;
    dst = output->data;
    for (int i = 0; i < dims_count; ++i) {
        dst[i] = scale * (src[i] - mean[i % channel_num]) / sqrt(variance[i % channel_num] + variance_eps) + offset;
    }
    return 0;
}

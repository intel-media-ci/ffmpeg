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

#ifndef AVFILTER_DNN_DNN_BACKEND_NATIVE_LAYER_AVGPOOL_H
#define AVFILTER_DNN_DNN_BACKEND_NATIVE_LAYER_AVGPOOL_H

#include "dnn_backend_native.h"

typedef struct AvgPoolParams{
    int32_t strides, in_channels, out_channels, height, width;
    DNNActivationFunc activation;
    DNNPaddingParam padding_method;
} AvgPoolParams;

int dnn_load_layer_avg_pool(Layer *layer, AVIOContext *model_file_context, int file_size, int operands_num);
int dnn_execute_layer_avg_pool(DnnOperand *operands, const int32_t *input_operand_indexes,
                             int32_t output_operand_index, const void *parameters);

#endif

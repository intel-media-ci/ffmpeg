/*
 * Copyright (c) 2020
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

#ifndef AVFILTER_DNN_DNN_BACKEND_NATIVE_LAYER_DENSE_H
#define AVFILTER_DNN_DNN_BACKEND_NATIVE_LAYER_DENSE_H

#include "dnn_backend_native.h"

typedef struct DenseParams{
    int32_t input_num, output_num;
    DNNActivationFunc activation;
    int32_t has_bias;
    float *kernel;
    float *biases;
} DenseParams;

/**
 * @brief Load the Densely-Connnected Layer.
 *
 * It assigns the layer parameters to the hyperparameters
 * like activation, bias, and kernel size after parsing
 * from the model file context.
 *
 * @param layer pointer to the DNN layer instance
 * @param model_file_context pointer to model file context
 * @param file_size model file size
 * @param operands_num number of operands for the layer
 * @return Size of DNN Layer
 * @retval 0 if model file context contains invalid hyperparameters.
 */
int ff_dnn_load_layer_dense(Layer *layer, AVIOContext *model_file_context, int file_size, int operands_num);

/**
 * @brief Execute the Densely-Connnected Layer.
 *
 * @param operands input operands
 * @param input_operand_indexes input operand indexes
 * @param output_operand_index output operand index
 * @param parameters layer parameters
 * @param ctx pointer to Native model context
 * @retval DNN_SUCCESS if the execution succeeds
 * @retval DNN_ERROR if the execution fails
 */
int ff_dnn_execute_layer_dense(DnnOperand *operands, const int32_t *input_operand_indexes,
                               int32_t output_operand_index, const void *parameters, NativeContext *ctx);
#endif

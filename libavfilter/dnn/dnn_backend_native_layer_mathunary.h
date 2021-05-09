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

/**
 * @file
 * DNN inference functions interface for native backend.
 */

#ifndef AVFILTER_DNN_DNN_BACKEND_NATIVE_LAYER_MATHUNARY_H
#define AVFILTER_DNN_DNN_BACKEND_NATIVE_LAYER_MATHUNARY_H

#include "libavformat/avio.h"
#include "dnn_backend_native.h"

typedef enum {
    DMUO_ABS = 0,
    DMUO_SIN = 1,
    DMUO_COS = 2,
    DMUO_TAN = 3,
    DMUO_ASIN = 4,
    DMUO_ACOS = 5,
    DMUO_ATAN = 6,
    DMUO_SINH = 7,
    DMUO_COSH = 8,
    DMUO_TANH = 9,
    DMUO_ASINH = 10,
    DMUO_ACOSH = 11,
    DMUO_ATANH = 12,
    DMUO_CEIL = 13,
    DMUO_FLOOR = 14,
    DMUO_ROUND = 15,
    DMUO_EXP = 16,
    DMUO_COUNT
} DNNMathUnaryOperation;

typedef struct DnnLayerMathUnaryParams{
    DNNMathUnaryOperation un_op;
} DnnLayerMathUnaryParams;

/**
 * @brief Load the Unary Math Layer.
 *
 * It assigns the layer parameters to the unary operator
 * hyperparameter from the model file context.
 *
 * @param layer pointer to the DNN layer instance
 * @param model_file_context pointer to model file context
 * @param file_size model file size
 * @param operands_num number of operands for the layer
 * @return Size of DNN Layer
 * @retval 0 if model file context contains invalid hyperparameters.
 */
int ff_dnn_load_layer_math_unary(Layer *layer, AVIOContext *model_file_context, int file_size, int operands_num);

/**
 * @brief Execute the Unary Math Layer.
 *
 * It applies the unary operator parsed while
 * loading to the given input operands.
 *
 * @param operands input operands
 * @param input_operand_indexes input operand indexes
 * @param output_operand_index output operand index
 * @param parameters layer parameters
 * @param ctx pointer to Native model context
 * @retval DNN_SUCCESS if the execution succeeds
 * @retval DNN_ERROR if the execution fails
 */
int ff_dnn_execute_layer_math_unary(DnnOperand *operands, const int32_t *input_operand_indexes,
                                    int32_t output_operand_index, const void *parameters, NativeContext *ctx);

#endif

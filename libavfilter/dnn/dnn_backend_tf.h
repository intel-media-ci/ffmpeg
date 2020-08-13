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
 * DNN inference functions interface for TensorFlow backend.
 */


#ifndef AVFILTER_DNN_DNN_BACKEND_TF_H
#define AVFILTER_DNN_DNN_BACKEND_TF_H

#include "../dnn_interface.h"
#include "libavformat/avio.h"
#include <tensorflow/c/c_api.h>

typedef struct TFContext {
    const AVClass *class;
} TFContext;

typedef struct TFModel{
    TFContext ctx;
    TF_Graph *graph;
    TF_Session *session;
    TF_Status *status;
    TF_Output input;
    TF_Tensor *input_tensor;
    TF_Tensor **output_tensors;
    uint32_t nb_output;
} TFModel;

DNNModel *ff_dnn_load_model_tf(const char *model_filename, const char *options);

DNNReturnType ff_dnn_execute_model_tf(const DNNModel *model, DNNData *outputs, const char **output_names, uint32_t nb_output);

void ff_dnn_free_model_tf(DNNModel **model);

#endif

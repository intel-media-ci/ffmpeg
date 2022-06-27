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
 * DNN inference functions interface for Torch backend.
 */

#ifndef AVFILTER_DNN_DNN_BACKEND_TORCH_H
#define AVFILTER_DNN_DNN_BACKEND_TORCH_H


#ifdef __cplusplus
extern "C" {
#endif
#include "../dnn_interface.h"

DNNModel *ff_dnn_load_model_th(const char *model_filename, DNNFunctionType func_type, const char *options, AVFilterContext *filter_ctx);

int ff_dnn_execute_model_th(const DNNModel *model, DNNExecBaseParams *exec_params);
DNNAsyncStatusType ff_dnn_get_result_th(const DNNModel *model, AVFrame **in, AVFrame **out);
int ff_dnn_flush_th(const DNNModel *model);

void ff_dnn_free_model_th(DNNModel **model);

#ifdef __cplusplus
}
#endif

#endif

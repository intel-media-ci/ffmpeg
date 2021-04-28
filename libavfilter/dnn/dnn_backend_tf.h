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

/**
 * @brief Load model from the model.
 *
 * It allocates and initializes the DNN model
 * with the model parameters. If the graph cannot
 * be read by the TensorFlow C API, the function
 * tries loading it with native backend.
 *
 * @param model_filename name of the model file to load
 * @param func_type function type of model
 * @param options model execution options
 * @param filter_ctx filter context
 * @return The pointer to DNNModel instance of the loaded model
 * @retval NULL if the model cannot be loaded.
 * All allocated are freed by the function.
 */
DNNModel *ff_dnn_load_model_tf(const char *model_filename, DNNFunctionType func_type, const char *options, AVFilterContext *filter_ctx);

/**
 * @brief Execute TensorFlow model in synchronous mode.
 *
 * It parses the data from the input frame, preprocesses
 * the input frame data if required and fills the input
 * Tensor. Then it run the TensorFlow session and fills
 * the output after post processing it if required.
 *
 * Currently only single output is supported.
 *
 * @param model name of the model file to load
 * @param input_name function type of model
 * @param in_frame input frame
 * @param output_names TensorFlow operation names
 * @param nb_output number of outputs from the model
 * @param out_frame output frame
 * @return Status of model execution
 * @retval DNN_SUCCESS if the execution succeeds
 * @retval DNN_ERROR if the execution fails.
 * All allocated are freed by the function.
 */
DNNReturnType ff_dnn_execute_model_tf(const DNNModel *model, const char *input_name, AVFrame *in_frame,
                                      const char **output_names, uint32_t nb_output, AVFrame *out_frame);

/**
 * @brief Free the DNN model.
 *
 * If model isn't freed, it frees the TensorFlow
 * graph and status instances. It also closes and
 * deletes the TensorFlow session.
 *
 * @param model DNNModel instance of the loaded model
 */
void ff_dnn_free_model_tf(DNNModel **model);

#endif

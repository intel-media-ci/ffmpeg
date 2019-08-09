/*
 * Copyright (c) 2019 Guo Yejun
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

#include "dnn_filter_utils.h"
#include "libavutil/avassert.h"
#include "libavutil/common.h"

int copy_from_frame_to_dnn(DNNData *dnn_data, const AVFrame *in)
{
    // extend this function to support more formats
    av_assert0(in->format == AV_PIX_FMT_RGB24 || in->format == AV_PIX_FMT_RGB24);

    if (dnn_data->dt == DNN_FLOAT) {
        float *dnn_input = dnn_data->data;
        for (int i = 0; i < in->height; i++) {
            for(int j = 0; j < in->width * 3; j++) {
                int k = i * in->linesize[0] + j;
                int t = i * in->width * 3 + j;
                dnn_input[t] = in->data[0][k] / 255.0f;
            }
        }
    } else {
        uint8_t *dnn_input = dnn_data->data;
        av_assert0(dnn_data->dt == DNN_UINT8);
        for (int i = 0; i < in->height; i++) {
            for(int j = 0; j < in->width * 3; j++) {
                int k = i * in->linesize[0] + j;
                int t = i * in->width * 3 + j;
                dnn_input[t] = in->data[0][k];
            }
        }
    }

    return 0;
}

int copy_from_dnn_to_frame(AVFrame *out, const DNNData *dnn_data)
{
    // extend this function to support more formats
    av_assert0(out->format == AV_PIX_FMT_RGB24 || out->format == AV_PIX_FMT_RGB24);

    if (dnn_data->dt == DNN_FLOAT) {
        float *dnn_output = dnn_data->data;
        for (int i = 0; i < out->height; i++) {
            for(int j = 0; j < out->width * 3; j++) {
                int k = i * out->linesize[0] + j;
                int t = i * out->width * 3 + j;
                out->data[0][k] = av_clip((int)(dnn_output[t] * 255.0f), 0, 255);
            }
        }
    } else {
        uint8_t *dnn_output = dnn_data->data;
        av_assert0(dnn_data->dt == DNN_UINT8);
        for (int i = 0; i < out->height; i++) {
            for(int j = 0; j < out->width * 3; j++) {
                int k = i * out->linesize[0] + j;
                int t = i * out->width * 3 + j;
                out->data[0][k] = dnn_output[t];
            }
        }
    }

    return 0;
}
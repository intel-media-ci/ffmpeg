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

/**
 * @file
 * DNN filter utilities.
 */

#ifndef AVFILTER_DNN_FILTER_UTILS_H
#define AVFILTER_DNN_FILTER_UTILS_H

#include "dnn_interface.h"
#include "libavutil/frame.h"

int copy_from_frame_to_dnn(DNNData *dnn_data, const AVFrame *in);
int copy_from_dnn_to_frame(AVFrame *out, const DNNData *dnn_data);

#endif

/*
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

#ifndef AVFILTER_DNN_BBOX_H
#define AVFILTER_DNN_BBOX_H

#include "rational.h"

typedef struct AVDnnBoundingBox {
    /**
     * Must be set to the size of this data structure (that is,
     * sizeof(AVDnnBoundingBox)).
     */
    uint32_t self_size;

    /**
     * Object detection is usually applied to a smaller image that
     * is scaled down from the original frame.
     * width and height are attributes of the scaled image, in pixel.
     */
    int width;
    int height;

    /**
     * Distance in pixels from the top edge of the scaled image to top
     * and bottom, and from the left edge of the scaled image to left and
     * right, defining the bounding box.
     */
    int top;
    int left;
    int bottom;
    int right;

    /**
     * Detect result
     */
    uint32_t detect_label;
    AVRational confidence;

    /**
     * At most 4 classifications based on the detected bounding box.
     * For example, we can get max 4 different attributes with 4 different
     * DNN models on one bounding box.
     * classify_count is zero if no classification.
     */
    uint32_t classify_labels[4];
    AVRational classify_confidences[4];
    uint32_t classify_count;
} AVDnnBoundingBox;

#endif

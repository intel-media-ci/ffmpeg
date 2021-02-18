/*
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

#ifndef AVUTIL_BOUNDINGBOX_H
#define AVUTIL_BOUNDINGBOX_H

#include "libavutil/rational.h"

typedef struct AVBoundingBox {
    /**
     * Distance in pixels from the top edge of the frame to top
     * and bottom, and from the left edge of the frame to left and
     * right, defining the bounding box.
     */
    int top;
    int left;
    int bottom;
    int right;

#define AV_BBOX_LABEL_NAME_MAX_SIZE 32

    /**
     * Detect result with confidence
     */
    char detect_label[AV_BBOX_LABEL_NAME_MAX_SIZE];
    AVRational detect_confidence;

    /**
     * At most 4 classifications based on the detected bounding box.
     * For example, we can get max 4 different attributes with 4 different
     * DNN models on one bounding box.
     * classify_count is zero if no classification.
     */
#define AV_NUM_BBOX_CLASSIFY 4
    uint32_t classify_count;
    char classify_labels[AV_NUM_BBOX_CLASSIFY][AV_BBOX_LABEL_NAME_MAX_SIZE];
    AVRational classify_confidences[AV_NUM_BBOX_CLASSIFY];
} AVBoundingBox;

typedef struct AVBoundingBoxHeader {
    /**
     * Information about how the bounding box is generated.
     * for example, the DNN model name.
     */
    char source[128];

    /**
     * The size of frame when it is detected.
     */
    int frame_width;
    int frame_height;

    /**
     * Number of bounding boxes.
     */
    uint32_t nb_bbox;

    /**
     * Pointer to the array of AVBoundingBox.
     */
    AVBoundingBox bboxes[];
} AVBoundingBoxHeader;

#endif

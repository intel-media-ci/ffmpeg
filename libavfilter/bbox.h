/*
 * Copyright (c) 2005 Robert Edele <yartrebo@earthlink.net>
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

#ifndef AVFILTER_BBOX_H
#define AVFILTER_BBOX_H

#include <stdint.h>
#include "libavutil/rational.h"

typedef struct BoundingBoxHeader {
    /*
     * Information about how the bounding box is generated.
     * for example, the DNN model name.
     */
    char source[128];

    /* Must be set to the size of BoundingBox (that is,
     * sizeof(BoundingBox)).
     */
    uint32_t bbox_size;
} BoundingBoxHeader;

typedef struct BoundingBox {
    /**
     * Distance in pixels from the top edge of the frame to top
     * and bottom, and from the left edge of the frame to left and
     * right, defining the bounding box.
     */
    int top;
    int left;
    int bottom;
    int right;

#define BBOX_LABEL_NAME_MAX_LENGTH 32

    /**
     * Detect result with confidence
     */
    char detect_label[BBOX_LABEL_NAME_MAX_LENGTH+1];
    AVRational detect_confidence;

    /**
     * At most 4 classifications based on the detected bounding box.
     * For example, we can get max 4 different attributes with 4 different
     * DNN models on one bounding box.
     * classify_count is zero if no classification.
     */
#define AV_NUM_BBOX_CLASSIFY 4
    uint32_t classify_count;
    char classify_labels[AV_NUM_BBOX_CLASSIFY][BBOX_LABEL_NAME_MAX_LENGTH+1];
    AVRational classify_confidences[AV_NUM_BBOX_CLASSIFY];
} BoundingBox;

typedef struct FFBoundingBox {
    int x1, x2, y1, y2;
} FFBoundingBox;

/**
 * Calculate the smallest rectangle that will encompass the
 * region with values > min_val.
 *
 * @param bbox bounding box structure which is updated with the found values.
 *             If no pixels could be found with value > min_val, the
 *             structure is not modified.
 * @return 1 in case at least one pixel with value > min_val was found,
 *         0 otherwise
 */
int ff_calculate_bounding_box(FFBoundingBox *bbox,
                              const uint8_t *data, int linesize,
                              int w, int h, int min_val, int depth);

#endif /* AVFILTER_BBOX_H */

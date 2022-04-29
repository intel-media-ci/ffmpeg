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

#include "sub_frame_metadata.h"

static void sub_frame_free(void *opaque, uint8_t *data)
{
    AVFrame *frame = (AVFrame*)data;

    av_frame_free(&frame);
}

static AVFrame *sub_frame_alloc(size_t *out_size)
{
    AVFrame *sub_frame = av_frame_alloc();
    if (!sub_frame)
        return NULL;

    *out_size = sizeof(*sub_frame);

    return sub_frame;
}

AVFrame *av_sub_frame_create_side_data(AVFrame *frame)
{
    AVBufferRef *buf;
    AVFrame *sub_frame;
    size_t size;

    sub_frame = sub_frame_alloc(&size);
    if (!sub_frame)
        return NULL;

    buf = av_buffer_create((uint8_t *)sub_frame, size, &sub_frame_free, NULL, 0);
    if (!buf) {
        av_frame_free(&sub_frame);
        return NULL;
    }

    if (!av_frame_new_side_data_from_buf(frame, AV_FRAME_DATA_SUB_FRAME, buf)) {
        av_buffer_unref(&buf);
        return NULL;
    }

    return sub_frame;
}

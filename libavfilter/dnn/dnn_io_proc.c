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

#include "dnn_io_proc.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"
#include "libavutil/avassert.h"
#include "libavutil/detection_bbox.h"

DNNReturnType ff_proc_from_dnn_to_frame(AVFrame *frame, DNNData *output, void *log_ctx)
{
    struct SwsContext *sws_ctx;
    int frame_size = frame->height * frame->width;
    int linesize[3];
    linesize[0] = frame->linesize[0];
    void **dst_data, *tmp_data;
    dst_data = frame->data;

    int bytewidth = av_image_get_linesize(frame->format, frame->width, 0);
    if (bytewidth < 0) {
        return DNN_ERROR;
    }
    if (output->dt != DNN_FLOAT) {
        avpriv_report_missing_feature(log_ctx, "data type rather than DNN_FLOAT");
        return DNN_ERROR;
    }
    if (output->format == AV_PIX_FMT_RGBP) {
        tmp_data = malloc(frame_size * 3 * sizeof(uint8_t));
        if (!tmp_data) {
            av_log(log_ctx, AV_LOG_ERROR, "Failed to malloc memory for tmp_data for "
                    "the conversion fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                    av_get_pix_fmt_name(AV_PIX_FMT_GRAYF32),  frame->width, frame->height,
                    av_get_pix_fmt_name(AV_PIX_FMT_GRAY8),frame->width, frame->height);
            return DNN_ERROR;
        }
        dst_data = &tmp_data;
        linesize[0] = frame->width * 3;
    }

    switch (frame->format) {
    case AV_PIX_FMT_RGB24:
    case AV_PIX_FMT_BGR24:
        sws_ctx = sws_getContext(frame->width * 3,
                                 frame->height,
                                 AV_PIX_FMT_GRAYF32,
                                 frame->width * 3,
                                 frame->height,
                                 AV_PIX_FMT_GRAY8,
                                 0, NULL, NULL, NULL);
        if (!sws_ctx) {
            av_log(log_ctx, AV_LOG_ERROR, "Impossible to create scale context for the conversion "
                "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                av_get_pix_fmt_name(AV_PIX_FMT_GRAYF32), frame->width * 3, frame->height,
                av_get_pix_fmt_name(AV_PIX_FMT_GRAY8),   frame->width * 3, frame->height);
            return DNN_ERROR;
        }
        sws_scale(sws_ctx, (const uint8_t *[4]){(const uint8_t *)output->data, 0, 0, 0},
                           (const int[4]){frame->width * 3 * sizeof(float), 0, 0, 0}, 0, frame->height,
                           (uint8_t * const*)dst_data, linesize);
        sws_freeContext(sws_ctx);
        switch (output->format) {
        case AV_PIX_FMT_RGBP:
            sws_ctx = sws_getContext(frame->width,
                                     frame->height,
                                     AV_PIX_FMT_RGBP,
                                     frame->width,
                                     frame->height,
                                     frame->format,
                                     0, NULL, NULL, NULL);
            if (!sws_ctx) {
                av_log(log_ctx, AV_LOG_ERROR, "Impossible to create scale context for the conversion "
                       "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                       av_get_pix_fmt_name(AV_PIX_FMT_RGBP),  frame->width, frame->height,
                       av_get_pix_fmt_name(frame->format),frame->width, frame->height);
                return DNN_ERROR;
            }
            sws_scale(sws_ctx, (const uint8_t * const[4]){dst_data[0],
                                                          dst_data[0] + frame_size * sizeof(uint8_t),
                                                          dst_data[0] + frame_size * sizeof(uint8_t) * 2, 0},
                      (const int [4]){frame->width * sizeof(uint8_t),
                                      frame->width * sizeof(uint8_t),
                                      frame->width * sizeof(uint8_t), 0}
                      , 0, frame->height,
                      (const uint8_t **)frame->data, frame->linesize);
            break;
        default:
            break;
        }
        return DNN_SUCCESS;
    case AV_PIX_FMT_GRAYF32:
        av_image_copy_plane(frame->data[0], frame->linesize[0],
                            output->data, bytewidth,
                            bytewidth, frame->height);
        return DNN_SUCCESS;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUV410P:
    case AV_PIX_FMT_YUV411P:
    case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_NV12:
        sws_ctx = sws_getContext(frame->width,
                                 frame->height,
                                 AV_PIX_FMT_GRAYF32,
                                 frame->width,
                                 frame->height,
                                 AV_PIX_FMT_GRAY8,
                                 0, NULL, NULL, NULL);
        if (!sws_ctx) {
            av_log(log_ctx, AV_LOG_ERROR, "Impossible to create scale context for the conversion "
                "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                av_get_pix_fmt_name(AV_PIX_FMT_GRAYF32), frame->width, frame->height,
                av_get_pix_fmt_name(AV_PIX_FMT_GRAY8),   frame->width, frame->height);
            return DNN_ERROR;
        }
        sws_scale(sws_ctx, (const uint8_t *[4]){(const uint8_t *)output->data, 0, 0, 0},
                           (const int[4]){frame->width * sizeof(float), 0, 0, 0}, 0, frame->height,
                           (uint8_t * const*)frame->data, frame->linesize);
        sws_freeContext(sws_ctx);
        return DNN_SUCCESS;
    default:
        avpriv_report_missing_feature(log_ctx, "%s", av_get_pix_fmt_name(frame->format));
        return DNN_ERROR;
    }

    return DNN_SUCCESS;
}

DNNReturnType ff_proc_from_frame_to_dnn(AVFrame *frame, DNNData *input, void *log_ctx)
{
    struct SwsContext *sws_ctx;
    int bytewidth = av_image_get_linesize(frame->format, frame->width, 0);
    int frame_size = frame->height * frame->width;
    int linesize[3];
    linesize[0] = frame->linesize[0];
    void **src_data, *dst_data, *tmp_data = NULL;
    src_data = frame->data;
    dst_data = input->data;

    if (bytewidth < 0) {
        return DNN_ERROR;
    }
    if (input->dt != DNN_FLOAT) {
        avpriv_report_missing_feature(log_ctx, "data type rather than DNN_FLOAT");
        return DNN_ERROR;
    }

    switch (frame->format) {
    case AV_PIX_FMT_RGB24:
    case AV_PIX_FMT_BGR24:
        switch (input->format) {
        case AV_PIX_FMT_RGBP:
            tmp_data = av_malloc(frame_size * 3 * sizeof(uint8_t));
            if (!tmp_data) {
                av_log(log_ctx, AV_LOG_ERROR, "Failed to malloc memory for tmp_data for "
                       "the conversion fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                       av_get_pix_fmt_name(frame->format),  frame->width, frame->height,
                       av_get_pix_fmt_name(AV_PIX_FMT_RGBP),frame->width, frame->height);
                return DNN_ERROR;
            }
            sws_ctx = sws_getContext(frame->width,
                                     frame->height,
                                     frame->format,
                                     frame->width,
                                     frame->height,
                                     AV_PIX_FMT_RGBP,
                                     0, NULL, NULL, NULL);
            if (!sws_ctx) {
                av_log(log_ctx, AV_LOG_ERROR, "Impossible to create scale context for the conversion "
                       "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                       av_get_pix_fmt_name(frame->format),  frame->width, frame->height,
                       av_get_pix_fmt_name(AV_PIX_FMT_RGBP),frame->width, frame->height);
                return DNN_ERROR;
            }
            uint8_t *data = input->data;
            sws_scale(sws_ctx, (const uint8_t **)frame->data,
                      frame->linesize, 0, frame->height,
                      (uint8_t * const [4]){tmp_data,
                                            tmp_data + frame_size * sizeof(uint8_t),
                                            tmp_data + frame_size * sizeof(uint8_t) * 2, 0},
                      (const int [4]){frame->width * sizeof(uint8_t),
                                      frame->width * sizeof(uint8_t),
                                      frame->width * sizeof(uint8_t), 0});
            sws_freeContext(sws_ctx);
            src_data = &tmp_data;
            linesize[0] = frame->width * 3;
            break;
        default:
            break;
        }
        sws_ctx = sws_getContext(frame->width * 3,
                                 frame->height,
                                 AV_PIX_FMT_GRAY8,
                                 frame->width * 3,
                                 frame->height,
                                 AV_PIX_FMT_GRAYF32,
                                 0, NULL, NULL, NULL);
        if (!sws_ctx) {
            av_log(log_ctx, AV_LOG_ERROR, "Impossible to create scale context for the conversion "
                "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                av_get_pix_fmt_name(AV_PIX_FMT_GRAY8),  frame->width * 3, frame->height,
                av_get_pix_fmt_name(AV_PIX_FMT_GRAYF32),frame->width * 3, frame->height);
            av_freep(tmp_data);
            return DNN_ERROR;
        }
        sws_scale(sws_ctx, (const uint8_t **)src_data,
                           linesize, 0, frame->height,
                           (uint8_t * const [4]){input->data, 0, 0, 0},
                           (const int [4]){frame->width * 3 * sizeof(float), 0, 0, 0});
        sws_freeContext(sws_ctx);
        av_freep(&tmp_data);
        break;
    case AV_PIX_FMT_GRAYF32:
        av_image_copy_plane(input->data, bytewidth,
                            frame->data[0], frame->linesize[0],
                            bytewidth, frame->height);
        break;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUV410P:
    case AV_PIX_FMT_YUV411P:
    case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_NV12:
        sws_ctx = sws_getContext(frame->width,
                                 frame->height,
                                 AV_PIX_FMT_GRAY8,
                                 frame->width,
                                 frame->height,
                                 AV_PIX_FMT_GRAYF32,
                                 0, NULL, NULL, NULL);
        if (!sws_ctx) {
            av_log(log_ctx, AV_LOG_ERROR, "Impossible to create scale context for the conversion "
                "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                av_get_pix_fmt_name(AV_PIX_FMT_GRAY8),  frame->width, frame->height,
                av_get_pix_fmt_name(AV_PIX_FMT_GRAYF32),frame->width, frame->height);
            return DNN_ERROR;
        }
        sws_scale(sws_ctx, (const uint8_t **)frame->data,
                           frame->linesize, 0, frame->height,
                           (uint8_t * const [4]){input->data, 0, 0, 0},
                           (const int [4]){frame->width * sizeof(float), 0, 0, 0});
        sws_freeContext(sws_ctx);
        break;
    default:
        avpriv_report_missing_feature(log_ctx, "%s", av_get_pix_fmt_name(frame->format));
        return DNN_ERROR;
    }

    return DNN_SUCCESS;
}

DNNReturnType ff_frame_to_dnn_classify(AVFrame *frame, DNNData *input, uint32_t bbox_index, void *log_ctx)
{
    const AVPixFmtDescriptor *desc;
    int offsetx[4], offsety[4];
    uint8_t *bbox_data[4];
    struct SwsContext *sws_ctx;
    int linesizes[4];
    enum AVPixelFormat fmt;
    int left, top, width, height;
    const AVDetectionBBoxHeader *header;
    const AVDetectionBBox *bbox;
    AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DETECTION_BBOXES);
    av_assert0(sd);

    header = (const AVDetectionBBoxHeader *)sd->data;
    bbox = av_get_detection_bbox(header, bbox_index);

    left = bbox->x;
    width = bbox->w;
    top = bbox->y;
    height = bbox->h;

    fmt = input->format;
    sws_ctx = sws_getContext(width, height, frame->format,
                             input->width, input->height, fmt,
                             SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        av_log(log_ctx, AV_LOG_ERROR, "Failed to create scale context for the conversion "
               "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
               av_get_pix_fmt_name(frame->format), width, height,
               av_get_pix_fmt_name(fmt), input->width, input->height);
        return DNN_ERROR;
    }

    if (av_image_fill_linesizes(linesizes, fmt, input->width) < 0) {
        av_log(log_ctx, AV_LOG_ERROR, "unable to get linesizes with av_image_fill_linesizes");
        sws_freeContext(sws_ctx);
        return DNN_ERROR;
    }

    desc = av_pix_fmt_desc_get(frame->format);
    offsetx[1] = offsetx[2] = AV_CEIL_RSHIFT(left, desc->log2_chroma_w);
    offsetx[0] = offsetx[3] = left;

    offsety[1] = offsety[2] = AV_CEIL_RSHIFT(top, desc->log2_chroma_h);
    offsety[0] = offsety[3] = top;

    for (int k = 0; frame->data[k]; k++)
        bbox_data[k] = frame->data[k] + offsety[k] * frame->linesize[k] + offsetx[k];

    sws_scale(sws_ctx, (const uint8_t *const *)&bbox_data, frame->linesize,
                       0, height,
                       (uint8_t *const [4]){input->data, 0, 0, 0}, linesizes);

    sws_freeContext(sws_ctx);

    return DNN_SUCCESS;
}

DNNReturnType ff_frame_to_dnn_detect(AVFrame *frame, DNNData *input, void *log_ctx)
{
    struct SwsContext *sws_ctx;
    int linesizes[4];
    enum AVPixelFormat fmt = input->format;
    sws_ctx = sws_getContext(frame->width, frame->height, frame->format,
                             input->width, input->height, fmt,
                             SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        av_log(log_ctx, AV_LOG_ERROR, "Impossible to create scale context for the conversion "
            "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
            av_get_pix_fmt_name(frame->format), frame->width, frame->height,
            av_get_pix_fmt_name(fmt), input->width, input->height);
        return DNN_ERROR;
    }

    if (av_image_fill_linesizes(linesizes, fmt, input->width) < 0) {
        av_log(log_ctx, AV_LOG_ERROR, "unable to get linesizes with av_image_fill_linesizes");
        sws_freeContext(sws_ctx);
        return DNN_ERROR;
    }

    sws_scale(sws_ctx, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
                       (uint8_t *const [4]){input->data, 0, 0, 0}, linesizes);

    sws_freeContext(sws_ctx);
    return DNN_SUCCESS;
}

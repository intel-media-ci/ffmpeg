/*
 * Copyright (c) 2019 Linjie Fu
 *
 * HW Acceleration API (video decoding) decode sample
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * QSV-Accelerated H264 decoding GPU Copy example.
 *
 * @example qsvdec_gpucopy.c
 * This example shows how to do QSV decode with GPU Copy
 */

#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "libavcodec/qsv.h"
#include <sys/time.h>

static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;
static FILE *output_file = NULL;
static int g_frame_count = 0;
int g_avg_fps = 0;
struct timeval pre_timestamp, cur_timestamp;

static int hw_decoder_init(AVCodecContext *ctx,
                           const enum AVHWDeviceType type) {
    int err = 0;

    if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0)) <
        0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt) return *p;
    }

    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

static int decode_write(AVCodecContext *avctx, AVPacket *packet) {
    AVFrame *frame = NULL;

    uint8_t *buffer = NULL;
    int size;
    int ret = 0;

    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        return ret;
    }

    while (1) {
        if (!(frame = av_frame_alloc())) {
            fprintf(stderr, "Can not alloc frame\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        size = av_image_get_buffer_size(frame->format, frame->width,
                                        frame->height, 1);
        buffer = av_malloc(size);
        if (!buffer) {
            fprintf(stderr, "Can not alloc buffer\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        /* Y data stores in frame->data[0], UV data stores in frame->data[1] */
        ret = av_image_copy_to_buffer(
            buffer, size, (const uint8_t *const *)frame->data,
            (const int *)frame->linesize, frame->format, frame->width,
            frame->height, 1);
        if (ret < 0) {
            fprintf(stderr, "Can not copy image to buffer\n");
            goto fail;
        }

        g_frame_count++;
        g_avg_fps++;
        gettimeofday(&cur_timestamp, NULL);
		
	float interval = cur_timestamp.tv_sec - pre_timestamp.tv_sec + 
		(float)(cur_timestamp.tv_usec - pre_timestamp.tv_usec)/1000000;
	if (interval >= 1) {
            fprintf(
                stderr, "Frame count: %d, avg fps: %f\n", g_frame_count,
                (float)g_avg_fps / interval);
            g_avg_fps = 0;
            pre_timestamp = cur_timestamp;
        }

/*
         if ((ret = fwrite(buffer, 1, size, output_file)) < 0) {
             fprintf(stderr, "Failed to dump raw data.\n");
             goto fail;
         }
*/
    fail:
        av_frame_free(&frame);
        av_freep(&buffer);
        if (ret < 0) return ret;
    }
}

int main(int argc, char *argv[]) {
    AVFormatContext *input_ctx = NULL;
    int video_stream, ret;
    AVStream *video = NULL;
    AVCodecContext *decoder_ctx = NULL;
    AVCodec *decoder = NULL;
    AVPacket packet;

    gettimeofday(&pre_timestamp, NULL);

    enum AVHWDeviceType type;
    int i;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <on/off> <input file> <output file>\n",
                argv[0]);
        return -1;
    }

    type = av_hwdevice_find_type_by_name("qsv");
    if (type == AV_HWDEVICE_TYPE_NONE) {
        fprintf(stderr, "Device type qsv is not supported.\n");
        fprintf(stderr, "Available device types:");
        while ((type = av_hwdevice_iterate_types(type)) !=
               AV_HWDEVICE_TYPE_NONE)
            fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
        fprintf(stderr, "\n");
        return -1;
    }

    /* open the input file */
    if (avformat_open_input(&input_ctx, argv[2], NULL, NULL) != 0) {
        fprintf(stderr, "Cannot open input file '%s'\n", argv[2]);
        return -1;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        fprintf(stderr, "Cannot find input stream information.\n");
        return -1;
    }

    /* find the first H.264 video stream */
    for (i = 0; i < input_ctx->nb_streams; i++) {
        AVStream *st = input_ctx->streams[i];

        if (st->codecpar->codec_id == AV_CODEC_ID_H264 && !video) {
            video = st;
            video_stream = i;
        } else
            st->discard = AVDISCARD_ALL;
    }
    if (!video) {
        fprintf(stderr, "No H.264 video stream in the input file\n");
    }

    /* initialize the decoder */
    decoder = avcodec_find_decoder_by_name("h264_qsv");

    if (!decoder) {
        fprintf(stderr, "The QSV decoder is not present in libavcodec\n");
    }

    for (i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            fprintf(stderr, "Decoder %s does not support device type %s.\n",
                    decoder->name, av_hwdevice_get_type_name(type));
            return -1;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == type) {
            hw_pix_fmt = AV_PIX_FMT_NV12;
            break;
        }
    }

    if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
        return AVERROR(ENOMEM);

    /* set gpu_copy option */
    if (!strcmp(argv[1], "on"))
        av_opt_set(decoder_ctx->priv_data, "gpu_copy", "on", 0);// MFX_GPUCOPY_ON
    else
        av_opt_set(decoder_ctx->priv_data, "gpu_copy", "off", 0);// MFX_GPUCOPY_OFF

    if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
        return -1;

    decoder_ctx->get_format = get_hw_format;

    if (hw_decoder_init(decoder_ctx, type) < 0) return -1;

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
        fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
        return -1;
    }

    /* open the file to dump raw data */
    output_file = fopen(argv[3], "w+");

    /* actual decoding and dump the raw data */
    while (ret >= 0) {
        if ((ret = av_read_frame(input_ctx, &packet)) < 0) break;

        if (video_stream == packet.stream_index)
            ret = decode_write(decoder_ctx, &packet);

        av_packet_unref(&packet);
    }

    /* flush the decoder */
    packet.data = NULL;
    packet.size = 0;
    ret = decode_write(decoder_ctx, &packet);
    av_packet_unref(&packet);

    if (output_file) fclose(output_file);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);

    return 0;
}

/*
 * Intel MediaSDK QSV based AV1 encoder
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


#include <stdint.h>
#include <sys/types.h>

#include <mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavcodec/av1_parse.h"
#include "av1.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "bsf.h"
#include "qsv.h"
#include "qsvenc.h"

typedef struct QSVAV1EncContext {
    AVClass *class;
    AVBSFContext *extra_data_bsf;
    QSVEncContext qsv;
    AVFifo *packet_fifo;
    int64_t parsed_packet_offset;
    AVPacket output_pkt;
    int64_t last_pts;
} QSVAV1EncContext;

static av_cold int qsv_enc_init(AVCodecContext *avctx)
{
    QSVAV1EncContext *q = avctx->priv_data;
    int ret;

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        const AVBitStreamFilter *filter = av_bsf_get_by_name("extract_extradata");
        if (!filter) {
            av_log(avctx, AV_LOG_ERROR, "Cannot get extract_extradata bitstream filter\n");
            return AVERROR_BUG;
        }
        ret = av_bsf_alloc(filter, &q->extra_data_bsf);
        if (ret < 0)
            return ret;
        ret = avcodec_parameters_from_context(q->extra_data_bsf->par_in, avctx);
        if (ret < 0)
           return ret;
        ret = av_bsf_init(q->extra_data_bsf);
        if (ret < 0)
           return ret;
    }

    q->packet_fifo = av_fifo_alloc2(1, sizeof(AVPacket), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->packet_fifo)
        return AVERROR(ENOMEM);

    return ff_qsv_enc_init(avctx, &q->qsv);
}

static int read_tu(const uint8_t *buf, int size, int64_t *offset)
{
    const uint8_t *end = buf + size;
    int64_t obu_size;
    int start_pos, type, temporal_id, spatial_id;

    *offset = 0;
    while (buf < end) {
        int len = parse_obu_header(buf, end - buf, &obu_size, &start_pos,
                                   &type, &temporal_id, &spatial_id);
        if (len < 0)
            return len;

        *offset += len;
        switch (type) {
        case AV1_OBU_FRAME_HEADER:
            // Find show_existing_frame flag
            if (0b10000000 & *(buf + start_pos))
                return 1;
        case AV1_OBU_FRAME:
            // Find show_frame flag
            if (0b00010000 & *(buf + start_pos))
                return 1;
        default:
            break;
        }
        buf += len;
    }
    return 0;
}

static int qsv_reorder_bitstream(AVCodecContext *avctx, QSVAV1EncContext *q,
                                 AVPacket *pkt, int *got_packet)
{
    int ret = 0;
    int64_t offset = 0;
    QSVEncContext *enc_ctx = &q->qsv;
    AVPacket pkt_tmp, *output_pkt = &q->output_pkt;
    if (!output_pkt->data) {
        ret = av_new_packet(output_pkt, enc_ctx->packet_size);
        if (ret < 0)
            return ret;
        output_pkt->size = 0;
    }

    if (*got_packet) {
        av_packet_move_ref(&pkt_tmp, pkt);
        ret = av_fifo_write(q->packet_fifo, &pkt_tmp, 1);
        if (ret < 0)
            return ret;
    }
    *got_packet = 0;

    while (av_fifo_can_read(q->packet_fifo) && !*got_packet) {
        ret = av_fifo_peek(q->packet_fifo, &pkt_tmp, 1, 0);
        if (ret < 0)
            return ret;
        ret = read_tu(pkt_tmp.data + q->parsed_packet_offset,
                      pkt_tmp.size - q->parsed_packet_offset, &offset);
        if (ret < 0)
            return ret;
        // Copy parsed data to output_pkt
        memcpy(output_pkt->data + output_pkt->size,
               pkt_tmp.data + q->parsed_packet_offset, offset);
        output_pkt->size += offset;
        // If find show_frame, return output_pkt
        if (ret == 1) {
            ret = av_packet_copy_props(output_pkt, &pkt_tmp);
            if (ret < 0)
                return ret;
            av_packet_move_ref(pkt, output_pkt);
            // Timestamp need to be calculated because pkt is reorded.
            if (pkt->pts <= q->last_pts) {
                int64_t duration = av_rescale_q(1,
                        (AVRational){avctx->framerate.den, avctx->framerate.num},
                        avctx->time_base);
                q->last_pts = pkt->dts = pkt->pts = q->last_pts + duration;
            } else
                q->last_pts = pkt->dts = pkt->pts;
            *got_packet = 1;
        }
        q->parsed_packet_offset += offset;
        // If finish parsing one packet, realease it.
        if (q->parsed_packet_offset == pkt_tmp.size) {
            q->parsed_packet_offset = 0;
            ret = av_fifo_read(q->packet_fifo, &pkt_tmp, 1);
            if (ret < 0)
                return ret;
            av_packet_unref(&pkt_tmp);
        }
    }
    return 0;
}

static int qsv_enc_frame(AVCodecContext *avctx, AVPacket *pkt,
                         const AVFrame *frame, int *got_packet)
{
    QSVAV1EncContext *q = avctx->priv_data;
    int ret;

    ret = ff_qsv_encode(avctx, &q->qsv, pkt, frame, got_packet);
    if (ret < 0)
        return ret;

    ret = qsv_reorder_bitstream(avctx, q, pkt, got_packet);
    if (ret < 0)
        return ret;

    if (*got_packet && avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        ret = av_bsf_send_packet(q->extra_data_bsf, pkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "extract_extradata filter "
                "failed to send input packet\n");
            return ret;
        }

        ret = av_bsf_receive_packet(q->extra_data_bsf, pkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "extract_extradata filter "
                "failed to receive output packet\n");
            return ret;
        }
    }

    return ret;
}

static av_cold int qsv_enc_close(AVCodecContext *avctx)
{
    QSVAV1EncContext *q = avctx->priv_data;
    AVPacket pkt;

    av_bsf_free(&q->extra_data_bsf);
    av_packet_unref(&q->output_pkt);
    while(av_fifo_can_read(q->packet_fifo)) {
        av_fifo_read(q->packet_fifo, &pkt, 1);
        av_packet_unref(&pkt);
    }
    av_fifo_freep2(&q->packet_fifo);

    return ff_qsv_enc_close(avctx, &q->qsv);
}

#define OFFSET(x) offsetof(QSVAV1EncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    QSV_COMMON_OPTS
    QSV_OPTION_B_STRATEGY
    { "profile", NULL, OFFSET(qsv.profile), AV_OPT_TYPE_INT, { .i64 = MFX_PROFILE_UNKNOWN }, 0, INT_MAX, VE, "profile" },
        { "unknown" , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_UNKNOWN      }, INT_MIN, INT_MAX,     VE, "profile" },
        { "main"    , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_AV1_MAIN     }, INT_MIN, INT_MAX,     VE, "profile" },
    { "tile_cols",  "Number of columns for tiled encoding",   OFFSET(qsv.tile_cols),    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, UINT16_MAX, VE },
    { "tile_rows",  "Number of rows for tiled encoding",      OFFSET(qsv.tile_rows),    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, UINT16_MAX, VE },
    { NULL },
};

static const AVClass class = {
    .class_name = "av1_qsv encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault qsv_enc_defaults[] = {
    { "g",         "-1"   },
    { "bf",        "-1"   },
    { NULL },
};

FFCodec ff_av1_qsv_encoder = {
    .p.name           = "av1_qsv",
    .p.long_name      = NULL_IF_CONFIG_SMALL("AV1 (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVAV1EncContext),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_AV1,
    .init           = qsv_enc_init,
    FF_CODEC_ENCODE_CB(qsv_enc_frame),
    .close          = qsv_enc_close,
    .p.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HYBRID,
    .p.pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_NV12,
                                                    AV_PIX_FMT_P010,
                                                    AV_PIX_FMT_QSV,
                                                    AV_PIX_FMT_NONE },
    .p.priv_class     = &class,
    .defaults       = qsv_enc_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .p.wrapper_name   = "qsv",
    .hw_configs     = ff_qsv_enc_hw_configs,
};

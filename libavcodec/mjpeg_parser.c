/*
 * MJPEG parser
 * Copyright (c) 2000, 2001 Fabrice Bellard
 * Copyright (c) 2003 Alex Beregszaszi
 * Copyright (c) 2003-2004 Michael Niedermayer
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
 * MJPEG parser.
 */

#include "parser.h"
#include "mjpeg.h"
#include "mjpegdec.h"
#include "get_bits.h"

typedef struct MJPEGParserContext{
    ParseContext pc;
    MJpegDecodeContext dec_ctx;
    int size;
}MJPEGParserContext;

/* return the 8 bit start code value and update the search
   state. Return -1 if no start code found */
static int find_frame_header_marker(const uint8_t **pbuf_ptr, const uint8_t *buf_end)
{
    const uint8_t *buf_ptr;
    unsigned int v, v2;
    int val;
    int skipped = 0;

    buf_ptr = *pbuf_ptr;
    while (buf_end - buf_ptr > 1) {
        v  = *buf_ptr++;
        v2 = *buf_ptr;
        if ((v == 0xff) && buf_ptr < buf_end &&
            ((v2 >= SOF0) && (v2 <= SOF3)) ) {
            val = *buf_ptr++;
            goto found;
        }
        skipped++;
    }
    buf_ptr = buf_end;
    val = -1;
found:
    ff_dlog(NULL, "find_marker skipped %d bytes\n", skipped);
    *pbuf_ptr = buf_ptr;
    return val;
}

static void jpeg_set_interlace_polarity(AVCodecContext *avctx, MJpegDecodeContext *dec_ctx)
{
    if (avctx->extradata_size > 14
        && AV_RL32(avctx->extradata) == 0x2C
        && AV_RL32(avctx->extradata+4) == 0x18) {
        if (avctx->extradata[12] == 1) /* NTSC */
            dec_ctx->interlace_polarity = 1;
        if (avctx->extradata[12] == 2) /* PAL */
            dec_ctx->interlace_polarity = 0;
    }
}

static int jpeg_parse_header(AVCodecParserContext *s, AVCodecContext *avctx,
                               const uint8_t *buf, int buf_size)
{
    MJPEGParserContext *m = s->priv_data;
    MJpegDecodeContext *dec_ctx = &m->dec_ctx;
    int start_code;
    const uint8_t *start, *end;
    int ret=0;

    start = buf;
    end = buf + buf_size;
    start_code = find_frame_header_marker(&start, end);
    if (start_code < 0) {
        av_log(avctx, AV_LOG_ERROR, "parse header failure:"
            "can't find supported marker type (%x)\n", start_code);

        return -1;
    } else
        av_log(avctx, AV_LOG_DEBUG, "marker=%x\n", start_code);

    jpeg_set_interlace_polarity(avctx, dec_ctx);
    init_get_bits8(&dec_ctx->gb, start, end - start);
    dec_ctx->avctx = avctx;

    switch(start_code) {
    case SOF0:
        avctx->profile = FF_PROFILE_MJPEG_HUFFMAN_BASELINE_DCT;
        dec_ctx->lossless        = 0;
        dec_ctx->progressive     = 0;
        break;
    case SOF1:
        avctx->profile = FF_PROFILE_MJPEG_HUFFMAN_EXTENDED_SEQUENTIAL_DCT;
        dec_ctx->lossless        = 0;
        dec_ctx->progressive     = 0;
        break;
    case SOF2:
        avctx->profile = FF_PROFILE_MJPEG_HUFFMAN_PROGRESSIVE_DCT;
        dec_ctx->lossless        = 0;
        dec_ctx->progressive     = 1;
        break;
    case SOF3:
        avctx->profile = FF_PROFILE_MJPEG_HUFFMAN_LOSSLESS;
        dec_ctx->lossless        = 1;
        dec_ctx->progressive     = 0;
        break;
    default:
       assert(0);
    }

    ret = ff_mjpeg_decode_header(dec_ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_WARNING, "Failed to parse header\n");
        return ret;
    }

    s->height = dec_ctx->height;
    s->width  = dec_ctx->width;
    s->coded_height = s->height;
    s->coded_width  = s->width;
    s->format       = avctx->pix_fmt;
    s->pict_type    = AV_PICTURE_TYPE_I;
    s->key_frame    = 1;

    if (dec_ctx->interlaced) {
        if (dec_ctx->bottom_field)
            s->field_order = AV_FIELD_BB;
        else
            s->field_order = AV_FIELD_TT;
    } else
        s->field_order = AV_FIELD_PROGRESSIVE;

    return 0;
}


/**
 * Find the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or -1
 */
static int find_frame_end(MJPEGParserContext *m, const uint8_t *buf, int buf_size){
    ParseContext *pc= &m->pc;
    int vop_found, i;
    uint32_t state;

    vop_found= pc->frame_start_found;
    state= pc->state;

    i=0;
    if(!vop_found){
        for(i=0; i<buf_size;){
            state= (state<<8) | buf[i];
            if(state>=0xFFC00000 && state<=0xFFFEFFFF){
                if(state>=0xFFD80000 && state<=0xFFD8FFFF){
                    i++;
                    vop_found=1;
                    break;
                }else if(state<0xFFD00000 || state>0xFFD9FFFF){
                    m->size= (state&0xFFFF)-1;
                }
            }
            if(m->size>0){
                int size= FFMIN(buf_size-i, m->size);
                i+=size;
                m->size-=size;
                state=0;
                continue;
            }else
                i++;
        }
    }

    if(vop_found){
        /* EOF considered as end of frame */
        if (buf_size == 0)
            return 0;
        for(; i<buf_size;){
            state= (state<<8) | buf[i];
            if(state>=0xFFC00000 && state<=0xFFFEFFFF){
                if(state>=0xFFD80000 && state<=0xFFD8FFFF){
                    pc->frame_start_found=0;
                    pc->state=0;
                    return i-3;
                } else if(state<0xFFD00000 || state>0xFFD9FFFF){
                    m->size= (state&0xFFFF)-1;
                }
            }
            if(m->size>0){
                int size= FFMIN(buf_size-i, m->size);
                i+=size;
                m->size-=size;
                state=0;
                continue;
            }else
                i++;
        }
    }
    pc->frame_start_found= vop_found;
    pc->state= state;
    return END_NOT_FOUND;
}

static av_cold int jpeg_parse_init(AVCodecParserContext *s)
{
    MJPEGParserContext *m = s->priv_data;
    MJpegDecodeContext *dec_ctx = &m->dec_ctx;

    if (!dec_ctx->picture_ptr) {
        dec_ctx->picture = av_frame_alloc();
        if (!dec_ctx->picture)
            return AVERROR(ENOMEM);
        dec_ctx->picture_ptr = dec_ctx->picture;
    }

    dec_ctx->first_picture = 1;
    dec_ctx->got_picture   = 0;
    dec_ctx->org_height    = 0;
    dec_ctx->ls            = 0;
    return 0;
}

static av_cold void jpeg_parse_close(AVCodecParserContext *s)
{
    MJPEGParserContext *m = s->priv_data;
    ParseContext *pc = &m->pc;
    MJpegDecodeContext *dec_ctx = &m->dec_ctx;

    av_freep(&pc->buffer);

    if (dec_ctx->picture) {
        av_frame_free(&dec_ctx->picture);
        dec_ctx->picture_ptr = NULL;
    } else if (dec_ctx->picture_ptr)
        av_frame_unref(dec_ctx->picture_ptr);
}

static int jpeg_parse(AVCodecParserContext *s,
                      AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size,
                      const uint8_t *buf, int buf_size)
{
    MJPEGParserContext *m = s->priv_data;
    ParseContext *pc = &m->pc;
    int next;

    if(s->flags & PARSER_FLAG_COMPLETE_FRAMES){
        next= buf_size;
    }else{
        next= find_frame_end(m, buf, buf_size);

        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }
    }

    jpeg_parse_header(s, avctx, buf, buf_size);

    *poutbuf = buf;
    *poutbuf_size = buf_size;
    return next;
}


AVCodecParser ff_mjpeg_parser = {
    .codec_ids      = { AV_CODEC_ID_MJPEG, AV_CODEC_ID_JPEGLS },
    .priv_data_size = sizeof(MJPEGParserContext),
    .parser_init      = jpeg_parse_init,
    .parser_parse   = jpeg_parse,
    .parser_close   = jpeg_parse_close,
};

/*
 * VP9 parser
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

#include "libavutil/avassert.h"
#include "cbs.h"
#include "cbs_vp9.h"
#include "parser.h"

typedef struct VP9ParserContext {
    CodedBitstreamContext *cbc;
    VP9RawFrameHeader frame_header;
} VP9ParserContext;

static const enum AVPixelFormat vp9_pix_fmts[3][2][2] = {
    { // 8-bit.
        { AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV440P },
        { AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P },
    },
    { // 10-bit.
        { AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV440P10 },
        { AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV420P10 },
    },
    { // 12-bit.
        { AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV440P12 },
        { AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV420P12 },
    },
};

static int vp9_parser_parse(AVCodecParserContext *ctx,
                            AVCodecContext *avctx,
                            const uint8_t **out_data, int *out_size,
                            const uint8_t *data, int size)
{
    VP9ParserContext *s = ctx->priv_data;
    const CodedBitstreamVP9Context *vp9 = s->cbc->priv_data;
    const VP9RawFrameHeader *fh;
    int err;

    *out_data = data;
    *out_size = size;

    if (!size)
        return 0;

    s->cbc->log_ctx = avctx;

    err = ff_cbs_vp9_parse_headers(s->cbc, &s->frame_header, data, size);
    if (err < 0) {
        av_log(avctx, AV_LOG_WARNING, "Failed to parse VP9 frame headers.\n");
        goto end;
    }
    fh = &s->frame_header;

    avctx->profile = vp9->profile;
    avctx->level   = FF_LEVEL_UNKNOWN;

    ctx->width  = ctx->coded_width  = vp9->frame_width;
    ctx->height = ctx->coded_height = vp9->frame_height;

    ctx->pict_type = fh->intra_only ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
    ctx->key_frame = !fh->frame_type;

    ctx->picture_structure = AV_PICTURE_STRUCTURE_FRAME;

    av_assert0(vp9->bit_depth == 8  ||
               vp9->bit_depth == 10 ||
               vp9->bit_depth == 12);

    ctx->format = vp9_pix_fmts[(vp9->bit_depth - 8) / 2]
                              [vp9->subsampling_x][vp9->subsampling_y];

end:
    s->cbc->log_ctx = NULL;

    return size;
}

static av_cold int vp9_parser_init(AVCodecParserContext *ctx)
{
    VP9ParserContext *s = ctx->priv_data;
    return ff_cbs_init(&s->cbc, AV_CODEC_ID_VP9, NULL);
}

static av_cold void vp9_parser_close(AVCodecParserContext *ctx)
{
    VP9ParserContext *s = ctx->priv_data;
    ff_cbs_close(&s->cbc);
}

AVCodecParser ff_vp9_parser = {
    .codec_ids      = { AV_CODEC_ID_VP9 },
    .priv_data_size = sizeof(VP9ParserContext),
    .parser_init    = vp9_parser_init,
    .parser_close   = vp9_parser_close,
    .parser_parse   = vp9_parser_parse,
};

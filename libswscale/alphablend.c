/*
 * Copyright (C) 2015 Michael Niedermayer <michaelni@gmx.at>
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

#include "swscale_internal.h"

int ff_sws_alphablendaway(SwsInternal *c, const uint8_t *const src[],
                          const int srcStride[], int srcSliceY, int srcSliceH,
                          uint8_t *const dst[], const int dstStride[])
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(c->opts.src_format);
    const int lum_w = c->opts.src_w;
    const int lum_h = c->opts.src_h;
    int nb_components = desc->nb_components;
    int plane, x, ysrc;
    int plane_count = isGray(c->opts.src_format) ? 1 : 3;
    int sixteen_bits = desc->comp[0].depth >= 9;
    unsigned off    = 1<<(desc->comp[0].depth - 1);
    unsigned shift  = desc->comp[0].depth;
    unsigned max    = (1<<shift) - 1;
    int target_table[2][3];

    for (plane = 0; plane < plane_count; plane++) {
        int a = 0, b = 0;
        if (c->opts.alpha_blend == SWS_ALPHA_BLEND_CHECKERBOARD) {
            a = (1<<(desc->comp[0].depth - 1))/2;
            b = 3*(1<<(desc->comp[0].depth-1))/2;
        }
        target_table[0][plane] = plane && !(desc->flags & AV_PIX_FMT_FLAG_RGB) ? 1<<(desc->comp[0].depth - 1) : a;
        target_table[1][plane] = plane && !(desc->flags & AV_PIX_FMT_FLAG_RGB) ? 1<<(desc->comp[0].depth - 1) : b;
    }

    av_assert0(plane_count == nb_components - 1);
    if (desc->flags & AV_PIX_FMT_FLAG_PLANAR) {
        for (plane = 0; plane < plane_count; plane++) {
            int w = plane ? c->chrSrcW : c->opts.src_w;
            int x_subsample = plane ? desc->log2_chroma_w: 0;
            int y_subsample = plane ? desc->log2_chroma_h: 0;
            for (ysrc = 0; ysrc < AV_CEIL_RSHIFT(srcSliceH, y_subsample); ysrc++) {
                int y = ysrc + (srcSliceY >> y_subsample);
                int subsample_row = y_subsample && (y << y_subsample) + 1 < lum_h;
                if (x_subsample || subsample_row) {
                    int alpha;
                    unsigned u;
                    if (sixteen_bits) {
                        ptrdiff_t alpha_step = srcStride[plane_count] >> 1;
                        const uint16_t *s = (const uint16_t *)(src[plane      ] +  srcStride[plane      ] * ysrc);
                        const uint16_t *a = (const uint16_t *)(src[plane_count] + (srcStride[plane_count] * ysrc << y_subsample));
                              uint16_t *d = (      uint16_t *)(dst[plane      ] +  dstStride[plane      ] * y);
                        if ((!isBE(c->opts.src_format)) == !HAVE_BIGENDIAN) {
                            for (x = 0; x < w; x++) {
                                const int xnext = FFMIN(2*x + 1, lum_w - 1);
                                if (subsample_row) {
                                    alpha = (a[2*x]              + a[xnext] + 2 +
                                             a[2*x + alpha_step] + a[xnext + alpha_step]) >> 2;
                                } else
                                    alpha = (a[2*x] + a[xnext]) >> 1;
                                u = s[x]*alpha + target_table[((x^y)>>5)&1][plane]*(max-alpha) + off;
                                d[x] = av_clip((u + (u >> shift)) >> shift, 0, max);
                            }
                        } else {
                            for (x = 0; x < w; x++) {
                                const int xnext = FFMIN(2*x + 1, lum_w - 1);
                                if (subsample_row) {
                                    alpha = (av_bswap16(a[2*x])              + av_bswap16(a[xnext]) + 2 +
                                             av_bswap16(a[2*x + alpha_step]) + av_bswap16(a[xnext + alpha_step])) >> 2;
                                } else
                                    alpha = (av_bswap16(a[2*x]) + av_bswap16(a[xnext])) >> 1;
                                u = av_bswap16(s[x])*alpha + target_table[((x^y)>>5)&1][plane]*(max-alpha) + off;
                                d[x] = av_clip((u + (u >> shift)) >> shift, 0, max);
                            }
                        }
                    } else {
                        ptrdiff_t alpha_step = srcStride[plane_count];
                        const uint8_t *s = src[plane      ] + srcStride[plane] * ysrc;
                        const uint8_t *a = src[plane_count] + (srcStride[plane_count] * ysrc << y_subsample);
                              uint8_t *d = dst[plane      ] + dstStride[plane] * y;
                        for (x = 0; x < w; x++) {
                            const int xnext = FFMIN(2*x + 1, lum_w - 1);
                            if (subsample_row) {
                                alpha = (a[2*x]              + a[xnext] + 2 +
                                         a[2*x + alpha_step] + a[xnext + alpha_step]) >> 2;
                            } else
                                alpha = (a[2*x] + a[xnext]) >> 1;
                            u = s[x]*alpha + target_table[((x^y)>>5)&1][plane]*(255-alpha) + 128;
                            d[x] = (257*u) >> 16;
                        }
                    }
                } else {
                if (sixteen_bits) {
                    const uint16_t *s = (const uint16_t *)(src[plane      ] + srcStride[plane      ] * ysrc);
                    const uint16_t *a = (const uint16_t *)(src[plane_count] + srcStride[plane_count] * ysrc);
                          uint16_t *d = (      uint16_t *)(dst[plane      ] + dstStride[plane      ] * y);
                    if ((!isBE(c->opts.src_format)) == !HAVE_BIGENDIAN) {
                        for (x = 0; x < w; x++) {
                            unsigned u = s[x]*a[x] + target_table[((x^y)>>5)&1][plane]*(max-a[x]) + off;
                            d[x] = av_clip((u + (u >> shift)) >> shift, 0, max);
                        }
                    } else {
                        for (x = 0; x < w; x++) {
                            unsigned aswap =av_bswap16(a[x]);
                            unsigned u = av_bswap16(s[x])*aswap + target_table[((x^y)>>5)&1][plane]*(max-aswap) + off;
                            d[x] = av_clip((u + (u >> shift)) >> shift, 0, max);
                        }
                    }
                } else {
                    const uint8_t *s = src[plane      ] + srcStride[plane] * ysrc;
                    const uint8_t *a = src[plane_count] + srcStride[plane_count] * ysrc;
                          uint8_t *d = dst[plane      ] + dstStride[plane] * y;
                    for (x = 0; x < w; x++) {
                        unsigned u = s[x]*a[x] + target_table[((x^y)>>5)&1][plane]*(255-a[x]) + 128;
                        d[x] = (257*u) >> 16;
                    }
                }
                }
            }
        }
    } else {
        int alpha_pos = desc->comp[plane_count].offset;
        int w = c->opts.src_w;
        for (ysrc = 0; ysrc < srcSliceH; ysrc++) {
            int y = ysrc + srcSliceY;
            if (sixteen_bits) {
                const uint16_t *s = (const uint16_t *)(src[0] + srcStride[0] * ysrc + 2*!alpha_pos);
                const uint16_t *a = (const uint16_t *)(src[0] + srcStride[0] * ysrc +    alpha_pos);
                      uint16_t *d = (      uint16_t *)(dst[0] + dstStride[0] * y);
                if ((!isBE(c->opts.src_format)) == !HAVE_BIGENDIAN) {
                    for (x = 0; x < w; x++) {
                        for (plane = 0; plane < plane_count; plane++) {
                            int x_index = (plane_count + 1) * x;
                            unsigned u = s[x_index + plane]*a[x_index] + target_table[((x^y)>>5)&1][plane]*(max-a[x_index]) + off;
                            d[plane_count*x + plane] = av_clip((u + (u >> shift)) >> shift, 0, max);
                        }
                    }
                } else {
                    for (x = 0; x < w; x++) {
                        for (plane = 0; plane < plane_count; plane++) {
                            int x_index = (plane_count + 1) * x;
                            unsigned aswap =av_bswap16(a[x_index]);
                            unsigned u = av_bswap16(s[x_index + plane])*aswap + target_table[((x^y)>>5)&1][plane]*(max-aswap) + off;
                            d[plane_count*x + plane] = av_clip((u + (u >> shift)) >> shift, 0, max);
                        }
                    }
                }
            } else {
                const uint8_t *s = src[0] + srcStride[0] * ysrc + !alpha_pos;
                const uint8_t *a = src[0] + srcStride[0] * ysrc + alpha_pos;
                      uint8_t *d = dst[0] + dstStride[0] * y;
                for (x = 0; x < w; x++) {
                    for (plane = 0; plane < plane_count; plane++) {
                        int x_index = (plane_count + 1) * x;
                        unsigned u = s[x_index + plane]*a[x_index] + target_table[((x^y)>>5)&1][plane]*(255-a[x_index]) + 128;
                        d[plane_count*x + plane] = (257*u) >> 16;
                    }
                }
            }
        }
    }

    return 0;
}

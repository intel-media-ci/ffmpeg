/*
 * AV1 video decoder
 * *
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

#ifndef AVCODEC_AV1DEC_H
#define AVCODEC_AV1DEC_H

#include <stdatomic.h>
#include "libavutil/buffer.h"
#include "libavutil/md5.h"
#include "avcodec.h"
#include "bswapdsp.h"
#include "get_bits.h"
#include "av1_parse.h"
#include "libavformat/avformat.h"
#include "libavformat/av1.h"
#include "thread.h"
#include "cbs.h"
#include "cbs_av1.h"

typedef enum AV1FrameRestorationType {
    AV1_RESTORE_NONE,
    AV1_RESTORE_WIENER,
    AV1_RESTORE_SGRPROJ,
    AV1_RESTORE_SWITCHABLE,
} AV1FrameRestorationType;

typedef struct AV1Frame {
    ThreadFrame tf;

    AVBufferRef *hwaccel_priv_buf;
    void *hwaccel_picture_private;

    uint8_t loop_filter_delta_enabled;
    int8_t  loop_filter_ref_deltas[AV1_TOTAL_REFS_PER_FRAME];
    int8_t  loop_filter_mode_deltas[2];
    uint8_t gm_type[AV1_TOTAL_REFS_PER_FRAME];
    int32_t gm_params[AV1_TOTAL_REFS_PER_FRAME][6];
} AV1Frame;

typedef struct TileGroupInfo {
    uint32_t tile_group_offset;
    uint32_t tile_offset;
    uint32_t tile_size;
    uint16_t tile_row;
    uint16_t tile_column;
    uint16_t tile_width_sbs;
    uint16_t tile_height_sbs;
    uint32_t tile_size_bytes;
} TileGroupInfo;

typedef struct AV1DecContext {
    const AVClass *class;
    AVCodecContext *avctx;

    enum AVPixelFormat pix_fmt;
    CodedBitstreamContext *cbc;
    CodedBitstreamFragment current_obu;

    AV1RawSequenceHeader raw_seq;
    AV1RawFrameHeader raw_frame_header;
    AV1RawTileGroup raw_tile_group;
    TileGroupInfo tile_group_info[128];
    uint16_t tile_num;
    uint16_t tg_start;
    uint16_t tg_end;

    AV1Frame ref[8];
    AV1Frame cur_frame;

} AV1DecContext;

#endif /* AVCODEC_AV1DEC_H */

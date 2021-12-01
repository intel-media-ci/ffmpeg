/*
 * Intel MediaSDK QSV encoder utility functions
 *
 * copyright (c) 2013 Yukinori Yamazoe
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

#ifndef AVCODEC_QSVENC_H
#define AVCODEC_QSVENC_H

#include <stdint.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <mfx/mfxvideo.h>

#include "libavutil/avutil.h"
#include "libavutil/fifo.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/rational.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"


#include "avcodec.h"
#include "hwconfig.h"
#include "qsv_internal.h"

#define QSV_HAVE_CO2 QSV_VERSION_ATLEAST(1, 6)
#define QSV_HAVE_CO3 QSV_VERSION_ATLEAST(1, 11)
#define QSV_HAVE_CO_VPS  QSV_VERSION_ATLEAST(1, 17)
#define QSV_HAVE_HDR_METADATA QSV_VERSION_ATLEAST(1, 26)
#define QSV_HAVE_VIDEO_SIGNAL_INFO QSV_VERSION_ATLEAST(1, 3)


#define QSV_HAVE_EXT_HEVC_TILES QSV_VERSION_ATLEAST(1, 13)
#define QSV_HAVE_EXT_VP9_PARAM QSV_VERSION_ATLEAST(1, 26)

#define QSV_HAVE_TRELLIS QSV_VERSION_ATLEAST(1, 8)
#define QSV_HAVE_MAX_SLICE_SIZE QSV_VERSION_ATLEAST(1, 9)
#define QSV_HAVE_BREF_TYPE      QSV_VERSION_ATLEAST(1, 8)

#define QSV_HAVE_LA     QSV_VERSION_ATLEAST(1, 7)
#define QSV_HAVE_LA_DS  QSV_VERSION_ATLEAST(1, 8)
#define QSV_HAVE_LA_HRD QSV_VERSION_ATLEAST(1, 11)
#define QSV_HAVE_VDENC  QSV_VERSION_ATLEAST(1, 15)

#define QSV_HAVE_GPB    QSV_VERSION_ATLEAST(1, 18)

#if defined(_WIN32) || defined(__CYGWIN__)
#define QSV_HAVE_AVBR   QSV_VERSION_ATLEAST(1, 3)
#define QSV_HAVE_ICQ    QSV_VERSION_ATLEAST(1, 8)
#define QSV_HAVE_VCM    QSV_VERSION_ATLEAST(1, 8)
#define QSV_HAVE_QVBR   QSV_VERSION_ATLEAST(1, 11)
#define QSV_HAVE_MF     0
#else
#define QSV_HAVE_AVBR   0
#define QSV_HAVE_ICQ    QSV_VERSION_ATLEAST(1, 28)
#define QSV_HAVE_VCM    0
#define QSV_HAVE_QVBR   QSV_VERSION_ATLEAST(1, 28)
#define QSV_HAVE_MF     QSV_VERSION_ATLEAST(1, 25)
#endif

#if !QSV_HAVE_LA_DS
#define MFX_LOOKAHEAD_DS_UNKNOWN 0
#define MFX_LOOKAHEAD_DS_OFF 0
#define MFX_LOOKAHEAD_DS_2x 0
#define MFX_LOOKAHEAD_DS_4x 0
#endif

#define QSV_PARAM_BAD_NAME (-1)
#define QSV_PARAM_BAD_VALUE (-2)
#define QSV_PARAMS_BUFFER_LENGTH 100
#define QSV_PARAM_VALID (0)
#define VIDEO_SIGNAL_COLOR_DESCRIPTION_PRESENT (1)
#define VIDEO_SIGNAL_FORMAT_UNSPECIFIED (5)
#define RGB_CHANNEL_LENGTH (2)
#define LIGHT_LEVEL_DATA_LENGTH (2)
#define WHITE_POINT_DATA_LENGTH (2)

#define QSV_COMMON_OPTS \
{ "async_depth", "Maximum processing parallelism", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 1, INT_MAX, VE },                          \
{ "avbr_accuracy",    "Accuracy of the AVBR ratecontrol (unit of tenth of percent)",    OFFSET(qsv.avbr_accuracy),    AV_OPT_TYPE_INT, { .i64 = 1 }, 1, UINT16_MAX, VE }, \
{ "avbr_convergence", "Convergence of the AVBR ratecontrol (unit of 100 frames)", OFFSET(qsv.avbr_convergence), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, UINT16_MAX, VE },     \
{ "preset", NULL, OFFSET(qsv.preset), AV_OPT_TYPE_INT, { .i64 = MFX_TARGETUSAGE_BALANCED }, MFX_TARGETUSAGE_BEST_QUALITY, MFX_TARGETUSAGE_BEST_SPEED,   VE, "preset" }, \
{ "veryfast",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_BEST_SPEED  },   INT_MIN, INT_MAX, VE, "preset" },                                                \
{ "faster",      NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_6  },            INT_MIN, INT_MAX, VE, "preset" },                                                \
{ "fast",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_5  },            INT_MIN, INT_MAX, VE, "preset" },                                                \
{ "medium",      NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_BALANCED  },     INT_MIN, INT_MAX, VE, "preset" },                                                \
{ "slow",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_3  },            INT_MIN, INT_MAX, VE, "preset" },                                                \
{ "slower",      NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_2  },            INT_MIN, INT_MAX, VE, "preset" },                                                \
{ "veryslow",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_BEST_QUALITY  }, INT_MIN, INT_MAX, VE, "preset" },                                                \
{ "rdo",            "Enable rate distortion optimization",    OFFSET(qsv.rdo),            AV_OPT_TYPE_INT, { .i64 = -1 }, -1,          1, VE },                         \
{ "max_frame_size", "Maximum encoded frame size in bytes",    OFFSET(qsv.max_frame_size), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, UINT16_MAX, VE },                         \
{ "max_slice_size", "Maximum encoded slice size in bytes",    OFFSET(qsv.max_slice_size), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, UINT16_MAX, VE },                         \
{ "bitrate_limit",  "Toggle bitrate limitations",             OFFSET(qsv.bitrate_limit),  AV_OPT_TYPE_INT, { .i64 = -1 }, -1,          1, VE },                         \
{ "mbbrc",          "MB level bitrate control",               OFFSET(qsv.mbbrc),          AV_OPT_TYPE_INT, { .i64 = -1 }, -1,          1, VE },                         \
{ "extbrc",         "Extended bitrate control",               OFFSET(qsv.extbrc),         AV_OPT_TYPE_INT, { .i64 = -1 }, -1,          1, VE },                         \
{ "adaptive_i",     "Adaptive I-frame placement",             OFFSET(qsv.adaptive_i),     AV_OPT_TYPE_INT, { .i64 = -1 }, -1,          1, VE },                         \
{ "adaptive_b",     "Adaptive B-frame placement",             OFFSET(qsv.adaptive_b),     AV_OPT_TYPE_INT, { .i64 = -1 }, -1,          1, VE },                         \
{ "b_strategy",     "Strategy to choose between I/P/B-frames", OFFSET(qsv.b_strategy),    AV_OPT_TYPE_INT, { .i64 = -1 }, -1,          1, VE },                         \
{ "forced_idr",     "Forcing I frames as IDR frames",         OFFSET(qsv.forced_idr),     AV_OPT_TYPE_BOOL,{ .i64 = 0  },  0,          1, VE },                         \
{ "low_power", "enable low power mode(experimental: many limitations by mfx version, BRC modes, etc.)", OFFSET(qsv.low_power), AV_OPT_TYPE_BOOL, { .i64 = -1}, -1, 1, VE},\
{ "qsv_params", "set the qsv configuration using a :-separated list of key=value parameters", OFFSET(qsv.qsv_opts), AV_OPT_TYPE_DICT, { 0 }, 0, 0, VE },\

/* String keys and values accepted by qsv_param_keys*/
static const char * const qsv_color_range_names[] = { "limited", "full", 0 };
static const char * const qsv_colorprim_names[] = { "reserved", "bt709", "unknown",
                                                    "reserved", "bt470m", "bt470bg", "smpte170m", "smpte240m", "film", "bt2020", "smpte428", "smpte431", "smpte432", 0 };
static const char * const qsv_transfer_names[] = { "reserved", "bt709", "unknown", "reserved", "bt470m", "bt470bg", "smpte170m", "smpte240m", "linear", "log100",
                                                    "log316", "iec61966-2-4", "bt1361e", "iec61966-2-1", "bt2020-10", "bt2020-12",
                                                    "smpte2084", "smpte428", "arib-std-b67", 0 };
static const char * const qsv_colmatrix_names[] = { "gbr", "bt709", "unknown", "", "fcc", "bt470bg", "smpte170m", "smpte240m",
                                                      "ycgco", "bt2020nc", "bt2020c", "smpte2085", "chroma-derived-nc", "chroma-derived-c", "ictcp", 0 };
static const char * const qsv_key_names[] = { "range", "colorprim", "transfer", "colormatrix", "master-display", "max-cll" };

extern const AVCodecHWConfigInternal *const ff_qsv_enc_hw_configs[];

typedef int SetEncodeCtrlCB (AVCodecContext *avctx,
                             const AVFrame *frame, mfxEncodeCtrl* enc_ctrl);
typedef struct QSVEncContext {
    AVCodecContext *avctx;

    QSVFrame *work_frames;

    mfxSession session;
    QSVSession internal_qs;

    int packet_size;
    int width_align;
    int height_align;

    mfxVideoParam param;
    mfxFrameAllocRequest req;

    mfxExtCodingOption  extco;
#if QSV_HAVE_CO2
    mfxExtCodingOption2 extco2;
#endif
#if QSV_HAVE_CO3
    mfxExtCodingOption3 extco3;
#endif
#if QSV_HAVE_MF
    mfxExtMultiFrameParam   extmfp;
    mfxExtMultiFrameControl extmfc;
#endif
#if QSV_HAVE_EXT_HEVC_TILES
    mfxExtHEVCTiles exthevctiles;
#endif
#if QSV_HAVE_HDR_METADATA
    mfxExtMasteringDisplayColourVolume master_display_volume_data;
#endif
#if QSV_HAVE_EXT_VP9_PARAM
    mfxExtVP9Param  extvp9param;
#endif

    mfxExtOpaqueSurfaceAlloc opaque_alloc;
    mfxFrameSurface1       **opaque_surfaces;
    AVBufferRef             *opaque_alloc_buf;

    mfxExtVideoSignalInfo extvsi;

    mfxExtBuffer  *extparam_internal[3 + QSV_HAVE_CO2 + QSV_HAVE_CO3 + (QSV_HAVE_MF * 2) + QSV_HAVE_HDR_METADATA + QSV_HAVE_VIDEO_SIGNAL_INFO];
    int         nb_extparam_internal;

    mfxExtBuffer **extparam;

    AVFifoBuffer *async_fifo;

    QSVFramesContext frames_ctx;

    mfxVersion          ver;

    int hevc_vps;

    // options set by the caller
    int async_depth;
    int idr_interval;
    int profile;
    int preset;
    int avbr_accuracy;
    int avbr_convergence;
    int pic_timing_sei;
    int look_ahead;
    int look_ahead_depth;
    int look_ahead_downsampling;
    int vcm;
    int rdo;
    int max_frame_size;
    int max_slice_size;

    int tile_cols;
    int tile_rows;

    int aud;

    int single_sei_nal_unit;
    int max_dec_frame_buffering;

    int bitrate_limit;
    int mbbrc;
    int extbrc;
    int adaptive_i;
    int adaptive_b;
    int b_strategy;
    int cavlc;

    int int_ref_type;
    int int_ref_cycle_size;
    int int_ref_qp_delta;
    int recovery_point_sei;

    int repeat_pps;
    int low_power;
    int gpb;

    int a53_cc;
	AVDictionary *qsv_opts;

#if QSV_HAVE_MF
    int mfmode;
#endif
    char *load_plugins;
    SetEncodeCtrlCB *set_encode_ctrl_cb;
    int forced_idr;
} QSVEncContext;

int ff_qsv_enc_init(AVCodecContext *avctx, QSVEncContext *q);

int ff_qsv_encode(AVCodecContext *avctx, QSVEncContext *q,
                  AVPacket *pkt, const AVFrame *frame, int *got_packet);

int ff_qsv_enc_close(AVCodecContext *avctx, QSVEncContext *q);

/**
 * Verify a key that specifies a specific qsv parameter exists and its value
 * is valid
 */
int ff_qsv_validate_params(char *key, char *value);

/**
 * validate master display values provided as input and check if they are within HDR
 * range
 */
int ff_qsv_validate_display(char *value);

/**
 * Extract the display_color values once verified and store in an AVMasteringDisplayMetadata
 * struct
 */
int ff_qsv_extract_display_values(char *value, AVMasteringDisplayMetadata *aVMasteringDisplayMetadata);

/**
 * Verify content light level is within acceptable HDR range
 */
int ff_qsv_validate_cll(char *value);

/**
 * Parse and extract values from a string and store them in an array as integers
 */
int ff_qsv_extract_values(char *input_value, int *output_values);

/**
 * validate display's RGB and white point color levels
 * @param input_colors an array representation of the X and Y values of the color.
 */
int ff_qsv_validate_display_color_range(int *input_colors);

/**
 * validate display's luminousity
 * @param input_luminousity an array representation of the min and max values of the display luminousity.
 */

int ff_qsv_validate_display_luminousity_range(int *input_luminousity);

/**
 * Obtain video range value from the acceptable strings
 * @param value a predefined string defining the video color range as per ISO/IEC TR 23091-4.
 */

int ff_qsv_extract_range_value(char *value);

/**
 * Obtain source video light level and assign these values to AVContentLightMetadata reference
 * @param value a predefined string defining the light level.
 * @param light_meta AVContentLightMetadata reference.
 */
 int ff_qsv_get_content_light_level(char *value, AVContentLightMetadata *light_meta);

/**
 * convert AVCodecContext color range value to equivalent mfx color range
 * @param AVCodecContext color range
 */
int ff_avcol_range_to_mfx_col_range(int color_range);

/**
* Extract a substring from a string from a specified index
 * @param start_index, where the target substring offset begins
 * @param input_string, pointer to the first character of the original long string
 * @param ouput_string,  pointer to the first character of the string variable that will hold the result substring
 */
void ff_build_sub_string(char *input_string, char *ouput_string, int start_index);

#endif /* AVCODEC_QSVENC_H */

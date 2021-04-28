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

/**
 ** @file
 ** Hardware accelerated common filters based on Intel Quick Sync Video VPP
 **/

#include <float.h>

#include "libavutil/opt.h"
#include "libavutil/eval.h"
#include "libavutil/hwcontext.h"
#include "libavutil/pixdesc.h"
#include "libavutil/mathematics.h"

#include "formats.h"
#include "internal.h"
#include "avfilter.h"
#include "filters.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

#include "qsvvpp.h"
#include "transpose.h"

#define OFFSET(x) offsetof(VPPContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

/* number of video enhancement filters */
#define ENH_FILTERS_COUNT (8)
#define QSV_HAVE_ROTATION       QSV_VERSION_ATLEAST(1, 17)
#define QSV_HAVE_MIRRORING      QSV_VERSION_ATLEAST(1, 19)
#define QSV_HAVE_SCALING_CONFIG QSV_VERSION_ATLEAST(1, 19)

typedef struct VPPContext{
    QSVVPPContext qsv;

    /* Video Enhancement Algorithms */
    mfxExtVPPDeinterlacing  deinterlace_conf;
    mfxExtVPPFrameRateConversion frc_conf;
    mfxExtVPPDenoise denoise_conf;
    mfxExtVPPDetail detail_conf;
    mfxExtVPPProcAmp procamp_conf;
    mfxExtVPPRotation rotation_conf;
    mfxExtVPPMirroring mirroring_conf;
#ifdef QSV_HAVE_SCALING_CONFIG
    mfxExtVPPScaling scale_conf;
#endif

    /**
     * New dimensions. Special values are:
     *   0 = original width/height
     *  -1 = keep original aspect
     */
    int out_width;
    int out_height;
    /**
     * Output sw format. AV_PIX_FMT_NONE for no conversion.
     */
    enum AVPixelFormat out_format;

    AVRational framerate;       /* target framerate */
    int use_frc;                /* use framerate conversion */
    int deinterlace;            /* deinterlace mode : 0=off, 1=bob, 2=advanced */
    int denoise;                /* Enable Denoise algorithm. Value [0, 100] */
    int detail;                 /* Enable Detail Enhancement algorithm. */
                                /* Level is the optional, value [0, 100] */
    int use_crop;               /* 1 = use crop; 0=none */
    int crop_w;
    int crop_h;
    int crop_x;
    int crop_y;

    int transpose;
    int rotate;                 /* rotate angle : [0, 90, 180, 270] */
    int hflip;                  /* flip mode : 0 = off, 1 = HORIZONTAL flip */

    int scale_mode;             /* scale mode : 0 = auto, 1 = low power, 2 = high quality */

    /* param for the procamp */
    int    procamp;            /* enable procamp */
    float  hue;
    float  saturation;
    float  contrast;
    float  brightness;

    char *cx, *cy, *cw, *ch;
    char *ow, *oh;
    char *output_format_str;
} VPPContext;

static const char *const var_names[] = {
    "iw", "in_w",
    "ih", "in_h",
    "ow", "out_w", "w",
    "oh", "out_h", "h",
    "cw",
    "ch",
    "cx",
    "cy",
    "a", "dar",
    "sar",
    NULL
};

enum var_name {
    VAR_IW, VAR_IN_W,
    VAR_IH, VAR_IN_H,
    VAR_OW, VAR_OUT_W, VAR_W,
    VAR_OH, VAR_OUT_H, VAR_H,
    CW,
    CH,
    CX,
    CY,
    VAR_A, VAR_DAR,
    VAR_SAR,
    VAR_VARS_NB
};

static int eval_expr(AVFilterContext *ctx)
{
#define PASS_EXPR(e, s) {\
    if (s) {\
        ret = av_expr_parse(&e, s, var_names, NULL, NULL, NULL, NULL, 0, ctx); \
        if (ret < 0) {                                                  \
            av_log(ctx, AV_LOG_ERROR, "Error when passing '%s'.\n", s); \
            goto release;                                               \
        }                                                               \
    }\
}
#define CALC_EXPR(e, v, i, d) {\
    if (e)\
        i = v = av_expr_eval(e, var_values, NULL);      \
    else\
        i = v = d;\
}
    VPPContext *vpp = ctx->priv;
    double  var_values[VAR_VARS_NB] = { NAN };
    AVExpr *w_expr  = NULL, *h_expr  = NULL;
    AVExpr *cw_expr = NULL, *ch_expr = NULL;
    AVExpr *cx_expr = NULL, *cy_expr = NULL;
    int     ret = 0;

    PASS_EXPR(cw_expr, vpp->cw);
    PASS_EXPR(ch_expr, vpp->ch);

    PASS_EXPR(w_expr, vpp->ow);
    PASS_EXPR(h_expr, vpp->oh);

    PASS_EXPR(cx_expr, vpp->cx);
    PASS_EXPR(cy_expr, vpp->cy);

    var_values[VAR_IW] =
    var_values[VAR_IN_W] = ctx->inputs[0]->w;

    var_values[VAR_IH] =
    var_values[VAR_IN_H] = ctx->inputs[0]->h;

    var_values[VAR_A] = (double)var_values[VAR_IN_W] / var_values[VAR_IN_H];
    var_values[VAR_SAR] = ctx->inputs[0]->sample_aspect_ratio.num ?
        (double)ctx->inputs[0]->sample_aspect_ratio.num / ctx->inputs[0]->sample_aspect_ratio.den : 1;
    var_values[VAR_DAR] = var_values[VAR_A] * var_values[VAR_SAR];

    /* crop params */
    CALC_EXPR(cw_expr, var_values[CW], vpp->crop_w, var_values[VAR_IW]);
    CALC_EXPR(ch_expr, var_values[CH], vpp->crop_h, var_values[VAR_IH]);

    /* calc again in case cw is relative to ch */
    CALC_EXPR(cw_expr, var_values[CW], vpp->crop_w, var_values[VAR_IW]);

    CALC_EXPR(w_expr,
            var_values[VAR_OUT_W] = var_values[VAR_OW] = var_values[VAR_W],
            vpp->out_width, var_values[CW]);
    CALC_EXPR(h_expr,
            var_values[VAR_OUT_H] = var_values[VAR_OH] = var_values[VAR_H],
            vpp->out_height, var_values[CH]);

    /* calc again in case ow is relative to oh */
    CALC_EXPR(w_expr,
            var_values[VAR_OUT_W] = var_values[VAR_OW] = var_values[VAR_W],
            vpp->out_width, var_values[CW]);

    CALC_EXPR(cx_expr, var_values[CX], vpp->crop_x, (var_values[VAR_IW] - var_values[VAR_OW]) / 2);
    CALC_EXPR(cy_expr, var_values[CY], vpp->crop_y, (var_values[VAR_IH] - var_values[VAR_OH]) / 2);

    /* calc again in case cx is relative to cy */
    CALC_EXPR(cx_expr, var_values[CX], vpp->crop_x, (var_values[VAR_IW] - var_values[VAR_OW]) / 2);

    if ((vpp->crop_w != var_values[VAR_IW]) || (vpp->crop_h != var_values[VAR_IH]))
        vpp->use_crop = 1;

release:
    av_expr_free(w_expr);
    av_expr_free(h_expr);
    av_expr_free(cw_expr);
    av_expr_free(ch_expr);
    av_expr_free(cx_expr);
    av_expr_free(cy_expr);
#undef PASS_EXPR
#undef CALC_EXPR

    return ret;
}

static av_cold int vpp_preinit(AVFilterContext *ctx)
{
    VPPContext  *vpp  = ctx->priv;
    /* For AV_OPT_TYPE_STRING options, NULL is handled in other way so
     * we needn't set default value here
     */
    vpp->saturation = 1.0;
    vpp->contrast = 1.0;
    vpp->transpose = -1;

    return 0;
}

static av_cold int vpp_init(AVFilterContext *ctx)
{
    VPPContext  *vpp  = ctx->priv;

    if (!strcmp(vpp->output_format_str, "same")) {
        vpp->out_format = AV_PIX_FMT_NONE;
    } else {
        vpp->out_format = av_get_pix_fmt(vpp->output_format_str);
        if (vpp->out_format == AV_PIX_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Unrecognized output pixel format: %s\n", vpp->output_format_str);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    VPPContext      *vpp = ctx->priv;
    int              ret;
    int64_t          ow, oh;

    if (vpp->framerate.den == 0 || vpp->framerate.num == 0)
        vpp->framerate = inlink->frame_rate;

    if (av_cmp_q(vpp->framerate, inlink->frame_rate))
        vpp->use_frc = 1;

    ret = eval_expr(ctx);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "Fail to eval expr.\n");
        return ret;
    }

    ow = vpp->out_width;
    oh = vpp->out_height;

    /* sanity check params */
    if (ow <  -1 || oh <  -1) {
        av_log(ctx, AV_LOG_ERROR, "Size values less than -1 are not acceptable.\n");
        return AVERROR(EINVAL);
    }

    if (ow == -1 && oh == -1)
        vpp->out_width = vpp->out_height = 0;

    if (!(ow = vpp->out_width))
        ow = inlink->w;

    if (!(oh = vpp->out_height))
        oh = inlink->h;

    if (ow == -1)
        ow = av_rescale(oh, inlink->w, inlink->h);

    if (oh == -1)
        oh = av_rescale(ow, inlink->h, inlink->w);

    if (ow > INT_MAX || oh > INT_MAX ||
        (oh * inlink->w) > INT_MAX  ||
        (ow * inlink->h) > INT_MAX)
        av_log(ctx, AV_LOG_ERROR, "Rescaled value for width or height is too big.\n");

    vpp->out_width = ow;
    vpp->out_height = oh;

    if (vpp->use_crop) {
        vpp->crop_x = FFMAX(vpp->crop_x, 0);
        vpp->crop_y = FFMAX(vpp->crop_y, 0);

        if(vpp->crop_w + vpp->crop_x > inlink->w)
           vpp->crop_x = inlink->w - vpp->crop_w;
        if(vpp->crop_h + vpp->crop_y > inlink->h)
           vpp->crop_y = inlink->h - vpp->crop_h;
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    VPPContext      *vpp = ctx->priv;
    QSVVPPParam     param = { NULL };
    QSVVPPCrop      crop  = { 0 };
    mfxExtBuffer    *ext_buf[ENH_FILTERS_COUNT];
    AVFilterLink    *inlink = ctx->inputs[0];
    enum AVPixelFormat in_format;

    outlink->w          = vpp->out_width;
    outlink->h          = vpp->out_height;
    outlink->frame_rate = vpp->framerate;
    outlink->time_base  = inlink->time_base;

    param.filter_frame  = NULL;
    param.num_ext_buf   = 0;
    param.ext_buf       = ext_buf;

    if (inlink->format == AV_PIX_FMT_QSV) {
         if (!inlink->hw_frames_ctx || !inlink->hw_frames_ctx->data)
             return AVERROR(EINVAL);
         else
             in_format = ((AVHWFramesContext*)inlink->hw_frames_ctx->data)->sw_format;
    } else
        in_format = inlink->format;

    if (vpp->out_format == AV_PIX_FMT_NONE)
        vpp->out_format = in_format;
    param.out_sw_format  = vpp->out_format;

    if (vpp->use_crop) {
        crop.in_idx = 0;
        crop.x = vpp->crop_x;
        crop.y = vpp->crop_y;
        crop.w = vpp->crop_w;
        crop.h = vpp->crop_h;

        param.num_crop = 1;
        param.crop     = &crop;
    }

#define INIT_MFX_EXTBUF(extbuf, id) do { \
        memset(&vpp->extbuf, 0, sizeof(vpp->extbuf)); \
        vpp->extbuf.Header.BufferId = id; \
        vpp->extbuf.Header.BufferSz = sizeof(vpp->extbuf); \
        param.ext_buf[param.num_ext_buf++] = (mfxExtBuffer*)&vpp->extbuf; \
    } while (0)

#define SET_MFX_PARAM_FIELD(extbuf, field, value) do { \
        vpp->extbuf.field = value; \
    } while (0)

    if (vpp->deinterlace) {
        INIT_MFX_EXTBUF(deinterlace_conf, MFX_EXTBUFF_VPP_DEINTERLACING);
        SET_MFX_PARAM_FIELD(deinterlace_conf, Mode, (vpp->deinterlace == 1 ?
                            MFX_DEINTERLACING_BOB : MFX_DEINTERLACING_ADVANCED));
    }

    if (vpp->use_frc) {
        INIT_MFX_EXTBUF(frc_conf, MFX_EXTBUFF_VPP_FRAME_RATE_CONVERSION);
        SET_MFX_PARAM_FIELD(frc_conf, Algorithm, MFX_FRCALGM_DISTRIBUTED_TIMESTAMP);
    }

    if (vpp->denoise) {
        INIT_MFX_EXTBUF(denoise_conf, MFX_EXTBUFF_VPP_DENOISE);
        SET_MFX_PARAM_FIELD(denoise_conf, DenoiseFactor, vpp->denoise);
    }

    if (vpp->detail) {
        INIT_MFX_EXTBUF(detail_conf, MFX_EXTBUFF_VPP_DETAIL);
        SET_MFX_PARAM_FIELD(detail_conf, DetailFactor, vpp->detail);
    }

    if (vpp->procamp) {
        INIT_MFX_EXTBUF(procamp_conf, MFX_EXTBUFF_VPP_PROCAMP);
        SET_MFX_PARAM_FIELD(procamp_conf, Hue, vpp->hue);
        SET_MFX_PARAM_FIELD(procamp_conf, Saturation, vpp->saturation);
        SET_MFX_PARAM_FIELD(procamp_conf, Contrast, vpp->contrast);
        SET_MFX_PARAM_FIELD(procamp_conf, Brightness, vpp->brightness);
    }

    if (vpp->transpose >= 0) {
#ifdef QSV_HAVE_ROTATION
        switch (vpp->transpose) {
        case TRANSPOSE_CCLOCK_FLIP:
            vpp->rotate = MFX_ANGLE_270;
            vpp->hflip  = MFX_MIRRORING_HORIZONTAL;
            break;
        case TRANSPOSE_CLOCK:
            vpp->rotate = MFX_ANGLE_90;
            vpp->hflip  = MFX_MIRRORING_DISABLED;
            break;
        case TRANSPOSE_CCLOCK:
            vpp->rotate = MFX_ANGLE_270;
            vpp->hflip  = MFX_MIRRORING_DISABLED;
            break;
        case TRANSPOSE_CLOCK_FLIP:
            vpp->rotate = MFX_ANGLE_90;
            vpp->hflip  = MFX_MIRRORING_HORIZONTAL;
            break;
        case TRANSPOSE_REVERSAL:
            vpp->rotate = MFX_ANGLE_180;
            vpp->hflip  = MFX_MIRRORING_DISABLED;
            break;
        case TRANSPOSE_HFLIP:
            vpp->rotate = MFX_ANGLE_0;
            vpp->hflip  = MFX_MIRRORING_HORIZONTAL;
            break;
        case TRANSPOSE_VFLIP:
            vpp->rotate = MFX_ANGLE_180;
            vpp->hflip  = MFX_MIRRORING_HORIZONTAL;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "Failed to set transpose mode to %d.\n", vpp->transpose);
            return AVERROR(EINVAL);
        }
#else
        av_log(ctx, AV_LOG_WARNING, "The QSV VPP transpose option is "
            "not supported with this MSDK version.\n");
        vpp->transpose = 0;
#endif
    }

    if (vpp->rotate) {
#ifdef QSV_HAVE_ROTATION
        INIT_MFX_EXTBUF(rotation_conf, MFX_EXTBUFF_VPP_ROTATION);
        SET_MFX_PARAM_FIELD(rotation_conf, Angle, vpp->rotate);

        if (MFX_ANGLE_90 == vpp->rotate || MFX_ANGLE_270 == vpp->rotate) {
            FFSWAP(int, vpp->out_width, vpp->out_height);
            FFSWAP(int, outlink->w, outlink->h);
            av_log(ctx, AV_LOG_DEBUG, "Swap width and height for clock/cclock rotation.\n");
        }
#else
        av_log(ctx, AV_LOG_WARNING, "The QSV VPP rotate option is "
            "not supported with this MSDK version.\n");
        vpp->rotate = 0;
#endif
    }

    if (vpp->hflip) {
#ifdef QSV_HAVE_MIRRORING
        INIT_MFX_EXTBUF(mirroring_conf, MFX_EXTBUFF_VPP_MIRRORING);
        SET_MFX_PARAM_FIELD(mirroring_conf, Type, vpp->hflip);
#else
        av_log(ctx, AV_LOG_WARNING, "The QSV VPP hflip option is "
            "not supported with this MSDK version.\n");
        vpp->hflip = 0;
#endif
    }

    if (inlink->w != outlink->w || inlink->h != outlink->h) {
#ifdef QSV_HAVE_SCALING_CONFIG
        INIT_MFX_EXTBUF(scale_conf, MFX_EXTBUFF_VPP_SCALING);
        SET_MFX_PARAM_FIELD(scale_conf, ScalingMode, vpp->scale_mode);
#else
        av_log(ctx, AV_LOG_WARNING, "The QSV VPP Scale option is "
            "not supported with this MSDK version.\n");
#endif
    }

#undef INIT_MFX_EXTBUF
#undef SET_MFX_PARAM_FIELD

    if (vpp->use_frc || vpp->use_crop || vpp->deinterlace || vpp->denoise ||
        vpp->detail || vpp->procamp || vpp->rotate || vpp->hflip ||
        inlink->w != outlink->w || inlink->h != outlink->h || in_format != vpp->out_format)
        return ff_qsvvpp_init(ctx, &param);
    else {
        /* No MFX session is created in this case */
        av_log(ctx, AV_LOG_VERBOSE, "qsv vpp pass through mode.\n");
        if (inlink->hw_frames_ctx)
            outlink->hw_frames_ctx = av_buffer_ref(inlink->hw_frames_ctx);
    }

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    QSVVPPContext *qsv = ctx->priv;
    AVFrame *in = NULL;
    int ret, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    if (!qsv->eof) {
        ret = ff_inlink_consume_frame(inlink, &in);
        if (ret < 0)
            return ret;

        if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
            if (status == AVERROR_EOF) {
                qsv->eof = 1;
            }
        }
    }

    if (qsv->session) {
        if (in || qsv->eof) {
            ret = ff_qsvvpp_filter_frame(qsv, inlink, in);
            av_frame_free(&in);

            if (qsv->eof) {
                ff_outlink_set_status(outlink, status, pts);
                return 0;
            }

            if (qsv->got_frame) {
                qsv->got_frame = 0;
                return ret;
            }
        }
    } else {
        /* No MFX session is created in pass-through mode */
        if (in) {
            if (in->pts != AV_NOPTS_VALUE)
                in->pts = av_rescale_q(in->pts, inlink->time_base, outlink->time_base);

            ret = ff_filter_frame(outlink, in);
            return ret;
        }
    }

    if (qsv->eof) {
        ff_outlink_set_status(outlink, status, pts);
        return 0;
    } else {
        FF_FILTER_FORWARD_WANTED(outlink, inlink);
    }

    return FFERROR_NOT_READY;
}

static av_cold void vpp_uninit(AVFilterContext *ctx)
{
    ff_qsvvpp_close(ctx);
}

static const AVFilterPad vpp_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_input,
    },
};

static const AVFilterPad vpp_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

#define DEFINE_QSV_FILTER(x, sn, ln) \
static const AVClass x##_class = { \
    .class_name = #sn "_qsv", \
    .item_name  = av_default_item_name, \
    .option     = x##_options, \
    .version    = LIBAVUTIL_VERSION_INT, \
}; \
const AVFilter ff_vf_##sn##_qsv = { \
    .name           = #sn "_qsv", \
    .description    = NULL_IF_CONFIG_SMALL("Quick Sync Video " #ln), \
    .preinit        = vpp_preinit, \
    .init           = vpp_init, \
    .uninit         = vpp_uninit, \
    .query_formats  = x##_query_formats, \
    .priv_size      = sizeof(VPPContext), \
    .priv_class     = &x##_class, \
    FILTER_INPUTS(vpp_inputs), \
    FILTER_OUTPUTS(vpp_outputs), \
    .activate       = activate, \
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE, \
};

static const AVOption vpp_options[] = {
    { "deinterlace", "deinterlace mode: 0=off, 1=bob, 2=advanced", OFFSET(deinterlace), AV_OPT_TYPE_INT,      { .i64 = 0 }, 0, MFX_DEINTERLACING_ADVANCED, .flags = FLAGS, "deinterlace" },
    { "bob",         "Bob deinterlace mode.",                      0,                   AV_OPT_TYPE_CONST,    { .i64 = MFX_DEINTERLACING_BOB },            .flags = FLAGS, "deinterlace" },
    { "advanced",    "Advanced deinterlace mode. ",                0,                   AV_OPT_TYPE_CONST,    { .i64 = MFX_DEINTERLACING_ADVANCED },       .flags = FLAGS, "deinterlace" },

    { "denoise",     "denoise level [0, 100]",       OFFSET(denoise),     AV_OPT_TYPE_INT,      { .i64 = 0 }, 0, 100, .flags = FLAGS },
    { "detail",      "enhancement level [0, 100]",   OFFSET(detail),      AV_OPT_TYPE_INT,      { .i64 = 0 }, 0, 100, .flags = FLAGS },
    { "framerate",   "output framerate",             OFFSET(framerate),   AV_OPT_TYPE_RATIONAL, { .dbl = 0.0 },0, DBL_MAX, .flags = FLAGS },
    { "procamp",     "Enable ProcAmp",               OFFSET(procamp),     AV_OPT_TYPE_INT,      { .i64 = 0 }, 0, 1, .flags = FLAGS},
    { "hue",         "ProcAmp hue",                  OFFSET(hue),         AV_OPT_TYPE_FLOAT,    { .dbl = 0.0 }, -180.0, 180.0, .flags = FLAGS},
    { "saturation",  "ProcAmp saturation",           OFFSET(saturation),  AV_OPT_TYPE_FLOAT,    { .dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS},
    { "contrast",    "ProcAmp contrast",             OFFSET(contrast),    AV_OPT_TYPE_FLOAT,    { .dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS},
    { "brightness",  "ProcAmp brightness",           OFFSET(brightness),  AV_OPT_TYPE_FLOAT,    { .dbl = 0.0 }, -100.0, 100.0, .flags = FLAGS},

    { "transpose",  "set transpose direction",       OFFSET(transpose),   AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, 6, FLAGS, "transpose"},
        { "cclock_hflip",  "rotate counter-clockwise with horizontal flip",  0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CCLOCK_FLIP }, .flags=FLAGS, .unit = "transpose" },
        { "clock",         "rotate clockwise",                               0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CLOCK       }, .flags=FLAGS, .unit = "transpose" },
        { "cclock",        "rotate counter-clockwise",                       0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CCLOCK      }, .flags=FLAGS, .unit = "transpose" },
        { "clock_hflip",   "rotate clockwise with horizontal flip",          0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CLOCK_FLIP  }, .flags=FLAGS, .unit = "transpose" },
        { "reversal",      "rotate by half-turn",                            0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_REVERSAL    }, .flags=FLAGS, .unit = "transpose" },
        { "hflip",         "flip horizontally",                              0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_HFLIP       }, .flags=FLAGS, .unit = "transpose" },
        { "vflip",         "flip vertically",                                0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_VFLIP       }, .flags=FLAGS, .unit = "transpose" },

    { "cw",   "set the width crop area expression",   OFFSET(cw), AV_OPT_TYPE_STRING, { .str = "iw" }, 0, 0, FLAGS },
    { "ch",   "set the height crop area expression",  OFFSET(ch), AV_OPT_TYPE_STRING, { .str = "ih" }, 0, 0, FLAGS },
    { "cx",   "set the x crop area expression",       OFFSET(cx), AV_OPT_TYPE_STRING, { .str = "(in_w-out_w)/2" }, 0, 0, FLAGS },
    { "cy",   "set the y crop area expression",       OFFSET(cy), AV_OPT_TYPE_STRING, { .str = "(in_h-out_h)/2" }, 0, 0, FLAGS },

    { "w",      "Output video width(0=input video width, -1=keep input video aspect)",  OFFSET(ow), AV_OPT_TYPE_STRING, { .str="cw" }, 0, 255, .flags = FLAGS },
    { "width",  "Output video width(0=input video width, -1=keep input video aspect)",  OFFSET(ow), AV_OPT_TYPE_STRING, { .str="cw" }, 0, 255, .flags = FLAGS },
    { "h",      "Output video height(0=input video height, -1=keep input video aspect)", OFFSET(oh), AV_OPT_TYPE_STRING, { .str="w*ch/cw" }, 0, 255, .flags = FLAGS },
    { "height", "Output video height(0=input video height, -1=keep input video aspect)", OFFSET(oh), AV_OPT_TYPE_STRING, { .str="w*ch/cw" }, 0, 255, .flags = FLAGS },
    { "format", "Output pixel format", OFFSET(output_format_str), AV_OPT_TYPE_STRING, { .str = "same" }, .flags = FLAGS },
    { "async_depth", "Internal parallelization depth, the higher the value the higher the latency.", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
#if QSV_HAVE_SCALING_CONFIG
    { "scale_mode", "scale mode", OFFSET(scale_mode), AV_OPT_TYPE_INT, { .i64 = MFX_SCALING_MODE_DEFAULT }, MFX_SCALING_MODE_DEFAULT, MFX_SCALING_MODE_QUALITY, .flags = FLAGS, "scale mode" },
    { "auto",      "auto mode",             0,    AV_OPT_TYPE_CONST,  { .i64 = MFX_SCALING_MODE_DEFAULT},  INT_MIN, INT_MAX, FLAGS, "scale mode"},
    { "low_power", "low power mode",        0,    AV_OPT_TYPE_CONST,  { .i64 = MFX_SCALING_MODE_LOWPOWER}, INT_MIN, INT_MAX, FLAGS, "scale mode"},
    { "hq",        "high quality mode",     0,    AV_OPT_TYPE_CONST,  { .i64 = MFX_SCALING_MODE_QUALITY},  INT_MIN, INT_MAX, FLAGS, "scale mode"},
#else
    { "scale_mode", "(not supported)",        OFFSET(scale_mode), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS, "scale mode" },
    { "auto",      "",                      0,    AV_OPT_TYPE_CONST,  { .i64 = 0}, 0,   0,     FLAGS, "scale mode"},
    { "low_power", "",                      0,    AV_OPT_TYPE_CONST,  { .i64 = 1}, 0,   0,     FLAGS, "scale mode"},
    { "hq",        "",                      0,    AV_OPT_TYPE_CONST,  { .i64 = 2}, 0,   0,     FLAGS, "scale mode"},
#endif

    { NULL }
};

static int vpp_query_formats(AVFilterContext *ctx)
{
    int ret;
    static const enum AVPixelFormat in_pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_RGB32,
        AV_PIX_FMT_QSV,
        AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat out_pix_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_P010,
        AV_PIX_FMT_QSV,
        AV_PIX_FMT_NONE
    };

    ret = ff_formats_ref(ff_make_format_list(in_pix_fmts),
                         &ctx->inputs[0]->outcfg.formats);
    if (ret < 0)
        return ret;
    return ff_formats_ref(ff_make_format_list(out_pix_fmts),
                          &ctx->outputs[0]->incfg.formats);
}

DEFINE_QSV_FILTER(vpp, vpp, "VPP");

static int qsvscale_query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pixel_formats[] = {
        AV_PIX_FMT_QSV, AV_PIX_FMT_NONE,
    };

    return ff_set_common_formats_from_list(ctx, pixel_formats);
}

static const AVOption qsvscale_options[] = {
    { "w",      "Output video width(0=input video width, -1=keep input video aspect)",  OFFSET(ow), AV_OPT_TYPE_STRING, { .str = "iw"   }, .flags = FLAGS },
    { "h",      "Output video height(0=input video height, -1=keep input video aspect)", OFFSET(oh), AV_OPT_TYPE_STRING, { .str = "ih"   }, .flags = FLAGS },
    { "cw",     "set the width crop area expression",   OFFSET(cw), AV_OPT_TYPE_STRING, { .str = "iw" }, .flags = FLAGS },
    { "ch",     "set the height crop area expression",  OFFSET(ch), AV_OPT_TYPE_STRING, { .str = "ih" }, .flags = FLAGS },
    { "cx",     "set the x crop area expression",       OFFSET(cx), AV_OPT_TYPE_STRING, { .str = "(iw-ow)/2" }, .flags = FLAGS },
    { "cy",     "set the y crop area expression",       OFFSET(cy), AV_OPT_TYPE_STRING, { .str = "(ih-oh)/2" }, .flags = FLAGS },
    { "format", "Output pixel format", OFFSET(output_format_str), AV_OPT_TYPE_STRING, { .str = "same" }, .flags = FLAGS },

#if QSV_HAVE_SCALING_CONFIG
    { "mode",      "set scaling mode",    OFFSET(scale_mode),    AV_OPT_TYPE_INT,    { .i64 = MFX_SCALING_MODE_DEFAULT}, MFX_SCALING_MODE_DEFAULT, MFX_SCALING_MODE_QUALITY, FLAGS, "mode"},
    { "low_power", "low power mode",        0,             AV_OPT_TYPE_CONST,  { .i64 = MFX_SCALING_MODE_LOWPOWER}, INT_MIN, INT_MAX, FLAGS, "mode"},
    { "hq",        "high quality mode",     0,             AV_OPT_TYPE_CONST,  { .i64 = MFX_SCALING_MODE_QUALITY},  INT_MIN, INT_MAX, FLAGS, "mode"},
#else
    { "mode",      "(not supported)",     OFFSET(scale_mode),    AV_OPT_TYPE_INT,    { .i64 = 0}, 0, INT_MAX, FLAGS, "mode"},
    { "low_power", "",                      0,             AV_OPT_TYPE_CONST,  { .i64 = 1}, 0,   0,     FLAGS, "mode"},
    { "hq",        "",                      0,             AV_OPT_TYPE_CONST,  { .i64 = 2}, 0,   0,     FLAGS, "mode"},
#endif

    { "async_depth", "Internal parallelization depth, the higher the value the higher the latency.", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },

    { NULL },
};

DEFINE_QSV_FILTER(qsvscale, scale, "scaling and format conversion")

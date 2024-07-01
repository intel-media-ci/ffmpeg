/*
 * Copyright (c) 2024
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

/* Install numpy: pip install numpy
 * python script example:
 *  ffmpeg-python.py:
 *
 * offset = 0
 * def init(init_str=None):
 *   global offset
 *   offset = int(init_str)
 *
 * def filter_func(frame):
 *   new_frame = []
 *   for plane in frame:
 *     new_frame.append(plane + offset)
 *   return tuple(new_frame)
 *
 *
 *
 * Add the path of the script file into environment variable PYTHONPATH.
 * ffmpeg command:
 * ffmpeg -i input.264 -vf format=rgb24, \
 *   video_python=module=ffmpeg-python:init_func=init:init_param="10": \
 *   filter_func=filter_func output.mp4
 *
 */

#include "avfilter.h"
#include "filters.h"
#include "internal.h"
#include "video.h"
#include "libavutil/fifo.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libswscale/swscale.h"
#include "Python.h"
#include "numpy/arrayobject.h"

typedef struct PythonContext {
    const AVClass *class;
    char *module_name;
    char *init_func;
    char *init_param;
    char *filter_func;
    PyObject *py_module;
    PyObject *py_func;
    AVFifo *frame_queue;
    int passthrough;
    int eof;
    PyGILState_STATE state;
} PythonContext;

#define OFFSET(x) offsetof(PythonContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption video_python_options[] = {
    { "module",        "module name to import",          OFFSET(module_name), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { "init_func",     "intialization function to call", OFFSET(init_func),   AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { "init_param",    "a string passed to init func",   OFFSET(init_param),  AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { "filter_func",   "filter function to call",        OFFSET(filter_func), AV_OPT_TYPE_STRING, {.str="filter"}, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(video_python);

static av_cold int init(AVFilterContext *context)
{
    int initialized = 0;
    PyObject *py_name = NULL;
    PythonContext *python_context = context->priv;

    if (python_context->py_module)
        // filter has already been initialized
        return 0;

    if (!Py_IsInitialized()) {
        Py_Initialize();
        initialized = 1;
    } else
        python_context->state = PyGILState_Ensure ();
    if (PyArray_API == NULL)
        import_array1(AVERROR_EXTERNAL);

    py_name = PyUnicode_DecodeFSDefault(python_context->module_name);
    if (!py_name) {
        PyErr_Print();
        av_log(python_context, AV_LOG_ERROR, "Failed to decode %s\n",
               python_context->module_name);
        return AVERROR(EINVAL);
    }

    python_context->py_module = PyImport_Import(py_name);
    Py_DECREF(py_name);
    if (!python_context->py_module) {
        PyErr_Print();
        av_log(python_context, AV_LOG_ERROR, "Failed to import %s\n",
               python_context->module_name);
        return AVERROR(EINVAL);
    }

    // call initialize function if init_func is set
    if (python_context->init_func) {
        PyObject *p_args = NULL, *py_value = NULL, *py_init_func = NULL;
        // Get init function
        py_init_func = PyObject_GetAttrString(python_context->py_module,
                                             python_context->init_func);
        if (!py_init_func || !PyCallable_Check(py_init_func)) {
            PyErr_Print();
            Py_DECREF(py_init_func);
            av_log(python_context, AV_LOG_ERROR, "Cannot find init function %s\n",
                python_context->init_func);
            return AVERROR(EINVAL);
        }
        // If init_param is set, pass it to init function
        if (python_context->init_param) {
            p_args = Py_BuildValue("(s)", python_context->init_param);
            if (!p_args) {
                PyErr_Print();
                Py_DECREF(py_init_func);
                av_log(python_context, AV_LOG_ERROR, "Failed to set config code %s\n",
                    python_context->init_param);
                return AVERROR(EINVAL);
            }
        }
        if (p_args) {
            py_value = PyObject_CallObject(py_init_func, p_args);
            Py_DECREF(p_args);
        } else
            py_value = PyObject_CallObject(py_init_func, NULL);
        Py_DECREF(py_init_func);
        if (!py_value) {
            PyErr_Print();
            av_log(python_context, AV_LOG_ERROR, "Failed to run init func %s\n",
                python_context->init_func);
            return AVERROR_EXTERNAL;
        }
        Py_DECREF(py_value);
    }
    // Get filter function
    python_context->py_func = PyObject_GetAttrString(python_context->py_module,
                                                    python_context->filter_func);
    if (!python_context->py_func || !PyCallable_Check(python_context->py_func)) {
        PyErr_Print();
        av_log(python_context, AV_LOG_ERROR, "Cannot find function %s\n",
               python_context->filter_func);
        return AVERROR(EINVAL);
    }

    python_context->frame_queue = av_fifo_alloc2(1, sizeof(AVFrame *), AV_FIFO_FLAG_AUTO_GROW);
    if (!python_context->frame_queue)
        return AVERROR(ENOMEM);

    if (initialized)
        PyEval_SaveThread();
    else
        PyGILState_Release(python_context->state);
    python_context->state = 0;
    return 0;
}

static int depth_to_numpy_type(int depth, int is_float)
{
    if (is_float) {
        if (depth == 16)
            return NPY_FLOAT16;
        else
            return NPY_FLOAT32;
    } else {
        if (depth <= 8)
            return NPY_UINT8;
        else
            return NPY_UINT16;
    }
    return NPY_UINT8;
}

static int create_plane_in_tuple(PyObject *py_planes, int index,
                                 int width, int height, int channel,
                                 int npy_type, void *data)
{
    int ret = 0;
    npy_intp dims[3] = { 0 };
    PyObject *py_numpy_plane = NULL;

    dims[0] = height;
    dims[1] = width;
    dims[2] = channel;
    py_numpy_plane = PyArray_SimpleNewFromData(3, dims, npy_type, data);
    if (!py_numpy_plane) {
        PyErr_Print();
        return AVERROR_EXTERNAL;
    }
    if (PyTuple_SetItem(py_planes, index, py_numpy_plane)) {
        PyErr_Print();
        Py_DECREF(py_numpy_plane);
        ret = AVERROR_EXTERNAL;
    }
    return ret;
}

static int create_tuple_from_frame(AVFrame *frame, PyObject **py_frame)
{
    /*
     * frame will represented as
     * Tuple(numpy.ndarray, numpy.ndarray, numpy.ndarray)
     */
    const AVPixFmtDescriptor *desc = NULL;
    int ret = 0;
    PyObject *py_planes = NULL;

    desc = av_pix_fmt_desc_get(frame->format);
    if (!desc || desc->flags & AV_PIX_FMT_FLAG_HWACCEL)
        return AVERROR(EINVAL);

    if (desc->flags & AV_PIX_FMT_FLAG_PAL) {
        int npy_type = depth_to_numpy_type(desc->comp[0].depth,
                                           desc->flags & AV_PIX_FMT_FLAG_FLOAT);
        py_planes = PyTuple_New(2);
        if (!py_planes) {
            PyErr_Print();
            return AVERROR_EXTERNAL;
        }
        ret = create_plane_in_tuple(py_planes, 0, frame->width, frame->height, 1,
                                    npy_type, frame->data[0]);
        if (ret < 0)
            goto err;
        if (frame->data[1]) {
            ret = create_plane_in_tuple(py_planes, 1, 4 * 256, 1, 1, NPY_UINT8,
                                       frame->data[0]);
            if (ret < 0)
                goto err;
        }
    } else {
        int i, planes_nb = 0, npy_type, byte_depth;

        for (i = 0; i < desc->nb_components; i++)
            planes_nb = FFMAX(planes_nb, desc->comp[i].plane + 1);

        py_planes = PyTuple_New(planes_nb);
        if (!py_planes) {
            PyErr_Print();
            return AVERROR_EXTERNAL;
        }
        npy_type = depth_to_numpy_type(desc->comp[0].depth,
                                desc->flags & AV_PIX_FMT_FLAG_FLOAT);
        byte_depth = desc->comp[0].depth / 8;
        byte_depth += desc->comp[0].depth % 8 == 0 ? 0 : 1;

        for (i = 0; i < planes_nb; i++) {
            int h = frame->height, w = frame->width, c = 0;
            int max_step     [4];
            int max_step_comp[4];
            av_image_fill_max_pixsteps(max_step, max_step_comp, desc);
            if (i == 1 || i == 2) {
                h = AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h);
                w = AV_CEIL_RSHIFT(frame->width,  desc->log2_chroma_w);
            }
            c = max_step[max_step_comp[i]] / byte_depth;
            if (desc->flags & AV_PIX_FMT_FLAG_BITSTREAM)
                c = 1;
            ret = create_plane_in_tuple(py_planes, i, w, h, c, npy_type, frame->data[i]);
            if (ret < 0)
                goto err;
        }
    }
    *py_frame = py_planes;
    return 0;
err:
    if (py_planes)
        Py_DECREF(py_planes);
    return ret;
}

static PyObject *pass_frame_to_python_func(PyObject *py_func, PyObject *py_frame)
{
    int ret;
    PyObject *py_value = NULL, *py_args = NULL;
    py_args = PyTuple_New(1);
    if (!py_args) {
        PyErr_Print();
        return NULL;
    }

    if (py_frame)
        ret = PyTuple_SetItem(py_args, 0, py_frame);
    else
        ret = PyTuple_SetItem(py_args, 0, Py_None);
    if (ret < 0) {
        PyErr_Print();
        Py_DECREF(py_args);
        return NULL;
    }

    py_value = PyObject_CallObject(py_func, py_args);
    if (!py_value)
        PyErr_Print();
    Py_DECREF(py_args);
    return py_value;
}

static int get_contiguous_array(PyObject *py_frame)
{
    int nb_planes, i, ret;
    PyObject *py_numpy_plane = NULL;
    PyArrayObject *py_contiguous_array = NULL;

    nb_planes = PyTuple_Size(py_frame);
    for (i = 0; i < nb_planes; i++) {
        py_numpy_plane = PyTuple_GetItem(py_frame, i);
        if (!PyArray_Check(py_numpy_plane))
            return AVERROR_EXTERNAL;
        py_contiguous_array = PyArray_GETCONTIGUOUS((PyArrayObject *)py_numpy_plane);
        if (!py_contiguous_array)
            return AVERROR_EXTERNAL;

        ret = PyTuple_SetItem(py_frame, i, (PyObject *)py_contiguous_array);
        py_numpy_plane = NULL;
        if (ret < 0) {
            Py_DECREF(py_contiguous_array);
            return AVERROR_EXTERNAL;
        }
    }
    return 0;
}

static int send_frame_to_output(PythonContext *python_context, AVFilterLink *outlink,
                                PyObject *py_value, AVFrame *out_frame)
{
    AVFrame *in_frame = NULL;
    int nb_planes, i, ret, ndims;
    int src_linesizes[4] = { 0 };
    uint8_t *src_data[4] = { 0 };

    ret = get_contiguous_array(py_value);
    if (ret < 0)
        return ret;

    nb_planes = PyTuple_Size(py_value);
    for (i = 0; i < nb_planes; i++) {
        PyObject *py_numpy_plane = NULL;
        npy_intp *shape = NULL;
        py_numpy_plane = PyTuple_GetItem(py_value, i);
        if (!PyArray_Check(py_numpy_plane))
            return AVERROR_EXTERNAL;
        ndims = PyArray_NDIM(py_numpy_plane);
        if (ndims != 3) {
            av_log(python_context, AV_LOG_ERROR, "Wrong output format: plane %d has ndim: %d\n", i, ndims);
            return AVERROR_EXTERNAL;
        }
        shape = PyArray_DIMS(py_numpy_plane);
        src_linesizes[i] = shape[1] * shape[2];
        src_data[i] = PyArray_DATA(py_numpy_plane);
    }
    out_frame = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out_frame)
        return AVERROR(ENOMEM);

    if (!av_fifo_can_read(python_context->frame_queue)) {
        av_frame_free(&out_frame);
        av_log(python_context, AV_LOG_ERROR, "No frame in queue, the frame number "
                                "of input and output are different\n");
        return AVERROR_BUG2;
    }
    ret = av_fifo_read(python_context->frame_queue, &in_frame, 1);
    if (ret < 0) {
        av_frame_free(&out_frame);
        return ret;
    }
    av_frame_copy_props(out_frame, in_frame);
    av_frame_free(&in_frame);
    av_image_copy(out_frame->data, out_frame->linesize,
                    (const uint8_t * const*)src_data, src_linesizes,
                    out_frame->format, out_frame->width, out_frame->height);
    return ff_filter_frame(outlink, out_frame);
}

static int process_return_value(AVFilterContext *context, PyObject *py_value)
{
    /*
     * returned value can be:
     * 1. int. need more data or passthrough input frame
     * 2. Tuple(np.ndarray, np.ndarray ...). one frame
     * 3. List[Tuple, Tuple, ...] multiple frames
     */
    AVFrame *out_frame = NULL, *in_frame = NULL;
    AVFilterLink *outlink = context->outputs[0];
    PythonContext *python_context = context->priv;
    int ret;
    if (PyLong_Check(py_value)) {
        ret = PyLong_AsLong(py_value);
        if (ret < 0) {
            av_log(context, AV_LOG_ERROR, "Filter function return error: %d\n", ret);
            return AVERROR(EINVAL);
        }

        if (!python_context->passthrough) {
            // Need more data
            return 0;
        }
        // Passthrough
        if (!av_fifo_can_read(python_context->frame_queue)) {
            av_log(context, AV_LOG_ERROR, "No frame in queue, the frame number "
                                          "of input and output are different\n");
            return AVERROR_BUG2;
        }
        ret = av_fifo_read(python_context->frame_queue, &in_frame, 1);
        if (ret < 0)
            return ret;
        return ff_filter_frame(outlink, in_frame);
    } else if (PyList_Check(py_value)) {
        // Get multiple output frame.
        int nb_frame, ret;
        nb_frame = PyList_Size(py_value);
        for (int i = 0; i < nb_frame; i++) {
            PyObject *py_frame = NULL;
            py_frame = PyList_GetItem(py_value, i);
            if (!py_frame) {
                PyErr_Print();
                return AVERROR_EXTERNAL;
            }
            ret = send_frame_to_output(python_context, outlink, py_frame, out_frame);
            Py_DECREF(py_frame);
            if (ret < 0)
                return ret;
        }
    } else if (PyTuple_Check(py_value))
        // Get one output frame
        return send_frame_to_output(python_context, outlink, py_value, out_frame);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFrame *empty_frame = NULL;
    AVFilterContext *context = outlink->src;
    AVFilterLink *inlink = context->inputs[0];
    int ret, ndims, nb_planes;
    npy_intp *shape = NULL;
    PyObject *py_value = NULL, *py_frame = NULL, *py_plane = NULL;
    PythonContext *python_context = context->priv;

    python_context->state = PyGILState_Ensure();
    empty_frame = av_frame_alloc();
    if (!empty_frame)
        return AVERROR(ENOMEM);

    empty_frame->format = inlink->format;
    empty_frame->width  = inlink->w;
    empty_frame->height = inlink->h;
    ret = av_frame_get_buffer(empty_frame, 0);
    if (ret < 0) {
        av_frame_free(&empty_frame);
        return ret;
    }

    ret = create_tuple_from_frame(empty_frame, &py_frame);
    if (ret < 0) {
        av_log(context, AV_LOG_ERROR, "Failed to create py_frame\n");
        av_frame_free(&empty_frame);
        return AVERROR_EXTERNAL;
    }

    py_value = pass_frame_to_python_func(python_context->py_func, py_frame);
    if (py_value != py_frame)
        // python function doesn't return input frame
        Py_DECREF(py_frame);
    av_frame_free(&empty_frame);
    if (!py_value) {
        PyErr_Print();
        av_log(context, AV_LOG_ERROR, "Failed to call function: %s\n", python_context->filter_func);
        return AVERROR_EXTERNAL;
    }
    /*
     * returned value can be:
     * 1. int. need more data or passthrough input frame
     * 2. Tuple(np.ndarray, np.ndarray ...). one frame
     * 3. List[Tuple, Tuple, ...] multiple frames
     */

    // Check returned value
    if (PyLong_Check(py_value)) {
        ret = PyLong_AsLong(py_value);
        Py_DECREF(py_value);
        if (ret < 0) {
            av_log(context, AV_LOG_ERROR, "Filter function return error: %d\n", ret);
            return AVERROR(EINVAL);
        }
        // Flush
        py_value = pass_frame_to_python_func(python_context->py_func, NULL);
        if (!py_value) {
            PyErr_Print();
            av_log(context, AV_LOG_ERROR, "Failed to call function: %s\n", python_context->filter_func);
            return AVERROR_EXTERNAL;
        }
        // If still no output frame, it is passthrough mode
        if (PyLong_Check(py_value)) {
            ret = PyLong_AsLong(py_value);
            Py_DECREF(py_value);
            if (ret < 0) {
                av_log(context, AV_LOG_ERROR, "Filter function return error: %d\n", ret);
                return AVERROR(EINVAL);
            }
            python_context->passthrough = 1;
            return 0;
        }
    }
    // If get a list of frame, choose the first frame
    if (PyList_Check(py_value)) {
        int nb_frames;
        nb_frames = PyList_Size(py_value);
        if (nb_frames < 0) {
            Py_DECREF(py_value);
            av_log(context, AV_LOG_ERROR, "Wrong output format: empty list\n");
            return AVERROR(EINVAL);
        }
        py_frame = PyList_GetItem(py_value, 0);
        if (!py_frame) {
            Py_DECREF(py_value);
            PyErr_Print();
            return AVERROR_EXTERNAL;
        }
        Py_IncRef(py_frame);
        Py_DECREF(py_value);
    } else
        // Get one frame
        py_frame = py_value;

    if (!PyTuple_Check(py_frame)) {
        Py_DECREF(py_frame);
        av_log(context, AV_LOG_ERROR, "Wrong output format, output is not Tuple\n");
        return AVERROR(EINVAL);
    }
    nb_planes = PyTuple_Size(py_frame);
    if (nb_planes <= 0) {
        Py_DECREF(py_frame);
        av_log(context, AV_LOG_ERROR, "Wrong output format: output Tuple size: %d\n", nb_planes);
        return AVERROR(EINVAL);
    }
    py_plane = PyTuple_GetItem(py_frame, 0);
    if (!PyArray_Check(py_plane)) {
        Py_DECREF(py_frame);
        av_log(context, AV_LOG_ERROR, "Wrong output format, output plane is not numpy.ndarray\n");
        return AVERROR(EINVAL);
    }
    ndims = PyArray_NDIM(py_plane);
    if (ndims != 3) {
        Py_DECREF(py_frame);
        av_log(context, AV_LOG_ERROR, "Wrong output format: plane 0 has ndim: %d\n", ndims);
        return AVERROR(EINVAL);
    }
    shape = PyArray_DIMS(py_plane);
    outlink->w = shape[1];
    outlink->h = shape[0];
    Py_DECREF(py_frame);
    PyGILState_Release(python_context->state);
    python_context->state = 0;
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in_frame)
{
    AVFilterContext *context = inlink->dst;
    AVFrame *no_align_frame = NULL;
    int ret = 0;
    PythonContext *python_context = context->priv;
    PyObject *py_value = NULL, *py_frame = NULL;

    no_align_frame = av_frame_alloc();
    if (!no_align_frame)
        return AVERROR(ENOMEM);

    no_align_frame->format = in_frame->format;
    no_align_frame->width  = in_frame->width;
    no_align_frame->height = in_frame->height;
    ret = av_frame_get_buffer(no_align_frame, 0);
    if (ret < 0) {
        av_frame_free(&no_align_frame);
        return ret;
    }

    ret = av_frame_copy(no_align_frame, in_frame);
    if (ret < 0) {
        av_frame_free(&no_align_frame);
        return ret;
    }

    ret = av_fifo_write(python_context->frame_queue, &in_frame, 1);
    if (ret < 0)
        return ret;

    python_context->state = PyGILState_Ensure();
    ret = create_tuple_from_frame(no_align_frame, &py_frame);
    if (ret < 0) {
        av_log(context, AV_LOG_ERROR, "Failed to create py_frame\n");
        av_frame_free(&no_align_frame);
        return AVERROR_EXTERNAL;
    }

    py_value = pass_frame_to_python_func(python_context->py_func, py_frame);
    if (!py_value) {
        Py_DECREF(py_frame);
        av_frame_free(&no_align_frame);
        PyErr_Print();
        av_log(context, AV_LOG_ERROR, "Failed to call function: %s\n", python_context->filter_func);
        return AVERROR_EXTERNAL;
    }

    ret = process_return_value(context, py_value);
    Py_DECREF(py_frame);
    av_frame_free(&no_align_frame);
    Py_DECREF(py_value);
    if (ret < 0) {
        av_log(context, AV_LOG_ERROR, "Failed to process return value\n");
        return ret;
    }
    PyGILState_Release(python_context->state);
    python_context->state = 0;
    return 0;
}


static int request_frame(AVFilterLink *link)
{
    AVFilterContext *context = link->src;
    PythonContext *python_context = context->priv;
    PyObject *py_value = NULL;
    int ret;

    if (python_context->eof)
        return AVERROR_EOF;

    python_context->state = PyGILState_Ensure ();

    ret = ff_request_frame(link->src->inputs[0]);
    if (ret == AVERROR_EOF) {
        python_context->eof = 1;
        if (!av_fifo_can_read(python_context->frame_queue))
            return 0;
        py_value = pass_frame_to_python_func(python_context->py_func, NULL);
        if (!py_value) {
            PyErr_Print();
            av_log(context, AV_LOG_ERROR, "Failed to call function: %s\n", python_context->filter_func);
            return AVERROR_EXTERNAL;
        }

        ret = process_return_value(context, py_value);
        Py_DECREF(py_value);
        if (ret < 0) {
            av_log(context, AV_LOG_ERROR, "Failed to process return value\n");
            return ret;
        }
        return 0;
    }
    PyGILState_Release(python_context->state);
    python_context->state = 0;

    return ret;
}

static void uninit(AVFilterContext *context)
{
    PythonContext *python_context = context->priv;
    if (python_context->py_func)
        Py_DECREF(python_context->py_func);
    python_context->py_func = NULL;
    if (python_context->py_module)
        Py_DECREF(python_context->py_module);
    python_context->py_module = NULL;
    if (python_context->state)
        PyGILState_Release(python_context->state);
    python_context->state = 0;
    return;
}

static AVFrame *python_get_video_buffer(AVFilterLink *inlink, int w, int h)
{
    return ff_default_get_video_buffer2(inlink, w, h, 0);
}

static const AVFilterPad video_python_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .filter_frame  = filter_frame,
    },
};

static const AVFilterPad video_python_outputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .config_props     = config_output,
        .get_buffer.video = python_get_video_buffer,
        .request_frame    = request_frame,
    },
};

const AVFilter ff_vf_video_python = {
    .name          = "video_python",
    .description   = NULL_IF_CONFIG_SMALL("Python Video filter."),
    .priv_size     = sizeof(PythonContext),
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(video_python_inputs),
    FILTER_OUTPUTS(video_python_outputs),
    .priv_class    = &video_python_class,
};

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

#ifndef AVUTIL_HWCONTEXT_QSV_H
#define AVUTIL_HWCONTEXT_QSV_H

#include <mfxvideo.h>

/**
 * @file
 * An API-specific header for AV_HWDEVICE_TYPE_QSV.
 *
 * This API does not support dynamic frame pools. AVHWFramesContext.pool must
 * contain AVBufferRefs whose data pointer points to an mfxFrameSurface1 struct.
 */

/**
 * This struct is allocated as AVHWDeviceContext.hwctx
 */
typedef struct AVQSVDeviceContext {
    mfxSession session;
    /**
     * The mfxLoader handle used for mfxSession creation
     *
     * This field is only available for oneVPL user. For non-oneVPL user, this
     * field must be set to NULL.
     *
     * Filled by the user before calling av_hwdevice_ctx_init() and should be
     * cast to mfxLoader handle. Deallocating the AVHWDeviceContext will always
     * release this interface.
     */
    void *loader;
} AVQSVDeviceContext;

/**
 * This struct is allocated as AVHWFramesContext.hwctx
 */
typedef struct AVQSVFramesContext {
    /**
     * A pointer to mfxFrameSurface1 or mfxFrameInfo structure.
     *
     * When nb_surfaces is 0, it is a pointer to mfxFrameInfo structure,
     * otherwise it is a pointer to mfxFrameSurface1.
     */
    union {
        mfxFrameSurface1 *surfaces;
        mfxFrameInfo     *info;
    };

    /**
     * Number of frames
     *
     * A dynamic frame pool is required when nb_surfaces is 0, otherwise
     * a fixed frame pool is required.
     *
     * User should make sure the configuration can support dynamic frame
     * allocation when dynamic frame pool is required. For example, you cannt
     * set nb_surfaces to 0 when the child_device_type is AV_HWDEVICE_TYPE_DXVA2.
     */
    int            nb_surfaces;

    /**
     * A combination of MFX_MEMTYPE_* describing the frame pool.
     */
    int frame_type;
} AVQSVFramesContext;

#endif /* AVUTIL_HWCONTEXT_QSV_H */


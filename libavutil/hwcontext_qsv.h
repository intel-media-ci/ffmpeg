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
 * AVHWFramesContext.pool must contain AVBufferRefs whose data pointer points
 * to a mfxFrameSurface1 struct.
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
     * A pointer to a mfxFrameSurface1 or mfxFrameInfo struct
     *
     * When nb_surfaces is non-zero, it is a pointer to a mfxFrameSurface1
     * struct.
     *
     * When nb_surfaces is 0, it is a pointer to a mfxFrameInfo struct, all
     * buffers allocated from the pool have the same mfxFrameInfo.
     */
    union {
        mfxFrameSurface1 *surfaces;
        mfxFrameInfo     *info;
    };

    /**
     * Number of frames in the pool
     *
     * It is 0 for dynamic frame pools or AVHWFramesContext.initial_pool_size
     * for fixed frame pools.
     *
     * Note only oneVPL GPU runtime 2.9+ can support dynamic frame pools
     * on d3d11va or vaapi
     */
    int            nb_surfaces;

    /**
     * Set by user. It is a combination of MFX_MEMTYPE_* describing the frame
     * pool.
     */
    int frame_type;
} AVQSVFramesContext;

#endif /* AVUTIL_HWCONTEXT_QSV_H */


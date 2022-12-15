/*
 * Direct3D 12 HW acceleration.
 *
 * copyright (c) 2022-2023 Wu Jianhua <toqsxw@outlook.com>
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

#ifndef AVUTIL_HWCONTEXT_D3D12VA_H
#define AVUTIL_HWCONTEXT_D3D12VA_H

/**
 * @file
 * An API-specific header for AV_HWDEVICE_TYPE_D3D12VA.
 *
 */
#include <stdint.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12video.h>

/**
 * @brief This struct is allocated as AVHWDeviceContext.hwctx
 *
 */
typedef struct AVD3D12VADeviceContext {
    /**
     * Device used for objects creation and access. This can also be
     * used to set the libavcodec decoding device.
     *
     * Can be set by the user. This is the only mandatory field - the other
     * device context fields are set from this and are available for convenience.
     *
     * Deallocating the AVHWDeviceContext will always release this interface,
     * and it does not matter whether it was user-allocated.
     */
    ID3D12Device *device;

    /**
     * If unset, this will be set from the device field on init.
     *
     * Deallocating the AVHWDeviceContext will always release this interface,
     * and it does not matter whether it was user-allocated.
     */
    ID3D12VideoDevice *video_device;

    /**
     * Specifed by sync=1 when init d3d12va
     *
     * Execute commands as sync mode
     */
    int sync;
} AVD3D12VADeviceContext;

/**
 * @brief This struct is used to sync d3d12 execution
 *
 */
typedef struct AVD3D12VASyncContext {
    /**
     * D3D12 fence object
     */
    ID3D12Fence *fence;

    /**
     * A handle to the event object
     */
    HANDLE event;

    /**
     * The fence value used for sync
     */
    uint64_t fence_value;
} AVD3D12VASyncContext;

/**
 * @brief D3D12 frame descriptor for pool allocation.
 *
 */
typedef struct AVD3D12FrameDescriptor {
    /**
     * The texture in which the frame is located. The reference count is
     * managed by the AVBufferRef, and destroying the reference will release
     * the interface.
     *
     * Normally stored in AVFrame.data[0].
     */
    ID3D12Resource *texture;

    /**
     * The index into the array texture element representing the frame
     *
     * Normally stored in AVFrame.data[1] (cast from intptr_t).
     */
    intptr_t index;

    /**
     * The sync context for the texture
     *
     * Use av_d3d12va_wait_idle(sync_ctx) to ensure the decoding or encoding have been finised
     * @see: https://learn.microsoft.com/en-us/windows/win32/medfound/direct3d-12-video-overview#directx-12-fences
     *
     * Normally stored in AVFrame.data[2].
     */
    AVD3D12VASyncContext *sync_ctx;
} AVD3D12FrameDescriptor;

/**
 * @brief This struct is allocated as AVHWFramesContext.hwctx
 *
 */
typedef struct AVD3D12VAFramesContext {
    /**
     * The same implementation as d3d11va
     * This field is not able to be user-allocated at the present.
     */
    AVD3D12FrameDescriptor *texture_infos;
} AVD3D12VAFramesContext;

/**
 * @brief Map sw pixel format to d3d12 format
 *
 * @return d3d12 specified format
 */
DXGI_FORMAT av_d3d12va_map_sw_to_hw_format(enum AVPixelFormat pix_fmt);

/**
 * @brief Allocate an AVD3D12VASyncContext
 *
 * @return Error code (ret < 0 if failed)
 */
int av_d3d12va_sync_context_alloc(AVD3D12VADeviceContext *ctx, AVD3D12VASyncContext **sync_ctx);

/**
 * @brief Free an AVD3D12VASyncContext
 */
void av_d3d12va_sync_context_free(AVD3D12VASyncContext **sync_ctx);

/**
 * @brief Wait for the sync context to the idle state
 *
 * @return Error code (ret < 0 if failed)
 */
int av_d3d12va_wait_idle(AVD3D12VASyncContext *sync_ctx);

/**
 * @brief Wait for a specified command queue to the idle state
 *
 * @return Error code (ret < 0 if failed)
 */
int av_d3d12va_wait_queue_idle(AVD3D12VASyncContext *sync_ctx, ID3D12CommandQueue *command_queue);

#endif /* AVUTIL_HWCONTEXT_D3D12VA_H */

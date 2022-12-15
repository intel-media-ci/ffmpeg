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

#include "config.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "hwcontext_d3d12va_internal.h"
#include "hwcontext_d3d12va.h"
#include "imgutils.h"
#include "pixdesc.h"
#include "pixfmt.h"
#include "thread.h"
#include "compat/w32dlfcn.h"
#include <dxgi1_3.h>

typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY2)(UINT Flags, REFIID riid, void **ppFactory);

static AVOnce functions_loaded = AV_ONCE_INIT;

static PFN_CREATE_DXGI_FACTORY2 d3d12va_create_dxgi_factory2;
static PFN_D3D12_CREATE_DEVICE d3d12va_create_device;
static PFN_D3D12_GET_DEBUG_INTERFACE d3d12va_get_debug_interface;

static av_cold void load_functions(void)
{
    HANDLE d3dlib, dxgilib;

    d3dlib  = dlopen("d3d12.dll", 0);
    dxgilib = dlopen("dxgi.dll", 0);
    if (!d3dlib || !dxgilib)
        return;

    d3d12va_create_device = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3dlib, "D3D12CreateDevice");
    d3d12va_create_dxgi_factory2 = (PFN_CREATE_DXGI_FACTORY2)GetProcAddress(dxgilib, "CreateDXGIFactory2");
    d3d12va_get_debug_interface = (PFN_D3D12_GET_DEBUG_INTERFACE)GetProcAddress(d3dlib, "D3D12GetDebugInterface");
}

typedef struct D3D12VAFramesContext {
    ID3D12Resource            *staging_buffer;
    ID3D12CommandQueue        *command_queue;
    ID3D12CommandAllocator    *command_allocator;
    ID3D12GraphicsCommandList *command_list;
    AVD3D12VASyncContext      *sync_ctx;
    int                        nb_surfaces;
    int                        nb_surfaces_used;
    DXGI_FORMAT                format;
    UINT                       luma_component_size;
} D3D12VAFramesContext;

static const struct {
    DXGI_FORMAT d3d_format;
    enum AVPixelFormat pix_fmt;
} supported_formats[] = {
    { DXGI_FORMAT_NV12, AV_PIX_FMT_NV12 },
    { DXGI_FORMAT_P010, AV_PIX_FMT_P010 },
};

DXGI_FORMAT av_d3d12va_map_sw_to_hw_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_NV12:return DXGI_FORMAT_NV12;
    case AV_PIX_FMT_P010:return DXGI_FORMAT_P010;
    default:             return DXGI_FORMAT_UNKNOWN;
    }
}

int av_d3d12va_sync_context_alloc(AVD3D12VADeviceContext *ctx, AVD3D12VASyncContext **psync_ctx)
{
    AVD3D12VASyncContext *sync_ctx;

    sync_ctx = av_mallocz(sizeof(AVD3D12VASyncContext));
    if (!sync_ctx)
        return AVERROR(ENOMEM);

    DX_CHECK(ID3D12Device_CreateFence(ctx->device, sync_ctx->fence_value, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, &sync_ctx->fence));

    sync_ctx->event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!sync_ctx->event)
        goto fail;

    *psync_ctx = sync_ctx;

    return 0;

fail:
    D3D12_OBJECT_RELEASE(sync_ctx->fence);
    av_freep(&sync_ctx);
    return AVERROR(EINVAL);
}

void av_d3d12va_sync_context_free(AVD3D12VASyncContext **psync_ctx)
{
    AVD3D12VASyncContext *sync_ctx = *psync_ctx;
    if (!psync_ctx || !sync_ctx)
        return;

    av_d3d12va_wait_idle(sync_ctx);

    D3D12_OBJECT_RELEASE(sync_ctx->fence);

    if (sync_ctx->event)
        CloseHandle(sync_ctx->event);

    av_freep(psync_ctx);
}

static int av_d3d12va_wait_for_fence_value(AVD3D12VASyncContext *sync_ctx, uint64_t fence_value)
{
    uint64_t completion = ID3D12Fence_GetCompletedValue(sync_ctx->fence);
    if (completion < fence_value) {
        if (FAILED(ID3D12Fence_SetEventOnCompletion(sync_ctx->fence, fence_value, sync_ctx->event)))
            return AVERROR(EINVAL);

        WaitForSingleObjectEx(sync_ctx->event, INFINITE, FALSE);
    }

    return 0;
}

int av_d3d12va_wait_idle(AVD3D12VASyncContext *ctx)
{
    return av_d3d12va_wait_for_fence_value(ctx, ctx->fence_value);
}

int av_d3d12va_wait_queue_idle(AVD3D12VASyncContext *sync_ctx, ID3D12CommandQueue *command_queue)
{
    DX_CHECK(ID3D12CommandQueue_Signal(command_queue, sync_ctx->fence, ++sync_ctx->fence_value));
    return av_d3d12va_wait_idle(sync_ctx);

fail:
    return AVERROR(EINVAL);
}

static inline int create_resource(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES states, ID3D12Resource **ppResource, int is_read_back)
{
    D3D12_HEAP_PROPERTIES props = {
        .Type                 = is_read_back ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask     = 0,
        .VisibleNodeMask      = 0,
    };

    if (FAILED(ID3D12Device_CreateCommittedResource(device, &props, D3D12_HEAP_FLAG_NONE, desc,
        states, NULL, &IID_ID3D12Resource, ppResource)))
        return AVERROR(EINVAL);

    return 0;
}

static int d3d12va_create_staging_buffer_resource(AVHWFramesContext *ctx)
{
    AVD3D12VADeviceContext *device_hwctx = ctx->device_ctx->hwctx;
    D3D12VAFramesContext   *s            = ctx->internal->priv;

    D3D12_RESOURCE_DESC desc = {
        .Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment          = 0,
        .Width              = 0,
        .Height             = 1,
        .DepthOrArraySize   = 1,
        .MipLevels          = 1,
        .Format             = DXGI_FORMAT_UNKNOWN,
        .SampleDesc         = { .Count = 1, .Quality = 0 },
        .Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags              = D3D12_RESOURCE_FLAG_NONE,
    };

    s->luma_component_size = FFALIGN(ctx->width * (s->format == DXGI_FORMAT_P010 ? 2 : 1), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) * ctx->height;
    desc.Width = s->luma_component_size + (s->luma_component_size >> 1);

    return create_resource(device_hwctx->device, &desc, D3D12_RESOURCE_STATE_COPY_DEST, &s->staging_buffer, 1);
}

static int d3d12va_create_helper_objects(AVHWFramesContext *ctx)
{
    AVD3D12VADeviceContext *device_hwctx = ctx->device_ctx->hwctx;
    D3D12VAFramesContext   *s            = ctx->internal->priv;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {
        .Type     = D3D12_COMMAND_LIST_TYPE_COPY,
        .Priority = 0,
        .NodeMask = 0,
    };

    int ret = d3d12va_create_staging_buffer_resource(ctx);
    if (ret < 0)
        return ret;

    ret = av_d3d12va_sync_context_alloc(device_hwctx, &s->sync_ctx);
    if (ret < 0)
        return ret;

    DX_CHECK(ID3D12Device_CreateCommandQueue(device_hwctx->device, &queue_desc,
        &IID_ID3D12CommandQueue, &s->command_queue));

    DX_CHECK(ID3D12Device_CreateCommandAllocator(device_hwctx->device, queue_desc.Type,
        &IID_ID3D12CommandAllocator, &s->command_allocator));

    DX_CHECK(ID3D12Device_CreateCommandList(device_hwctx->device, 0, queue_desc.Type,
        s->command_allocator, NULL, &IID_ID3D12GraphicsCommandList, &s->command_list));

    DX_CHECK(ID3D12GraphicsCommandList_Close(s->command_list));

    ID3D12CommandQueue_ExecuteCommandLists(s->command_queue, 1, (ID3D12CommandList **)&s->command_list);

    return av_d3d12va_wait_queue_idle(s->sync_ctx, s->command_queue);

fail:
    return AVERROR(EINVAL);
}

static void d3d12va_frames_uninit(AVHWFramesContext *ctx)
{
    AVD3D12VAFramesContext *frames_hwctx = ctx->hwctx;
    D3D12VAFramesContext   *s            = ctx->internal->priv;

    av_d3d12va_sync_context_free(&s->sync_ctx);

    D3D12_OBJECT_RELEASE(s->staging_buffer);
    D3D12_OBJECT_RELEASE(s->command_allocator);
    D3D12_OBJECT_RELEASE(s->command_list);
    D3D12_OBJECT_RELEASE(s->command_queue);

    av_freep(&frames_hwctx->texture_infos);
}

static int d3d12va_frames_get_constraints(AVHWDeviceContext *ctx, const void *hwconfig, AVHWFramesConstraints *constraints)
{
    HRESULT hr;
    int nb_sw_formats = 0;
    AVD3D12VADeviceContext *device_hwctx = ctx->hwctx;

    constraints->valid_sw_formats = av_malloc_array(FF_ARRAY_ELEMS(supported_formats) + 1,
                                                    sizeof(*constraints->valid_sw_formats));
    if (!constraints->valid_sw_formats)
        return AVERROR(ENOMEM);

    for (int i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support = { supported_formats[i].d3d_format };
        hr = ID3D12Device_CheckFeatureSupport(device_hwctx->device, D3D12_FEATURE_FORMAT_SUPPORT, &format_support, sizeof(format_support));
        if (SUCCEEDED(hr) && (format_support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D))
            constraints->valid_sw_formats[nb_sw_formats++] = supported_formats[i].pix_fmt;
    }
    constraints->valid_sw_formats[nb_sw_formats] = AV_PIX_FMT_NONE;

    constraints->valid_hw_formats = av_malloc_array(2, sizeof(*constraints->valid_hw_formats));
    if (!constraints->valid_hw_formats)
        return AVERROR(ENOMEM);

    constraints->valid_hw_formats[0] = AV_PIX_FMT_D3D12;
    constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

    return 0;
}

static void free_texture(void *opaque, uint8_t *data)
{
    AVD3D12FrameDescriptor *desc = (AVD3D12FrameDescriptor *)data;

    if (desc->sync_ctx)
        av_d3d12va_sync_context_free(&desc->sync_ctx);

    D3D12_OBJECT_RELEASE(desc->texture);
    av_freep(&data);
}

static AVBufferRef *wrap_texture_buf(AVHWFramesContext *ctx, ID3D12Resource *texture, AVD3D12VASyncContext *sync_ctx)
{
    AVBufferRef *buf;
    D3D12VAFramesContext   *s            = ctx->internal->priv;
    AVD3D12VAFramesContext *frames_hwctx = ctx->hwctx;

    AVD3D12FrameDescriptor *desc = av_mallocz(sizeof(*desc));
    if (!desc)
        goto fail;

    if (s->nb_surfaces <= s->nb_surfaces_used) {
        frames_hwctx->texture_infos = av_realloc_f(frames_hwctx->texture_infos,
                                                   s->nb_surfaces_used + 1,
                                                   sizeof(*frames_hwctx->texture_infos));
        if (!frames_hwctx->texture_infos)
            goto fail;
        s->nb_surfaces = s->nb_surfaces_used + 1;
    }

    desc->texture  = texture;
    desc->index    = s->nb_surfaces_used;
    desc->sync_ctx = sync_ctx;

    frames_hwctx->texture_infos[s->nb_surfaces_used].texture  = texture;
    frames_hwctx->texture_infos[s->nb_surfaces_used].index    = desc->index;
    frames_hwctx->texture_infos[s->nb_surfaces_used].sync_ctx = sync_ctx;
    s->nb_surfaces_used++;

    buf = av_buffer_create((uint8_t *)desc, sizeof(desc), free_texture, texture, 0);
    if (!buf) {
        D3D12_OBJECT_RELEASE(texture);
        av_freep(&desc);
        return NULL;
    }

    return buf;

fail:
    D3D12_OBJECT_RELEASE(texture);
    av_d3d12va_sync_context_free(&sync_ctx);
    return NULL;
}

static AVBufferRef *d3d12va_pool_alloc(void *opaque, size_t size)
{
    AVHWFramesContext      *ctx          = (AVHWFramesContext *)opaque;
    D3D12VAFramesContext   *s            = ctx->internal->priv;
    AVD3D12VAFramesContext *hwctx        = ctx->hwctx;
    AVD3D12VADeviceContext *device_hwctx = ctx->device_ctx->hwctx;

    int ret;
    ID3D12Resource *texture;
    AVD3D12VASyncContext *sync_ctx;

    D3D12_RESOURCE_DESC desc = {
        .Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment        = 0,
        .Width            = ctx->width,
        .Height           = ctx->height,
        .DepthOrArraySize = 1,
        .MipLevels        = 1,
        .Format           = s->format,
        .SampleDesc       = {.Count = 1, .Quality = 0 },
        .Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags            = D3D12_RESOURCE_FLAG_NONE,
    };

    if (s->nb_surfaces_used >= ctx->initial_pool_size) {
        av_log(ctx, AV_LOG_ERROR, "Static surface pool size exceeded.\n");
        return NULL;
    }

    ret = create_resource(device_hwctx->device, &desc, D3D12_RESOURCE_STATE_COMMON, &texture, 0);
    if (ret < 0)
        return NULL;

    ret = av_d3d12va_sync_context_alloc(device_hwctx, &sync_ctx);
    if (ret < 0) {
        D3D12_OBJECT_RELEASE(texture)
            return NULL;
    }

    return wrap_texture_buf(ctx, texture, sync_ctx);
}

static int d3d12va_frames_init(AVHWFramesContext *ctx)
{
    AVD3D12VAFramesContext *hwctx        = ctx->hwctx;
    AVD3D12VADeviceContext *device_hwctx = ctx->device_ctx->hwctx;
    D3D12VAFramesContext   *s            = ctx->internal->priv;

    int i;

    if (ctx->initial_pool_size > D3D12VA_MAX_SURFACES) {
        av_log(ctx, AV_LOG_WARNING, "Too big initial pool size(%d) for surfaces. "
            "The size will be limited to %d automatically\n", ctx->initial_pool_size, D3D12VA_MAX_SURFACES);
        ctx->initial_pool_size = D3D12VA_MAX_SURFACES;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++) {
        if (ctx->sw_format == supported_formats[i].pix_fmt) {
            s->format = supported_formats[i].d3d_format;
            break;
        }
    }
    if (i == FF_ARRAY_ELEMS(supported_formats)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported pixel format: %s\n",
               av_get_pix_fmt_name(ctx->sw_format));
        return AVERROR(EINVAL);
    }

    hwctx->texture_infos = av_realloc_f(NULL, ctx->initial_pool_size, sizeof(*hwctx->texture_infos));
    if (!hwctx->texture_infos)
        return AVERROR(ENOMEM);

    memset(hwctx->texture_infos, 0, ctx->initial_pool_size * sizeof(*hwctx->texture_infos));
    s->nb_surfaces = ctx->initial_pool_size;

    ctx->internal->pool_internal = av_buffer_pool_init2(sizeof(AVD3D12FrameDescriptor),
        ctx, d3d12va_pool_alloc, NULL);

    if (!ctx->internal->pool_internal)
        return AVERROR(ENOMEM);

    return 0;
}

static int d3d12va_get_buffer(AVHWFramesContext *ctx, AVFrame *frame)
{
    int ret;
    AVD3D12FrameDescriptor *desc;

    frame->buf[0] = av_buffer_pool_get(ctx->pool);
    if (!frame->buf[0])
        return AVERROR(ENOMEM);

    ret = av_image_fill_arrays(frame->data, frame->linesize, frame->buf[0]->data,
        ctx->sw_format, ctx->width, ctx->height, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    if (ret < 0)
        return ret;

    desc = (AVD3D12FrameDescriptor *)frame->buf[0]->data;
    frame->data[0] = (uint8_t *)desc->texture;
    frame->data[1] = (uint8_t *)desc->index;
    frame->data[2] = (uint8_t *)desc->sync_ctx;

    frame->format  = AV_PIX_FMT_D3D12;
    frame->width   = ctx->width;
    frame->height  = ctx->height;

    return 0;
}

static int d3d12va_transfer_get_formats(AVHWFramesContext *ctx,
                                        enum AVHWFrameTransferDirection dir,
                                        enum AVPixelFormat **formats)
{
    D3D12VAFramesContext *s = ctx->internal->priv;
    enum AVPixelFormat *fmts;

    fmts = av_malloc_array(2, sizeof(*fmts));
    if (!fmts)
        return AVERROR(ENOMEM);

    fmts[0] = ctx->sw_format;
    fmts[1] = AV_PIX_FMT_NONE;

    *formats = fmts;

    return 0;
}

static int d3d12va_transfer_data(AVHWFramesContext *ctx, AVFrame *dst,
                                 const AVFrame *src)
{
    AVD3D12VADeviceContext *hwctx        = ctx->device_ctx->hwctx;
    AVD3D12VAFramesContext *frames_hwctx = ctx->hwctx;
    D3D12VAFramesContext   *s            = ctx->internal->priv;

    int ret;
    int download = src->format == AV_PIX_FMT_D3D12;
    const AVFrame *frame = download ? src : dst;
    const AVFrame *other = download ? dst : src;

    ID3D12Resource       *texture  = (ID3D12Resource *)      frame->data[0];
    int                   index    = (intptr_t)              frame->data[1];
    AVD3D12VASyncContext *sync_ctx = (AVD3D12VASyncContext *)frame->data[2];

    uint8_t *mapped_data;
    uint8_t *data[4];
    int linesizes[4];

    D3D12_TEXTURE_COPY_LOCATION staging_y_location;
    D3D12_TEXTURE_COPY_LOCATION staging_uv_location;

    D3D12_TEXTURE_COPY_LOCATION texture_y_location = {
        .pResource        = texture,
        .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };

    D3D12_TEXTURE_COPY_LOCATION texture_uv_location = {
        .pResource        = texture,
        .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 1,
    };

    D3D12_RESOURCE_BARRIER barrier = {
        .Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource   = texture,
            .StateBefore = D3D12_RESOURCE_STATE_COMMON,
            .StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        }
    };

    s->format = av_d3d12va_map_sw_to_hw_format(ctx->sw_format);

    if (frame->hw_frames_ctx->data != (uint8_t *)ctx || other->format != ctx->sw_format)
        return AVERROR(EINVAL);

    if (!s->command_queue) {
        ret = d3d12va_create_helper_objects(ctx);
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < 4; i++)
        linesizes[i] = FFALIGN(frame->width * (s->format == DXGI_FORMAT_P010 ? 2 : 1), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

    staging_y_location = (D3D12_TEXTURE_COPY_LOCATION) {
        .pResource = s->staging_buffer,
        .Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {
            .Offset = 0,
            .Footprint = {
                .Format   = s->format == DXGI_FORMAT_P010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
                .Width    = ctx->width,
                .Height   = ctx->height,
                .Depth    = 1,
                .RowPitch = linesizes[0],
            },
        },
    };

    staging_uv_location = (D3D12_TEXTURE_COPY_LOCATION) {
        .pResource = s->staging_buffer,
        .Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {
            .Offset = s->luma_component_size,
            .Footprint = {
                .Format   = s->format == DXGI_FORMAT_P010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM,
                .Width    = ctx->width  >> 1,
                .Height   = ctx->height >> 1,
                .Depth    = 1,
                .RowPitch = linesizes[0],
            },
        },
    };

    DX_CHECK(ID3D12CommandAllocator_Reset(s->command_allocator));

    DX_CHECK(ID3D12GraphicsCommandList_Reset(s->command_list, s->command_allocator, NULL));

    if (download) {
        ID3D12GraphicsCommandList_ResourceBarrier(s->command_list, 1, &barrier);

        ID3D12GraphicsCommandList_CopyTextureRegion(s->command_list,
            &staging_y_location, 0, 0, 0, &texture_y_location, NULL);

        ID3D12GraphicsCommandList_CopyTextureRegion(s->command_list,
            &staging_uv_location, 0, 0, 0, &texture_uv_location, NULL);

        barrier.Transition.StateBefore = barrier.Transition.StateAfter;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        ID3D12GraphicsCommandList_ResourceBarrier(s->command_list, 1, &barrier);

        DX_CHECK(ID3D12GraphicsCommandList_Close(s->command_list));

        if (!hwctx->sync)
            DX_CHECK(ID3D12CommandQueue_Wait(s->command_queue, sync_ctx->fence, sync_ctx->fence_value));

        ID3D12CommandQueue_ExecuteCommandLists(s->command_queue, 1, (ID3D12CommandList **)&s->command_list);

        ret = av_d3d12va_wait_queue_idle(s->sync_ctx, s->command_queue);
        if (ret)
            return ret;

        DX_CHECK(ID3D12Resource_Map(s->staging_buffer, 0, NULL, &mapped_data));
        av_image_fill_pointers(data, ctx->sw_format, ctx->height, mapped_data, linesizes);

        av_image_copy(dst->data, dst->linesize, data, linesizes,
            ctx->sw_format, ctx->width, ctx->height);

        ID3D12Resource_Unmap(s->staging_buffer, 0, NULL);
    } else {
        av_log(ctx, AV_LOG_ERROR, "Transfer data to AV_PIX_FMT_D3D12 is not supported yet!\n");
        return AVERROR(EINVAL);
    }

    return 0;

fail:
    return AVERROR(EINVAL);
}

static int d3d12va_device_init(AVHWDeviceContext *hwdev)
{
    AVD3D12VADeviceContext *ctx = hwdev->hwctx;

    if (!ctx->video_device)
        DX_CHECK(ID3D12Device_QueryInterface(ctx->device, &IID_ID3D12VideoDevice, (void **)&ctx->video_device));

    return 0;

fail:
    return AVERROR(EINVAL);
}

static void d3d12va_device_uninit(AVHWDeviceContext *hwdev)
{
    AVD3D12VADeviceContext *device_hwctx = hwdev->hwctx;

    D3D12_OBJECT_RELEASE(device_hwctx->video_device);
    D3D12_OBJECT_RELEASE(device_hwctx->device);
}

static int d3d12va_device_create(AVHWDeviceContext *ctx, const char *device,
                                 AVDictionary *opts, int flags)
{
    AVD3D12VADeviceContext *device_hwctx = ctx->hwctx;

    HRESULT hr;
    UINT create_flags = 0;
    IDXGIAdapter *pAdapter = NULL;

    int ret;
    int is_debug       = !!av_dict_get(opts, "debug", NULL, 0);
    device_hwctx->sync = !!av_dict_get(opts, "sync",  NULL, 0);

    if ((ret = ff_thread_once(&functions_loaded, load_functions)) != 0)
        return AVERROR_UNKNOWN;

    if (is_debug) {
        ID3D12Debug *pDebug;
        if (d3d12va_get_debug_interface && SUCCEEDED(d3d12va_get_debug_interface(&IID_ID3D12Debug, &pDebug))) {
            create_flags |= DXGI_CREATE_FACTORY_DEBUG;
            ID3D12Debug_EnableDebugLayer(pDebug);
            D3D12_OBJECT_RELEASE(pDebug);
            av_log(ctx, AV_LOG_INFO, "D3D12 debug layer is enabled!\n");
        }
    }

    if (!device_hwctx->device) {
        IDXGIFactory2 *pDXGIFactory = NULL;

        if (!d3d12va_create_device || !d3d12va_create_dxgi_factory2) {
            av_log(ctx, AV_LOG_ERROR, "Failed to load D3D12 library or its functions\n");
            return AVERROR_UNKNOWN;
        }

        hr = d3d12va_create_dxgi_factory2(create_flags, &IID_IDXGIFactory2, (void **)&pDXGIFactory);
        if (SUCCEEDED(hr)) {
            int adapter = device ? atoi(device) : 0;
            if (FAILED(IDXGIFactory2_EnumAdapters(pDXGIFactory, adapter, &pAdapter)))
                pAdapter = NULL;
            IDXGIFactory2_Release(pDXGIFactory);
        }

        if (pAdapter) {
            DXGI_ADAPTER_DESC desc;
            hr = IDXGIAdapter2_GetDesc(pAdapter, &desc);
            if (!FAILED(hr)) {
                av_log(ctx, AV_LOG_INFO, "Using device %04x:%04x (%ls).\n",
                       desc.VendorId, desc.DeviceId, desc.Description);
            }
        }

        hr = d3d12va_create_device((IUnknown *)pAdapter, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, &device_hwctx->device);
        D3D12_OBJECT_RELEASE(pAdapter);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create DirectX3D 12 device (%lx)\n", (long)hr);
            return AVERROR_UNKNOWN;
        }
    }

    return 0;
}

const HWContextType ff_hwcontext_type_d3d12va = {
    .type                   = AV_HWDEVICE_TYPE_D3D12VA,
    .name                   = "D3D12VA",

    .device_hwctx_size      = sizeof(AVD3D12VADeviceContext),
    .frames_hwctx_size      = sizeof(AVD3D12VAFramesContext),
    .frames_priv_size       = sizeof(D3D12VAFramesContext),

    .device_create          = d3d12va_device_create,
    .device_init            = d3d12va_device_init,
    .device_uninit          = d3d12va_device_uninit,
    .frames_get_constraints = d3d12va_frames_get_constraints,
    .frames_init            = d3d12va_frames_init,
    .frames_uninit          = d3d12va_frames_uninit,
    .frames_get_buffer      = d3d12va_get_buffer,
    .transfer_get_formats   = d3d12va_transfer_get_formats,
    .transfer_data_to       = d3d12va_transfer_data,
    .transfer_data_from     = d3d12va_transfer_data,

    .pix_fmts               = (const enum AVPixelFormat[]){ AV_PIX_FMT_D3D12, AV_PIX_FMT_NONE },
};

/*
 * Direct3D 12 HW acceleration video decoder
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

#include <assert.h>
#include <string.h>
#include <initguid.h>

#include "libavutil/common.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext_d3d12va_internal.h"
#include "libavutil/hwcontext_d3d12va.h"
#include "avcodec.h"
#include "decode.h"
#include "d3d12va.h"

typedef struct CommandAllocator {
    ID3D12CommandAllocator *command_allocator;
    uint64_t fence_value;
} CommandAllocator;

int ff_d3d12va_get_suitable_max_bitstream_size(AVCodecContext *avctx)
{
    AVHWFramesContext *frames_ctx = D3D12VA_FRAMES_CONTEXT(avctx);
    return av_image_get_buffer_size(frames_ctx->sw_format, avctx->coded_width, avctx->coded_height, 1);
}

static int d3d12va_get_valid_command_allocator(AVCodecContext *avctx, ID3D12CommandAllocator **ppAllocator)
{
    HRESULT hr;
    D3D12VADecodeContext *ctx = D3D12VA_DECODE_CONTEXT(avctx);
    CommandAllocator allocator;

    if (av_fifo_peek(ctx->allocator_queue, &allocator, 1, 0) >= 0) {
        uint64_t completion = ID3D12Fence_GetCompletedValue(ctx->sync_ctx->fence);
        if (completion >= allocator.fence_value) {
            *ppAllocator = allocator.command_allocator;
            av_fifo_read(ctx->allocator_queue, &allocator, 1);
            return 0;
        }
    }

    hr = ID3D12Device_CreateCommandAllocator(ctx->device_ctx->device, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE,
        &IID_ID3D12CommandAllocator, ppAllocator);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create a new command allocator!\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int d3d12va_discard_command_allocator(AVCodecContext *avctx, ID3D12CommandAllocator *pAllocator, uint64_t fence_value)
{
    D3D12VADecodeContext *ctx = D3D12VA_DECODE_CONTEXT(avctx);

    CommandAllocator allocator = {
        .command_allocator = pAllocator,
        .fence_value = fence_value
    };

    if (av_fifo_write(ctx->allocator_queue, &allocator, 1) < 0) {
        D3D12_OBJECT_RELEASE(pAllocator);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static void bufref_free_interface(void *opaque, uint8_t *data)
{
    D3D12_OBJECT_RELEASE(opaque);
}

static AVBufferRef *bufref_wrap_interface(IUnknown *iface)
{
    return av_buffer_create((uint8_t*)iface, 1, bufref_free_interface, iface, 0);
}

static int d3d12va_create_buffer(AVCodecContext *avctx, UINT size, ID3D12Resource **ppResouce)
{
    D3D12VADecodeContext *ctx = D3D12VA_DECODE_CONTEXT(avctx);

    D3D12_HEAP_PROPERTIES heap_props = { .Type = D3D12_HEAP_TYPE_UPLOAD };

    D3D12_RESOURCE_DESC desc = {
        .Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment        = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        .Width            = size,
        .Height           = 1,
        .DepthOrArraySize = 1,
        .MipLevels        = 1,
        .Format           = DXGI_FORMAT_UNKNOWN,
        .SampleDesc       = { .Count = 1, .Quality = 0 },
        .Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags            = D3D12_RESOURCE_FLAG_NONE,
    };

    HRESULT hr = ID3D12Device_CreateCommittedResource(ctx->device_ctx->device, &heap_props, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, ppResouce);

    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create d3d12 buffer.\n");
        return  AVERROR(EINVAL);
    }

    return 0;
}

static int d3d12va_wait_for_gpu(AVCodecContext *avctx)
{
    D3D12VADecodeContext *ctx      = D3D12VA_DECODE_CONTEXT(avctx);
    AVD3D12VASyncContext *sync_ctx = ctx->sync_ctx;

    return av_d3d12va_wait_queue_idle(sync_ctx, ctx->command_queue);
}

static int d3d12va_create_decoder_heap(AVCodecContext *avctx)
{
    D3D12VADecodeContext   *ctx        = D3D12VA_DECODE_CONTEXT(avctx);
    AVHWFramesContext      *frames_ctx = D3D12VA_FRAMES_CONTEXT(avctx);
    AVD3D12VADeviceContext *hwctx      = ctx->device_ctx;

    D3D12_VIDEO_DECODER_HEAP_DESC desc = {
        .NodeMask      = 0,
        .Configuration = ctx->cfg,
        .DecodeWidth   = frames_ctx->width,
        .DecodeHeight  = frames_ctx->height,
        .Format        = av_d3d12va_map_sw_to_hw_format(frames_ctx->sw_format),
        .FrameRate     = { avctx->framerate.num, avctx->framerate.den },
        .BitRate       = avctx->bit_rate,
        .MaxDecodePictureBufferCount = frames_ctx->initial_pool_size,
    };

    DX_CHECK(ID3D12VideoDevice_CreateVideoDecoderHeap(hwctx->video_device, &desc,
        &IID_ID3D12VideoDecoderHeap, &ctx->decoder_heap));

    return 0;

fail:
    if (ctx->decoder) {
        av_log(avctx, AV_LOG_ERROR, "D3D12 doesn't support decoding frames with an extent "
            "[width(%d), height(%d)], on your device!\n", frames_ctx->width, frames_ctx->height);
    }

    return AVERROR(EINVAL);
}

static int d3d12va_create_decoder(AVCodecContext *avctx)
{
    D3D12_VIDEO_DECODER_DESC desc;
    D3D12VADecodeContext   *ctx        = D3D12VA_DECODE_CONTEXT(avctx);
    AVHWFramesContext      *frames_ctx = D3D12VA_FRAMES_CONTEXT(avctx);
    AVD3D12VADeviceContext *hwctx      = ctx->device_ctx;

    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT feature = {
        .NodeIndex     = 0,
        .Configuration = ctx->cfg,
        .Width         = frames_ctx->width,
        .Height        = frames_ctx->height,
        .DecodeFormat  = av_d3d12va_map_sw_to_hw_format(frames_ctx->sw_format),
        .FrameRate     = { avctx->framerate.num, avctx->framerate.den },
        .BitRate       = avctx->bit_rate,
    };

    DX_CHECK(ID3D12VideoDevice_CheckFeatureSupport(hwctx->video_device, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &feature, sizeof(feature)));
    if (!(feature.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED) ||
        !(feature.DecodeTier >= D3D12_VIDEO_DECODE_TIER_2)) {
        av_log(avctx, AV_LOG_ERROR, "D3D12 decoder doesn't support on this device\n");
        return AVERROR(EINVAL);
    }

    desc = (D3D12_VIDEO_DECODER_DESC) {
        .NodeMask = 0,
        .Configuration = ctx->cfg,
    };

    DX_CHECK(ID3D12VideoDevice_CreateVideoDecoder(hwctx->video_device, &desc, &IID_ID3D12VideoDecoder, &ctx->decoder));

    ctx->decoder_ref = bufref_wrap_interface((IUnknown *)ctx->decoder);
    if (!ctx->decoder_ref)
        return AVERROR(ENOMEM);

    return 0;

fail:
    return AVERROR(EINVAL);
}

static inline int d3d12va_get_num_surfaces(enum AVCodecID codec_id)
{
    int num_surfaces = 1;
    switch (codec_id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
        num_surfaces += 16;
        break;

    case AV_CODEC_ID_AV1:
        num_surfaces += 12;
        break;

    case AV_CODEC_ID_VP9:
        num_surfaces += 8;
        break;

    default:
        num_surfaces += 2;
    }

    return num_surfaces;
}

int ff_d3d12va_common_frame_params(AVCodecContext *avctx, AVBufferRef *hw_frames_ctx)
{
    AVHWFramesContext      *frames_ctx   = (AVHWFramesContext *)hw_frames_ctx->data;
    AVHWDeviceContext      *device_ctx   = frames_ctx->device_ctx;
    AVD3D12VAFramesContext *frames_hwctx = frames_ctx->hwctx;

    frames_ctx->format    = AV_PIX_FMT_D3D12;
    frames_ctx->sw_format = avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P10 ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    frames_ctx->width     = avctx->width;
    frames_ctx->height    = avctx->height;

    frames_ctx->initial_pool_size = d3d12va_get_num_surfaces(avctx->codec_id);

    return 0;
}

int ff_d3d12va_decode_init(AVCodecContext *avctx)
{
    int ret;
    UINT bitstream_size;
    AVHWFramesContext *frames_ctx;
    D3D12VADecodeContext *ctx = D3D12VA_DECODE_CONTEXT(avctx);

    ID3D12CommandAllocator *command_allocator = NULL;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {
        .Type     = D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE,
        .Priority = 0,
        .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0
    };

    ctx->pix_fmt = avctx->hwaccel->pix_fmt;

    ret = ff_decode_get_hw_frames_ctx(avctx, AV_HWDEVICE_TYPE_D3D12VA);
    if (ret < 0)
        return ret;

    frames_ctx = D3D12VA_FRAMES_CONTEXT(avctx);
    ctx->device_ctx = (AVD3D12VADeviceContext *)frames_ctx->device_ctx->hwctx;

    if (frames_ctx->format != ctx->pix_fmt) {
        av_log(avctx, AV_LOG_ERROR, "Invalid pixfmt for hwaccel!\n");
        goto fail;
    }

    ret = d3d12va_create_decoder(avctx);
    if (ret < 0)
        goto fail;

    ret = d3d12va_create_decoder_heap(avctx);
    if (ret < 0)
        goto fail;

    ctx->max_num_ref = frames_ctx->initial_pool_size;

    bitstream_size = ff_d3d12va_get_suitable_max_bitstream_size(avctx);
    ctx->buffers = av_calloc(sizeof(ID3D12Resource *), ctx->max_num_ref);
    for (int i = 0; i < ctx->max_num_ref; i++) {
        ret = d3d12va_create_buffer(avctx, bitstream_size, &ctx->buffers[i]);
        if (ret < 0)
            goto fail;
    }

    ctx->ref_resources = av_calloc(sizeof(ID3D12Resource *), ctx->max_num_ref);
    if (!ctx->ref_resources)
        return AVERROR(ENOMEM);

    ctx->ref_subresources = av_calloc(sizeof(UINT), ctx->max_num_ref);
    if (!ctx->ref_subresources)
        return AVERROR(ENOMEM);

    ctx->allocator_queue = av_fifo_alloc2(ctx->max_num_ref, sizeof(CommandAllocator), AV_FIFO_FLAG_AUTO_GROW);
    if (!ctx->allocator_queue)
        return AVERROR(ENOMEM);

    ret = av_d3d12va_sync_context_alloc(ctx->device_ctx, &ctx->sync_ctx);
    if (ret < 0)
        goto fail;

    ret = d3d12va_get_valid_command_allocator(avctx, &command_allocator);
    if (ret < 0)
        goto fail;

    DX_CHECK(ID3D12Device_CreateCommandQueue(ctx->device_ctx->device, &queue_desc,
        &IID_ID3D12CommandQueue, &ctx->command_queue));

    DX_CHECK(ID3D12Device_CreateCommandList(ctx->device_ctx->device, 0, queue_desc.Type,
        command_allocator, NULL, &IID_ID3D12CommandList, &ctx->command_list));

    DX_CHECK(ID3D12VideoDecodeCommandList_Close(ctx->command_list));

    ID3D12CommandQueue_ExecuteCommandLists(ctx->command_queue, 1, (ID3D12CommandList **)&ctx->command_list);

    d3d12va_wait_for_gpu(avctx);

    d3d12va_discard_command_allocator(avctx, command_allocator, ctx->sync_ctx->fence_value);

    return 0;

fail:
    D3D12_OBJECT_RELEASE(command_allocator);
    ff_d3d12va_decode_uninit(avctx);

    return AVERROR(EINVAL);
}

int ff_d3d12va_decode_uninit(AVCodecContext *avctx)
{
    int i, num_allocator = 0;
    D3D12VADecodeContext *ctx = D3D12VA_DECODE_CONTEXT(avctx);
    CommandAllocator allocator;

    if (ctx->sync_ctx)
        d3d12va_wait_for_gpu(avctx);

    av_freep(&ctx->ref_resources);

    av_freep(&ctx->ref_subresources);

    for (i = 0; i < ctx->max_num_ref; i++)
        D3D12_OBJECT_RELEASE(ctx->buffers[i]);

    av_freep(&ctx->buffers);

    D3D12_OBJECT_RELEASE(ctx->command_list);

    D3D12_OBJECT_RELEASE(ctx->command_queue);

    if (ctx->allocator_queue) {
        while (av_fifo_read(ctx->allocator_queue, &allocator, 1) >= 0) {
            num_allocator++;
            D3D12_OBJECT_RELEASE(allocator.command_allocator);
        }

        av_log(avctx, AV_LOG_VERBOSE, "Total number of command allocators reused: %d\n", num_allocator);
    }

    av_fifo_freep2(&ctx->allocator_queue);

    av_d3d12va_sync_context_free(&ctx->sync_ctx);

    D3D12_OBJECT_RELEASE(ctx->decoder_heap);

    av_buffer_unref(&ctx->decoder_ref);

    return 0;
}

static ID3D12Resource *get_surface(const AVFrame *frame)
{
    return (ID3D12Resource *)frame->data[0];
}

intptr_t ff_d3d12va_get_surface_index(AVCodecContext *ctx, const AVFrame* frame)
{
    return (intptr_t)frame->data[1];
}

static AVD3D12VASyncContext *d3d12va_get_sync_context(const AVFrame *frame)
{
    return (AVD3D12VASyncContext *)frame->data[2];
}

static int d3d12va_begin_update_reference_frames(AVCodecContext *avctx, D3D12_RESOURCE_BARRIER *barriers, int index)
{
    D3D12VADecodeContext   *ctx          = D3D12VA_DECODE_CONTEXT(avctx);
    AVHWFramesContext      *frames_ctx   = D3D12VA_FRAMES_CONTEXT(avctx);
    AVD3D12VAFramesContext *frames_hwctx = frames_ctx->hwctx;

    int num_barrier = 0;

    for (int i = 0; i < ctx->max_num_ref; i++) {
        if (ctx->ref_resources[i] && ctx->ref_resources[i] != frames_hwctx->texture_infos[index].texture) {
            barriers[num_barrier].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[num_barrier].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[num_barrier].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){
                .pResource   = ctx->ref_resources[i],
                .Subresource = 0,
                .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                .StateAfter  = D3D12_RESOURCE_STATE_VIDEO_DECODE_READ,
            };
            num_barrier++;
        }
    }

    return num_barrier;
}

static void d3d12va_end_update_reference_frames(AVCodecContext *avctx, D3D12_RESOURCE_BARRIER *barriers, int index)
{
    D3D12VADecodeContext   *ctx          = D3D12VA_DECODE_CONTEXT(avctx);
    AVHWFramesContext      *frames_ctx   = D3D12VA_FRAMES_CONTEXT(avctx);
    AVD3D12VAFramesContext *frames_hwctx = frames_ctx->hwctx;
    int num_barrier = 0;

    for (int i = 0; i < ctx->max_num_ref; i++) {
        if (ctx->ref_resources[i] && ctx->ref_resources[i] != frames_hwctx->texture_infos[index].texture) {
            barriers[num_barrier].Transition.pResource = ctx->ref_resources[i];
            barriers[num_barrier].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[num_barrier].Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_DECODE_READ;
            barriers[num_barrier].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            num_barrier++;
        }
    }
}

int ff_d3d12va_common_end_frame(AVCodecContext *avctx, AVFrame *frame,
                              const void *pp, unsigned pp_size,
                              const void *qm, unsigned qm_size,
                              int(*update_input_arguments)(AVCodecContext *, D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *, ID3D12Resource *))
{
    int ret;
    D3D12VADecodeContext   *ctx               = D3D12VA_DECODE_CONTEXT(avctx);
    AVHWFramesContext      *frames_ctx        = D3D12VA_FRAMES_CONTEXT(avctx);
    AVD3D12VAFramesContext *frames_hwctx      = frames_ctx->hwctx;
    ID3D12CommandAllocator *command_allocator = NULL;

    ID3D12Resource *resource = get_surface(frame);
    UINT index = ff_d3d12va_get_surface_index(avctx, frame);
    AVD3D12VASyncContext *sync_ctx = d3d12va_get_sync_context(frame);

    ID3D12VideoDecodeCommandList *cmd_list = ctx->command_list;
    D3D12_RESOURCE_BARRIER barriers[D3D12VA_MAX_SURFACES] = { 0 };

    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS input_args = {
        .NumFrameArguments = 2,
        .FrameArguments = {
            [0] = {
                .Type  = D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS,
                .Size  = pp_size,
                .pData = (void *)pp,
            },
            [1] = {
                .Type  = D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX,
                .Size  = qm_size,
                .pData = (void *)qm,
            },
        },
        .pHeap = ctx->decoder_heap,
    };

    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS output_args = {
        .ConversionArguments = 0,
        .OutputSubresource   = 0,
        .pOutputTexture2D    = resource,
    };

    UINT num_barrier = 1;
    barriers[0] = (D3D12_RESOURCE_BARRIER) {
        .Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource   = resource,
            .Subresource = 0,
            .StateBefore = D3D12_RESOURCE_STATE_COMMON,
            .StateAfter  = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE,
        },
    };

    memset(ctx->ref_resources, 0, sizeof(ID3D12Resource *) * ctx->max_num_ref);
    memset(ctx->ref_subresources, 0, sizeof(UINT) * ctx->max_num_ref);
    input_args.ReferenceFrames.NumTexture2Ds = ctx->max_num_ref;
    input_args.ReferenceFrames.ppTexture2Ds  = ctx->ref_resources;
    input_args.ReferenceFrames.pSubresources = ctx->ref_subresources;

    av_d3d12va_wait_idle(sync_ctx);

    if (!qm)
        input_args.NumFrameArguments = 1;

    ret = update_input_arguments(avctx, &input_args, ctx->buffers[index]);
    if (ret < 0)
        return ret;

    ret = d3d12va_get_valid_command_allocator(avctx, &command_allocator);
    if (ret < 0)
        goto fail;

    DX_CHECK(ID3D12CommandAllocator_Reset(command_allocator));

    DX_CHECK(ID3D12VideoDecodeCommandList_Reset(cmd_list, command_allocator));

    num_barrier += d3d12va_begin_update_reference_frames(avctx, &barriers[1], index);

    ID3D12VideoDecodeCommandList_ResourceBarrier(cmd_list, num_barrier, barriers);

    ID3D12VideoDecodeCommandList_DecodeFrame(cmd_list, ctx->decoder, &output_args, &input_args);

    barriers[0].Transition.StateBefore = barriers[0].Transition.StateAfter;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    d3d12va_end_update_reference_frames(avctx, &barriers[1], index);

    ID3D12VideoDecodeCommandList_ResourceBarrier(cmd_list, num_barrier, barriers);

    DX_CHECK(ID3D12VideoDecodeCommandList_Close(cmd_list));

    ID3D12CommandQueue_ExecuteCommandLists(ctx->command_queue, 1, (ID3D12CommandList **)&ctx->command_list);

    DX_CHECK(ID3D12CommandQueue_Signal(ctx->command_queue, sync_ctx->fence, ++sync_ctx->fence_value));

    DX_CHECK(ID3D12CommandQueue_Signal(ctx->command_queue, ctx->sync_ctx->fence, ++ctx->sync_ctx->fence_value));

    ret = d3d12va_discard_command_allocator(avctx, command_allocator, ctx->sync_ctx->fence_value);
    if (ret < 0)
        return ret;

    if (ctx->device_ctx->sync) {
        ret = d3d12va_wait_for_gpu(avctx);
        if (ret < 0)
            return ret;
    }

    return 0;

fail:
    if (command_allocator)
        d3d12va_discard_command_allocator(avctx, command_allocator, ctx->sync_ctx->fence_value);
    return AVERROR(EINVAL);
}

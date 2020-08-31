;*****************************************************************************
;* x86-optimized functions for dnn native backend convolution
;*
;* This file is part of FFmpeg.
;*
;* FFmpeg is free software; you can redistribute it and/or
;* modify it under the terms of the GNU Lesser General Public
;* License as published by the Free Software Foundation; either
;* version 2.1 of the License, or (at your option) any later version.
;*
;* FFmpeg is distributed in the hope that it will be useful,
;* but WITHOUT ANY WARRANTY; without even the implied warranty of
;* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;* Lesser General Public License for more details.
;*
;* You should have received a copy of the GNU Lesser General Public
;* License along with FFmpeg; if not, write to the Free Software
;* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
;******************************************************************************

%include "libavutil/x86/x86util.asm"

SECTION .text

%macro COUNT_INPUT 0
    mov tmp1d, padding_method
    cmp tmp1d, SAME_CLAMP_TO_EDGE
    je .clamp

    cmp y_posd, 0
    jl .out_of_th
    mov tmp2d, height
    cmp y_posd, tmp2d
    jge .out_of_th

    cmp x_posd, 0
    jl .out_of_th
    mov tmp2d, width
    cmp x_posd, tmp2d
    jge .out_of_th

    mov tmp1d, y_posd
    imul tmp1d, src_linesize
    mov tmp2d, x_posd
    imul tmp2d, input_num
    add tmp1d, tmp2d
    jmp .count_end

    .out_of_th:
        mov tmp1d, -1
        jmp .count_end

    .clamp:
    cmp y_posd, 0
    jl .y_clamp_zero
    mov tmp1d, height
    cmp y_posd, tmp1d
    jge .y_clamp_height
    mov tmp1d, y_posd
    jmp .y_normal

    .y_clamp_zero:
        xor tmp1d, tmp1d
        jmp .y_normal

    .y_clamp_height:
        sub tmp1d, 1

    .y_normal:

    cmp x_posd, 0
    jl .x_clamp_zero
    mov tmp2d, width
    cmp x_posd, tmp2d
    jge .x_clamp_width
    mov tmp2d, x_posd
    jmp .x_normal

    .x_clamp_zero:
        xor tmp2d, tmp2d
        jmp .x_normal

    .x_clamp_width:
        sub tmp2d, 1

    .x_normal:

    imul tmp1d, src_linesize
    imul tmp2d, input_num
    add tmp1d, tmp2d

    .count_end:
%endmacro

; void ff_dnn_execute_layer_conv2d_sse4(const execute_param *exe_param);

%if ARCH_X86_64
INIT_XMM sse4
cglobal dnn_execute_layer_conv2d, 8, 15, 3, exe_param,\
    x, y, n_filter, cha, kernel_x, kernel_y, x_pos, y_pos, kernel_pos,\
    input, output, kernel, tmp1, tmp2

%define thread_start [exe_paramq]
%define thread_end [exe_paramq + 1 * 4]
%define input_num [exe_paramq + 2 * 4]
%define output_num [exe_paramq + 3 * 4]
%define kernel_size [exe_paramq + 4 * 4]
%define padding_method [exe_paramq + 5 * 4]
%define dilation [exe_paramq + 6 * 4]
%define pad_size [exe_paramq + 7 * 4]
%define width [exe_paramq + 8 * 4]
%define height [exe_paramq + 9 * 4]
%define radius [exe_paramq + 10 * 4]
%define src_linesize [exe_paramq + 11 * 4]
%define filter_size [exe_paramq + 12 * 4]
%define filter_linesize [exe_paramq + 13 * 4]
%define SAME_CLAMP_TO_EDGE 2

    mov inputq, [exe_paramq + 14 * 4]
    mov outputq, [exe_paramq + 14 * 4 + 8]
    mov kernelq, [exe_paramq + 14 * 4 + 2 * 8]

    mov yd, thread_start
.loop_y:
    mov xd, pad_size
    .loop_x:
        xor n_filterd, n_filterd
        xor kernel_posq, kernel_posq
        .loop_filter:
            xorps m2, m2
            xor kernel_yd, kernel_yd

            mov tmp1d, kernel_yd
            sub tmp1d, radius
            mov y_posd, dilation
            imul y_posd, tmp1d
            add y_posd, yd

            .loop_kery:
                xor kernel_xd, kernel_xd

                mov tmp1d, kernel_xd
                sub tmp1d, radius
                mov x_posd, dilation
                imul x_posd, tmp1d
                add x_posd, xd

                .loop_kerx:
                    COUNT_INPUT
                    xor chad, chad
                    .loop_ch:
                        cmp tmp1d, -1
                        je .out

                        movsxdifnidn tmp1q, tmp1d
                        movups m0, [inputq + tmp1q * 4]
                        add tmp1d, 4
                        jmp .load_end

                        .out:
                        xorps m0, m0

                        .load_end:

                        movups m1, [kernelq + kernel_posq * 4]
                        add kernel_posq, 4

                        mulps m0, m1
                        addps m2, m0

                        add chad, 4
                        mov tmp2d, input_num
                        cmp chad, tmp2d
                        jl .loop_ch

                    add x_posd, dilation
                    add kernel_xd, 1
                    mov tmp1d, kernel_size
                    cmp kernel_xd, tmp1d
                    jl .loop_kerx

                add y_posd, dilation
                add kernel_yd, 1
                mov tmp1d, kernel_size
                cmp kernel_yd, tmp1d
                jl .loop_kery

            haddps m2, m2
            haddps m2, m2
            movsxdifnidn n_filterq, n_filterd
            movss [outputq + n_filterq * 4], m2

            add n_filterd, 1
            mov tmp1d, output_num
            cmp n_filterd, tmp1d
            jl .loop_filter

        mov tmp1d, output_num
        movsxdifnidn tmp1q, tmp1d
        shl tmp1d, 2
        add outputq, tmp1q
        add xd, 1
        mov tmp2d, width
        sub tmp2d, pad_size
        cmp xd, tmp2d
        jl .loop_x

    add yd, 1
    mov tmp1d, thread_end
    cmp yd, tmp1d
    jl .loop_y

    RET

; void ff_dnn_execute_layer_conv2d_avx2(const execute_param *exe_param);

INIT_YMM avx2
cglobal dnn_execute_layer_conv2d, 8, 15, 3, exe_param,\
    x, y, n_filter, cha, kernel_x, kernel_y, x_pos, y_pos, kernel_pos,\
    input, output, kernel, tmp1, tmp2

%define thread_start [exe_paramq]
%define thread_end [exe_paramq + 1 * 4]
%define input_num [exe_paramq + 2 * 4]
%define output_num [exe_paramq + 3 * 4]
%define kernel_size [exe_paramq + 4 * 4]
%define padding_method [exe_paramq + 5 * 4]
%define dilation [exe_paramq + 6 * 4]
%define pad_size [exe_paramq + 7 * 4]
%define width [exe_paramq + 8 * 4]
%define height [exe_paramq + 9 * 4]
%define radius [exe_paramq + 10 * 4]
%define src_linesize [exe_paramq + 11 * 4]
%define filter_size [exe_paramq + 12 * 4]
%define filter_linesize [exe_paramq + 13 * 4]
%define SAME_CLAMP_TO_EDGE 2

    mov inputq, [exe_paramq + 14 * 4]
    mov outputq, [exe_paramq + 14 * 4 + 8]
    mov kernelq, [exe_paramq + 14 * 4 + 2 * 8]

    mov yd, thread_start
.loop_y:
    mov xd, pad_size
    .loop_x:
        xor n_filterd, n_filterd
        xor kernel_posq, kernel_posq
        .loop_filter:
            xorps m2, m2
            xor kernel_yd, kernel_yd

            mov tmp1d, kernel_yd
            sub tmp1d, radius
            mov y_posd, dilation
            imul y_posd, tmp1d
            add y_posd, yd

            .loop_kery:
                xor kernel_xd, kernel_xd

                mov tmp1d, kernel_xd
                sub tmp1d, radius
                mov x_posd, dilation
                imul x_posd, tmp1d
                add x_posd, xd

                .loop_kerx:
                    COUNT_INPUT
                    xor chad, chad
                    .loop_ch:
                        cmp tmp1d, -1
                        je .out

                        movsxdifnidn tmp1q, tmp1d
                        movups m0, [inputq + tmp1q * 4]
                        add tmp1d, 8
                        jmp .load_end

                        .out:
                        xorps m0, m0

                        .load_end:

                        movups m1, [kernelq + kernel_posq * 4]
                        add kernel_posq, 8

                        mulps m0, m1
                        addps m2, m0

                        add chad, 8
                        mov tmp2d, input_num
                        cmp chad, tmp2d
                        jl .loop_ch

                    add x_posd, dilation
                    add kernel_xd, 1
                    mov tmp1d, kernel_size
                    cmp kernel_xd, tmp1d
                    jl .loop_kerx

                add y_posd, dilation
                add kernel_yd, 1
                mov tmp1d, kernel_size
                cmp kernel_yd, tmp1d
                jl .loop_kery

            vperm2f128 m1, m2, m2, 1
            addps m2, m1
            haddps m2, m2
            haddps m2, m2
            movsxdifnidn n_filterq, n_filterd
            movss [outputq + n_filterq * 4], xm2

            add n_filterd, 1
            mov tmp1d, output_num
            cmp n_filterd, tmp1d
            jl .loop_filter

        mov tmp1d, output_num
        movsxdifnidn tmp1q, tmp1d
        shl tmp1d, 2
        add outputq, tmp1q
        add xd, 1
        mov tmp2d, width
        sub tmp2d, pad_size
        cmp xd, tmp2d
        jl .loop_x

    add yd, 1
    mov tmp1d, thread_end
    cmp yd, tmp1d
    jl .loop_y

    RET
%endif

;*****************************************************************************
;* x86-optimized functions for threshold filter
;*
;* Copyright (C) 2017 Paul B Mahol
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
;*****************************************************************************

%include "libavutil/x86/x86util.asm"

SECTION_RODATA

pb_128: times 16 db 128
pb_128_0 : times 8 db 0, 128

SECTION .text

%macro DECL_MASK 2
%if mmsize < 64
    %xdefine %1 m%2
%else
    %assign  %%i %2 + 1
    %xdefine  %1 k %+ %%i
%endif
%endmacro

;%1 depth (8 or 16) ; %2 b or w ; %3 constant
%macro THRESHOLD 3
%if ARCH_X86_64
cglobal threshold%1, 10, 13, 5, in, threshold, min, max, out, ilinesize, tlinesize, flinesize, slinesize, olinesize, w, h, x
    mov             wd, dword wm
    mov             hd, dword hm
%else
cglobal threshold%1, 5, 7, 5, in, threshold, min, max, out, w, x
    mov             wd, r10m
%define     ilinesizeq  r5mp
%define     tlinesizeq  r6mp
%define     flinesizeq  r7mp
%define     slinesizeq  r8mp
%define     olinesizeq  r9mp
%define             hd  r11mp
%endif
    VBROADCASTI128  m4, [%3]
%if %1 == 16
    add             wq, wq ; w *= 2 (16 bits instead of 8)
%endif
    add            inq, wq
    add     thresholdq, wq
    add           minq, wq
    add           maxq, wq
    add           outq, wq
    neg             wq
.nextrow:
    mov         xq, wq

.loop:
    movu              m1, [inq + xq]
    movu              m0, [thresholdq + xq]
    movu              m2, [minq + xq]
    movu              m3, [maxq + xq]
    pxor              m0, m4
    pxor              m1, m4
    DECL_MASK       mask, 0
    pcmpgt%2        mask, m0, m1

%if mmsize == 64
    vpblendm%2  m3{mask}, m3, m2
%else
    PBLENDVB          m3, m2, mask
%endif

    movu     [outq + xq], m3
    add               xq, mmsize
    jl .loop

    add          inq, ilinesizeq
    add   thresholdq, tlinesizeq
    add         minq, flinesizeq
    add         maxq, slinesizeq
    add         outq, olinesizeq
    sub         hd, 1
    jg .nextrow
RET
%endmacro

INIT_XMM sse4
THRESHOLD 8, b, pb_128
THRESHOLD 16, w, pb_128_0

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
THRESHOLD 8, b, pb_128
THRESHOLD 16, w, pb_128_0
%endif

%if HAVE_AVX512_EXTERNAL
INIT_ZMM avx512
THRESHOLD 8, b, pb_128
THRESHOLD 16, w, pb_128_0
%endif

;*****************************************************************************
;* x86-optimized functions for exposure filter
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

;*******************************************************************************
; void ff_exposure(float *ptr, int length, float black, float scale);
;*******************************************************************************
%macro EXPOSURE 0
cglobal exposure, 2, 2, 4, ptr, length, black, scale
    movsxdifnidn lengthq, lengthd
%if WIN64
    VBROADCASTSS m0, xmm2
    VBROADCASTSS m1, xmm3
%else
    VBROADCASTSS m0, xmm0
    VBROADCASTSS m1, xmm1
%endif

.loop:
    movu        m2, [ptrq]
    subps       m2, m2, m0
    mulps       m2, m2, m1
    movu    [ptrq], m2
    add       ptrq, mmsize
    sub    lengthq, mmsize/4

    jg .loop

    RET
%endmacro

%if ARCH_X86_64
INIT_XMM sse
EXPOSURE
%endif

;*****************************************************************************
;* x86-optimized functions for gblur filter
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

%macro KXNOR 3
%if cpuflag(avx512)
kxnorw %1, %2, %3
%else
kmovd %1, [gblur_mask]
%endif
%endmacro

SECTION .data align=64
gblur_mask: dd 0xffffffff
SECTION .data align=64

gblur_transpose_16x16_indices1: dq 2, 3, 0, 1, 6, 7, 4, 5
gblur_transpose_16x16_indices2: dq 1, 0, 3, 2, 5, 4, 7, 6
gblur_transpose_16x16_indices3: dd 1, 0, 3, 2, 5 ,4 ,7 ,6 ,9 ,8 , 11, 10, 13, 12 ,15, 14
gblur_transpose_16x16_mask: dw 0xcc, 0x33, 0xaa, 0x55, 0xaaaa, 0x5555
gblur_vindex_width: dd 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15

SECTION .text

; void ff_horiz_slice_sse4(float *ptr, int width, int height, int steps,
;                          float nu, float bscale)

%macro HORIZ_SLICE 0
%if cpuflag(avx512) || cpuflag(avx2)
cglobal horiz_slice, 5, 12, 32, 0x50, buffer, width, height, steps, localbuf, x, y, step, stride, remain, ptr
%elif UNIX64
cglobal horiz_slice, 4, 9, 9, ptr, width, height, steps, x, y, step, stride, remain
%else
cglobal horiz_slice, 4, 9, 9, ptr, width, height, steps, nu, bscale, x, y, step, stride, remain
%endif
%if cpuflag(avx512) || cpuflag(avx2)
%assign rows mmsize/4
    VBROADCASTSS  m0, xmm0 ; nu
    VBROADCASTSS  m1, xmm1 ; bscale

%if cpuflag(avx512)
    vpbroadcastd  m2, widthd
    kmovw k6, [gblur_transpose_16x16_mask + 0*2]
    kmovw k5, [gblur_transpose_16x16_mask + 1*2]
    kmovw k4, [gblur_transpose_16x16_mask + 2*2]
    kmovw k3, [gblur_transpose_16x16_mask + 3*2]
    kmovw k2, [gblur_transpose_16x16_mask + 4*2]
    kmovw k1, [gblur_transpose_16x16_mask + 5*2]

    movu m28, [gblur_transpose_16x16_indices1]
    movu m29, [gblur_transpose_16x16_indices2]
    movu m30, [gblur_transpose_16x16_indices3]
%else
    movd         xm2, widthd
    VBROADCASTSS  m2, xm2
%endif
    vpmulld       m2, m2, [gblur_vindex_width] ; vindex width

    xor yq, yq ; y = 0
    xor xq, xq ; x = 0

    cmp heightq, rows
    jl .y_scalar
    sub heightq, rows
    .loop_y:
        xor stepd, stepd
        .loop_step:
            mov  ptrq, yq
            imul ptrq, widthq
            lea  ptrq, [bufferq + ptrq*4] ; ptr = buffer + y * width;

            KXNOR  k7, k7, k7
            vgatherdps m3{k7}, [ptrq + m2*4]
            mulps m3, m1
            movu [localbufq], m3
            inc xq
            add ptrq, 4 ; ptr++

            ; Filter rightwards
            .loop_x:
                KXNOR k7, k7, k7
                vgatherdps m4{k7}, [ptrq + m2*4]
                FMULADD_PS m3, m3, m0, m4, m3
                add localbufq, mmsize
                movu [localbufq], m3
                add ptrq, 4

                inc xq
                cmp xq, widthq
                jl .loop_x

            sub ptrq, 4
            mulps m3, m1
            KXNOR k7, k7, k7
            vscatterdps [ptrq + m2*4]{k7}, m3

%if !cpuflag(avx512)
            movu [rsp + 0x10], m2
%endif

            ; Filter leftwards
            dec xq
            .loop_x_back:
                sub localbufq, rows * mmsize
%if cpuflag(avx512)
                ; Read 16x16 matrix from local buffer
                ; row1-row16 => m19-m4
                mova m19, m3
                vfmadd213ps m19, m0, [localbufq + 0xf * mmsize]
                mova m18, m19
                vfmadd213ps m18, m0, [localbufq + 0xe * mmsize]
                mova m17, m18
                vfmadd213ps m17, m0, [localbufq + 0xd * mmsize]
                mova m16, m17
                vfmadd213ps m16, m0, [localbufq + 0xc * mmsize]
                mova m15, m16
                vfmadd213ps m15, m0, [localbufq + 0xb * mmsize]
                mova m14, m15
                vfmadd213ps m14, m0, [localbufq + 0xa * mmsize]
                mova m13, m14
                vfmadd213ps m13, m0, [localbufq + 0x9 * mmsize]
                mova m12, m13
                vfmadd213ps m12, m0, [localbufq + 0x8 * mmsize]
                mova m11, m12
                vfmadd213ps m11, m0, [localbufq + 0x7 * mmsize]
                mova m10, m11
                vfmadd213ps m10, m0, [localbufq + 0x6 * mmsize]
                mova m9, m10
                vfmadd213ps m9,  m0, [localbufq + 0x5 * mmsize]
                mova m8, m9
                vfmadd213ps m8,  m0, [localbufq + 0x4 * mmsize]
                mova m7, m8
                vfmadd213ps m7,  m0, [localbufq + 0x3 * mmsize]
                mova m6, m7
                vfmadd213ps m6,  m0, [localbufq + 0x2 * mmsize]
                mova m5, m6
                vfmadd213ps m5,  m0, [localbufq + 0x1 * mmsize]
                mova m4, m5
                vfmadd213ps m4,  m0, [localbufq + 0x0 * mmsize]

                mova m3, m4 ; temp value used by next cycle

                ; Transpose 16x16 matrix
                vinserti64x4 m20, m4,  ym12, 0x1 ; t0[511: 0] = t0[255:0], t8[255:0]
                vinserti64x4 m21, m5,  ym13, 0x1 ; t1[511: 0] = t1[255:0], t9[255:0]
                vinserti64x4 m22, m6,  ym14, 0x1 ; t2[511: 0] = t2[255:0], ta[255:0]
                vinserti64x4 m23, m7,  ym15, 0x1 ; t3[511: 0] = t3[255:0], tb[255:0]
                vinserti64x4 m24, m8,  ym16, 0x1 ; t4[511: 0] = t4[255:0], tc[255:0]
                vinserti64x4 m25, m9,  ym17, 0x1 ; t5[511: 0] = t5[255:0], td[255:0]
                vinserti64x4 m26, m10, ym18, 0x1 ; t6[511: 0] = t6[255:0], te[255:0]
                vinserti64x4 m27, m11, ym19, 0x1 ; t7[511: 0] = t7[255:0], tf[255:0]

                vextractf64x4 ym4, m4, 0x1
                vextractf64x4 ym12, m12, 0x1
                vinserti64x4 m4, m4, ym12, 0x1   ; t8[511: 0] = t0[511:255], t8[511:255]

                vextractf64x4 ym5,  m5,  0x1
                vextractf64x4 ym13, m13, 0x1
                vinserti64x4 m5, m5, ym13, 0x1   ; t9[511: 0] = t1[511:255], t9[511:255]

                vextractf64x4 ym6,  m6,  0x1
                vextractf64x4 ym14, m14, 0x1
                vinserti64x4 m6, m6, ym14, 0x1   ; ta[511: 0] = t2[511:255], ta[511:255]

                vextractf64x4 ym7,  m7,  0x1
                vextractf64x4 ym15, m15, 0x1
                vinserti64x4 m7, m7, ym15, 0x1   ; tb[511: 0] = t3[511:255], tb[511:255]

                vextractf64x4 ym8,  m8,  0x1
                vextractf64x4 ym16, m16, 0x1
                vinserti64x4 m8, m8, ym16, 0x1   ; tc[511: 0] = t4[511:255], tc[511:255]

                vextractf64x4 ym9,  m9,  0x1
                vextractf64x4 ym17, m17, 0x1
                vinserti64x4 m9, m9, ym17, 0x1   ; td[511: 0] = t5[511:255], td[511:255]

                vextractf64x4 ym10, m10,  0x1
                vextractf64x4 ym18, m18, 0x1
                vinserti64x4 m10, m10, ym18, 0x1 ; te[511: 0] = t6[511:255], te[511:255]

                vextractf64x4 ym11, m11,  0x1
                vextractf64x4 ym19, m19, 0x1
                vinserti64x4 m11, m11, ym19, 0x1 ; tf[511: 0] = t7[511:255], tf[511:255]

                ; (t0-t7, t8-tf) => (m20-m27, m4-m11)
                mova m12, m20 ; t0
                mova m13, m21 ; t1
                mova m14, m22 ; t2
                mova m15, m23 ; t3
                vpermq m12{k6}, m28, m24  ; r0[511:0] = permute(t0{k6[0x33]}, index1, t4)
                vpermq m13{k6}, m28, m25  ; r1[511:0] = permute(t1{k6[0x33]}, index1, t5)
                vpermq m14{k6}, m28, m26  ; r2[511:0] = permute(t2{k6[0x33]}, index1, t6)
                vpermq m15{k6}, m28, m27  ; r3[511:0] = permute(t3{k6[0x33]}, index1, t7)
                vpermq m24{k5}, m28, m20  ; r4[511:0] = permute(t4{k5[0xcc]}, index1, t0)
                vpermq m25{k5}, m28, m21  ; r5[511:0] = permute(t5{k5[0xcc]}, index1, t1)
                vpermq m26{k5}, m28, m22  ; r6[511:0] = permute(t5{k5[0xcc]}, index1, t2)
                vpermq m27{k5}, m28, m23  ; r7[511:0] = permute(t5{k5[0xcc]}, index1, t3)

                mova m16, m4 ; t8
                mova m17, m5 ; t9
                mova m18, m6 ; ta
                mova m19, m7 ; tb
                vpermq m16{k6}, m28, m8  ; r8[511:0] = permute(t8{k6[0x33]}, index1, tc)
                vpermq m17{k6}, m28, m9  ; r9[511:0] = permute(t9{k6[0x33]}, index1, td)
                vpermq m18{k6}, m28, m10 ; ra[511:0] = permute(ta{k6[0x33]}, index1, te)
                vpermq m19{k6}, m28, m11 ; rb[511:0] = permute(tb{k6[0x33]}, index1, tf)
                vpermq m8{k5},  m28, m4  ; rc[511:0] = permute(tc{k5[0xcc]}, index1, t8)
                vpermq m9{k5},  m28, m5  ; rd[511:0] = permute(td{k5[0xcc]}, index1, t9)
                vpermq m10{k5}, m28, m6  ; re[511:0] = permute(te{k5[0xcc]}, index1, ta)
                vpermq m11{k5}, m28, m7  ; rf[511:0] = permute(tf{k5[0xcc]}, index1, tb)

                ; Free registers: m20-m23, m4-m7
                ; (r0-r3, r4-r7, r8-rb, rc-rf) => (m12-m15, m24-m27, m16-m19, m8-m11)
                mova m4,  m12 ; r0
                mova m5,  m13 ; r1
                mova m6,  m24 ; r4
                mova m7,  m25 ; r5
                mova m20, m16 ; r8
                mova m21, m17 ; r9
                mova m22, m8  ; rc
                mova m23, m9  ; rd
                vpermq m4{k4},  m29, m14  ; t0[511:0] = permute(r0{k4[0xaa]}, index2, r2)
                vpermq m5{k4},  m29, m15  ; t1[511:0] = permute(r1{k4[0xaa]}, index2, r3)
                vpermq m14{k3}, m29, m12  ; t2[511:0] = permute(r2{k3[0x55]}, index2, r0)
                vpermq m15{k3}, m29, m13  ; t3[511:0] = permute(r3{k3[0x55]}, index2, r1)
                vpermq m6{k4},  m29, m26  ; t4[511:0] = permute(r4{k4[0xaa]}, index2, r6)
                vpermq m7{k4},  m29, m27  ; t5[511:0] = permute(r5{k4[0xaa]}, index2, r7)
                vpermq m26{k3}, m29, m24  ; t6[511:0] = permute(r6{k3[0x55]}, index2, r4)
                vpermq m27{k3}, m29, m25  ; t7[511:0] = permute(r7{k3[0x55]}, index2, r5)
                vpermq m20{k4}, m29, m18  ; t8[511:0] = permute(r8{k4[0xaa]}, index2, ra)
                vpermq m21{k4}, m29, m19  ; t9[511:0] = permute(r9{k4[0xaa]}, index2, rb)
                vpermq m18{k3}, m29, m16  ; ta[511:0] = permute(ra{k3[0x55]}, index2, r8)
                vpermq m19{k3}, m29, m17  ; tb[511:0] = permute(rb{k3[0x55]}, index2, r9)
                vpermq m22{k4}, m29, m10  ; tc[511:0] = permute(rc{k4[0xaa]}, index2, re)
                vpermq m23{k4}, m29, m11  ; td[511:0] = permute(rd{k4[0xaa]}, index2, rf)
                vpermq m10{k3}, m29, m8   ; te[511:0] = permute(re{k3[0x55]}, index2, rc)
                vpermq m11{k3}, m29, m9   ; tf[511:0] = permute(rf{k3[0x55]}, index2, rd)

                ; Free registers: m8, m9, m16, m17, m24, m25, m12, m13
                ; t0  t1  t2   t3   t4   t5   t6   t7  t8  t9   ta   tb   tc   td   te   tf
                ; m4 m5 m14 m15 m6 m7 m26 m27 m20 m21 m18 m19 m22 m23 m10 m11
                mova m8,  m4  ; t0
                mova m9,  m14 ; t2
                mova m16, m6  ; t4
                mova m17, m26 ; t6
                mova m24, m20 ; t8
                mova m25, m18 ; ta
                mova m12, m22 ; tc
                mova m13, m10 ; te

                vpermd m8{k2},  m30, m5   ; r0[511:0] = permute(t0{k2[0x5555]}, index3, t1)
                vpermd m5{k1},  m30, m4   ; r1[511:0] = permute(t1{k1[0xaaaa]}, index3, t0)
                vpermd m9{k2},  m30, m15  ; r2[511:0] = permute(t2{k2[0x5555]}, index3, t3)
                vpermd m15{k1}, m30, m14  ; r3[511:0] = permute(t3{k1[0xaaaa]}, index3, t2)
                vpermd m16{k2}, m30, m7   ; r4[511:0] = permute(t4{k2[0x5555]}, index3, t5)
                vpermd m7{k1},  m30, m6   ; r5[511:0] = permute(t5{k1[0xaaaa]}, index3, t4)
                vpermd m17{k2}, m30, m27  ; r6[511:0] = permute(t6{k2[0x5555]}, index3, t7)
                vpermd m27{k1}, m30, m26  ; r7[511:0] = permute(t7{k1[0xaaaa]}, index3, t6)
                vpermd m24{k2}, m30, m21  ; r8[511:0] = permute(t8{k2[0x5555]}, index3, t9)
                vpermd m21{k1}, m30, m20  ; r9[511:0] = permute(t9{k1[0xaaaa]}, index3, t8)
                vpermd m25{k2}, m30, m19  ; ra[511:0] = permute(ta{k2[0x5555]}, index3, tb)
                vpermd m19{k1}, m30, m18  ; rb[511:0] = permute(tb{k1[0xaaaa]}, index3, ta)
                vpermd m12{k2}, m30, m23  ; rc[511:0] = permute(tc{k2[0x5555]}, index3, td)
                vpermd m23{k1}, m30, m22  ; rd[511:0] = permute(td{k1[0xaaaa]}, index3, tc)
                vpermd m13{k2}, m30, m11  ; re[511:0] = permute(te{k2[0x5555]}, index3, tf)
                vpermd m11{k1}, m30, m10  ; rf[511:0] = permute(tf{k1[0xaaaa]}, index3, te)

                ; r0  r1  r2   r3   r4   r5   r6   r7  r8  r9   ra   rb   rc   rd   re   rf
                ; m8 m5 m9 m15 m16 m7 m17 m27 m24 m21 m25 m19 m12 m23 m13 m11

                ; move back to buffer
                sub ptrq, mmsize
                movu [ptrq + strideq*0], m8
                mov strideq, widthq
                movu [ptrq + strideq*4], m5
                add strideq, widthq
                movu [ptrq + strideq*4], m9
                add strideq, widthq
                movu [ptrq + strideq*4], m15
                add strideq, widthq
                movu [ptrq + strideq*4], m16
                add strideq, widthq
                movu [ptrq + strideq*4], m7
                add strideq, widthq
                movu [ptrq + strideq*4], m17
                add strideq, widthq
                movu [ptrq + strideq*4], m27
                add strideq, widthq
                movu [ptrq + strideq*4], m24
                add strideq, widthq
                movu [ptrq + strideq*4], m21
                add strideq, widthq
                movu [ptrq + strideq*4], m25
                add strideq, widthq
                movu [ptrq + strideq*4], m19
                add strideq, widthq
                movu [ptrq + strideq*4], m12
                add strideq, widthq
                movu [ptrq + strideq*4], m23
                add strideq, widthq
                movu [ptrq + strideq*4], m13
                add strideq, widthq
                movu [ptrq + strideq*4], m11
%else
                ; Read 8x8 matrix from local buffer
                ; t0-t7 => m9-m2
                mova m9, m3
                vfmadd213ps m9, m0, [localbufq + 0x7 * mmsize]
                mova m8, m9
                vfmadd213ps m8, m0, [localbufq + 0x6 * mmsize]
                mova m7, m8
                vfmadd213ps m7, m0, [localbufq + 0x5 * mmsize]
                mova m6, m7
                vfmadd213ps m6, m0, [localbufq + 0x4 * mmsize]
                mova m5, m6
                vfmadd213ps m5, m0, [localbufq + 0x3 * mmsize]
                mova m4, m5
                vfmadd213ps m4, m0, [localbufq + 0x2 * mmsize]
                mova m3, m4
                vfmadd213ps m3, m0, [localbufq + 0x1 * mmsize]
                mova m2, m3
                vfmadd213ps m2, m0, [localbufq + 0x0 * mmsize]

                ; Transpose 8x8
                ; Limit the number ym# register to 16 for compatibility with VEX
                sub ptrq, mmsize

                vinsertf128 m10, m2, xm6, 0x1
                vinsertf128 m11, m3, xm7, 0x1
                vunpcklpd m12, m10, m11
                vunpckhpd m13, m10, m11

                vinsertf128 m10, m4, xm8, 0x1
                vinsertf128 m11, m5, xm9, 0x1
                vunpcklpd m14, m10, m11
                vunpckhpd m15, m10, m11

                vshufps m10, m12, m14, 0x88
                vshufps m11, m12, m14, 0xDD
                movu [ptrq + strideq*0], m10
                mov strideq, widthq
                movu [ptrq + strideq*4], m11
                add strideq, widthq

                vshufps m10, m13, m15, 0x88
                vshufps m11, m13, m15, 0xDD
                movu [ptrq + strideq*4], m10
                add strideq, widthq
                movu [ptrq + strideq*4], m11
                add strideq, widthq

                mova m10, m2
                vextractf128 xm10, m10, 0x1
                vextractf128 xm3,  m3,  0x1
                vinsertf128 m6, m6, xm10, 0x0
                vinsertf128 m7, m7, xm3,  0x0
                vunpcklpd m12, m6, m7
                vunpckhpd m13, m6, m7

                mova m3, m2 ; backup for next cycle

                vextractf128 xm4, ym4, 0x1
                vextractf128 xm5, ym5, 0x1
                vinsertf128 m8, m8, xm4, 0x0
                vinsertf128 m9, m9, xm5, 0x0
                vunpcklpd m14, m8, m9
                vunpckhpd m15, m8, m9

                vshufps m10, m12, m14, 0x88
                vshufps m11, m12, m14, 0xDD
                movu [ptrq + strideq*4], m10
                add strideq, widthq
                movu [ptrq + strideq*4], m11
                add strideq, widthq

                vshufps m10, m13, m15, 0x88
                vshufps m11, m13, m15, 0xDD
                movu [ptrq + strideq*4], m10
                add strideq, widthq
                movu [ptrq + strideq*4], m11
%endif
                sub xq, rows
                cmp xq, rows
                jge .loop_x_back

%if !cpuflag(avx512)
                movu m2, [rsp + 0x10]
%endif

            .loop_x_scalar:
                sub ptrq, 0x4
                sub localbufq, mmsize
                vfmadd213ps m3, m0, [localbufq]
                KXNOR k7, k7, k7
                vscatterdps [ptrq + m2*4]{k7}, m3
                dec xq
                cmp xq, 0
                jg .loop_x_scalar

            inc stepd
            cmp stepd, stepsd
            jl .loop_step

        add yq, rows
        cmp yq, heightq
        jle .loop_y

    add heightq, rows
    cmp yq, heightq
    jge .end_scalar

    mov remainq, widthq
    imul remainq, mmsize
    add ptrq, remainq

    .y_scalar:
        xor remainq, remainq
        mov remainq, heightq
        sub remainq, yq
        mov xw, 1
        shlx remainq, xq, remainq
        sub remainw, 1
        kmovd k1, remaind

        xor stepq, stepq
        .loop_step_scalar:
            mov xq, 1
            mov  ptrq, yq
            imul ptrq, widthq
            lea  ptrq, [bufferq + ptrq * 4] ; ptrq = buffer + y * width
            vxorps m3, m3
            kmovw k7, k1
            vgatherdps m3{k7}, [ptrq + m2 * 4]
            mulps m3, m1 ; p0 *= bscale
            movu [localbufq], m3

            ; Filter rightwards
            .y_scalar_loop_x:
                add ptrq, 4
                kmovw k7, k1
                vgatherdps m4{k7}, [ptrq + m2 * 4]
                FMULADD_PS m3, m3, m0, m4, m3
                add localbufq, mmsize
                movu [localbufq], m3

                inc xq
                cmp xq, widthq
                jl .y_scalar_loop_x

            mulps m3, m1 ; p0 *= bscale
            kmovw k7, k1
            vscatterdps [ptrq + m2 * 4]{k7}, m3
            dec xq

            ; Filter leftwards
            .y_scalar_loop_x_back:
                sub ptrq, 4
                sub localbufq, mmsize
                vfmadd213ps m3, m0, [localbufq]
                kmovw k7, k1
                vscatterdps [ptrq + m2 * 4]{k7}, m3
                dec xq
                cmp xq, 0
                jg .y_scalar_loop_x_back

            inc stepd
            cmp stepd, stepsd
            jl .loop_step_scalar

    .end_scalar:
    RET
%else
%if WIN64
    movss m0, num
    movss m1, bscalem
    DEFINE_ARGS ptr, width, height, steps, x, y, step, stride, remain
%endif
    movsxdifnidn widthq, widthd

    mulss m2, m0, m0 ; nu ^ 2
    mulss m3, m2, m0 ; nu ^ 3
    mulss m4, m3, m0 ; nu ^ 4
    xor   xq, xq
    xor   yd, yd
    mov   strideq, widthq
    ; stride = width * 4
    shl   strideq, 2
    ; w = w - ((w - 1) & 3)
    mov   remainq, widthq
    sub   remainq, 1
    and   remainq, 3
    sub   widthq, remainq

    shufps m0, m0, 0
    shufps m2, m2, 0
    shufps m3, m3, 0
    shufps m4, m4, 0

.loop_y:
    xor   stepd, stepd

    .loop_step:
        ; p0 *= bscale
        mulss m5, m1, [ptrq + xq * 4]
        movss [ptrq + xq * 4], m5
        inc xq
        ; filter rightwards
        ; Here we are vectorizing the c version by 4
        ;    for (x = 1; x < width; x++)
        ;       ptr[x] += nu * ptr[x - 1];
        ;   let p0 stands for ptr[x-1], the data from last loop
        ;   and [p1,p2,p3,p4] be the vector data for this loop.
        ; Unrolling the loop, we get:
        ;   p1' = p1 + p0*nu
        ;   p2' = p2 + p1*nu + p0*nu^2
        ;   p3' = p3 + p2*nu + p1*nu^2 + p0*nu^3
        ;   p4' = p4 + p3*nu + p2*nu^2 + p1*nu^3 + p0*nu^4
        ; so we can do it in simd:
        ; [p1',p2',p3',p4'] = [p1,p2,p3,p4] + [p0,p1,p2,p3]*nu +
        ;                     [0,p0,p1,p2]*nu^2 + [0,0,p0,p1]*nu^3 +
        ;                     [0,0,0,p0]*nu^4

        .loop_x:
            movu m6, [ptrq + xq * 4]         ; s  = [p1,p2,p3,p4]
            pslldq m7, m6, 4                 ;      [0, p1,p2,p3]
            movss  m7, m5                    ;      [p0,p1,p2,p3]
            FMULADD_PS  m6, m7, m0, m6, m8   ; s += [p0,p1,p2,p3] * nu
            pslldq m7, 4                     ;      [0,p0,p1,p2]
            FMULADD_PS  m6, m7, m2, m6, m8   ; s += [0,p0,p1,p2]  * nu^2
            pslldq m7, 4
            FMULADD_PS  m6, m7, m3, m6, m8   ; s += [0,0,p0,p1]   * nu^3
            pslldq m7, 4
            FMULADD_PS  m6, m7, m4, m6, m8   ; s += [0,0,0,p0]    * nu^4
            movu [ptrq + xq * 4], m6
            shufps m5, m6, m6, q3333
            add xq, 4
            cmp xq, widthq
            jl .loop_x

        add widthq, remainq
        cmp xq, widthq
        jge .end_scalar

        .loop_scalar:
            ; ptr[x] += nu * ptr[x-1]
            movss m5, [ptrq + 4*xq - 4]
            mulss m5, m0
            addss m5, [ptrq + 4*xq]
            movss [ptrq + 4*xq], m5
            inc xq
            cmp xq, widthq
            jl .loop_scalar
        .end_scalar:
            ; ptr[width - 1] *= bscale
            dec xq
            mulss m5, m1, [ptrq + 4*xq]
            movss [ptrq + 4*xq], m5
            shufps m5, m5, 0

        ; filter leftwards
        ;    for (; x > 0; x--)
        ;        ptr[x - 1] += nu * ptr[x];
        ; The idea here is basically the same as filter rightwards.
        ; But we need to take care as the data layout is different.
        ; Let p0 stands for the ptr[x], which is the data from last loop.
        ; The way we do it in simd as below:
        ; [p-4', p-3', p-2', p-1'] = [p-4, p-3, p-2, p-1]
        ;                          + [p-3, p-2, p-1, p0] * nu
        ;                          + [p-2, p-1, p0,  0]  * nu^2
        ;                          + [p-1, p0,  0,   0]  * nu^3
        ;                          + [p0,  0,   0,   0]  * nu^4
        .loop_x_back:
            sub xq, 4
            movu m6, [ptrq + xq * 4]      ; s = [p-4, p-3, p-2, p-1]
            psrldq m7, m6, 4              ;     [p-3, p-2, p-1, 0  ]
            blendps m7, m5, 0x8           ;     [p-3, p-2, p-1, p0 ]
            FMULADD_PS m6, m7, m0, m6, m8 ; s+= [p-3, p-2, p-1, p0 ] * nu
            psrldq m7, 4                  ;
            FMULADD_PS m6, m7, m2, m6, m8 ; s+= [p-2, p-1, p0,  0] * nu^2
            psrldq m7, 4
            FMULADD_PS m6, m7, m3, m6, m8 ; s+= [p-1, p0,   0,  0] * nu^3
            psrldq m7, 4
            FMULADD_PS m6, m7, m4, m6, m8 ; s+= [p0,  0,    0,  0] * nu^4
            movu [ptrq + xq * 4], m6
            shufps m5, m6, m6, 0          ; m5 = [p-4', p-4', p-4', p-4']
            cmp xq, remainq
            jg .loop_x_back

        cmp xq, 0
        jle .end_scalar_back

        .loop_scalar_back:
            ; ptr[x-1] += nu * ptr[x]
            movss m5, [ptrq + 4*xq]
            mulss m5, m0
            addss m5, [ptrq + 4*xq - 4]
            movss [ptrq + 4*xq - 4], m5
            dec xq
            cmp xq, 0
            jg .loop_scalar_back
        .end_scalar_back:

        ; reset aligned width for next line
        sub widthq, remainq

        inc stepd
        cmp stepd, stepsd
        jl .loop_step

    add ptrq, strideq
    inc yd
    cmp yd, heightd
    jl .loop_y

    RET
%endif
%endmacro

%if ARCH_X86_64
INIT_XMM sse4
HORIZ_SLICE

INIT_YMM avx2
HORIZ_SLICE

INIT_ZMM avx512
HORIZ_SLICE

%endif

%macro POSTSCALE_SLICE 0
cglobal postscale_slice, 2, 2, 4, ptr, length, postscale, min, max
    shl lengthd, 2
    add ptrq, lengthq
    neg lengthq
%if ARCH_X86_32
    VBROADCASTSS m0, postscalem
    VBROADCASTSS m1, minm
    VBROADCASTSS m2, maxm
%elif WIN64
    SWAP 0, 2
    SWAP 1, 3
    VBROADCASTSS m0, xm0
    VBROADCASTSS m1, xm1
    VBROADCASTSS m2, maxm
%elif cpuflag(avx512)
    VBROADCASTSS m0, xmm0
    VBROADCASTSS m1, xmm1
    VBROADCASTSS m2, xmm2
%else ; UNIX64
    VBROADCASTSS m0, xm0
    VBROADCASTSS m1, xm1
    VBROADCASTSS m2, xm2
%endif

    .loop:
%if cpuflag(avx2) || cpuflag(avx512)
    mulps         m3, m0, [ptrq + lengthq]
%else
    movu          m3, [ptrq + lengthq]
    mulps         m3, m0
%endif
    maxps         m3, m1
    minps         m3, m2
    movu   [ptrq+lengthq], m3

    add lengthq, mmsize
    jl .loop

    RET
%endmacro

INIT_XMM sse
POSTSCALE_SLICE

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
POSTSCALE_SLICE
%endif

%if HAVE_AVX512_EXTERNAL
INIT_ZMM avx512
POSTSCALE_SLICE
%endif

; void ff_verti_slice_avx512(float *buffer, int width, int height, int column_begin, int column_end,
;                         int steps, float nu, float bscale);

%macro VERTI_SLICE 0
cglobal verti_slice, 6, 12, 9, 0x20, buffer, width, height, cbegin, cend, steps, x, y, cwidth, step, ptr, stride
%assign cols mmsize / 4
    VBROADCASTSS m0, xmm0 ; nu
    VBROADCASTSS m1, xmm1 ; bscale

    mov cwidthq, cendq
    sub cwidthq, cbeginq

    lea strideq, [widthq * 4]

    xor xq, xq ; x = 0
    cmp cwidthq, cols
    jl .x_scalar
    cmp cwidthq, 0x0
    je .end_scalar

    sub cwidthq, cols
    .loop_x:
        xor stepq, stepq
        .loop_step:
            ; ptr = buffer + x + column_begin;
            lea ptrq, [xq + cbeginq]
            lea ptrq, [bufferq + ptrq*4]

            ;  ptr[15:0] *= bcale;
            movu m2, [ptrq]
            mulps m2, m1
            movu [ptrq], m2

            ; Filter downwards
            mov yq, 1
            .loop_y_down:
                add ptrq, strideq ; ptrq += width
                movu m3, [ptrq]
                FMULADD_PS m2, m2, m0, m3, m2
                movu [ptrq], m2

                inc yq
                cmp yq, heightq
                jl .loop_y_down

            mulps m2, m1
            movu [ptrq], m2

            ; Filter upwards
            dec yq
            .loop_y_up:
                sub ptrq, strideq
                movu m3, [ptrq]
                FMULADD_PS m2, m2, m0, m3, m2
                movu [ptrq], m2

                dec yq
                cmp yq, 0
                jg .loop_y_up

            inc stepq
            cmp stepq, stepsq
            jl .loop_step

        add xq, cols
        cmp xq, cwidthq
        jle .loop_x

    add cwidthq, cols
    cmp xq, cwidthq
    jge .end_scalar

    .x_scalar:
        xor stepq, stepq
        mov qword [rsp + 0x10], xq
        sub cwidthq, xq
        mov xq, 1
        shlx cwidthq, xq, cwidthq
        sub cwidthq, 1
        kmovd k1, cwidthd
        mov xq, qword [rsp + 0x10]

        .loop_step_scalar:
            lea ptrq, [xq + cbeginq]
            lea ptrq, [bufferq + ptrq*4]

            kmovw k7, k1
            vmovups m2{k7}, [ptrq]
            mulps m2, m1
            kmovw k7, k1
            vmovups [ptrq]{k7}, m2

            ; Filter downwards
            mov yq, 1
            .x_scalar_loop_y_down:
                add ptrq, strideq
                kmovw k7, k1
                vmovups m3{k7}, [ptrq]
                FMULADD_PS m2, m2, m0, m3, m2
                kmovw k7, k1
                vmovups [ptrq]{k7}, m2

                inc yq
                cmp yq, heightq
                jl .x_scalar_loop_y_down

            mulps m2, m1
            kmovw k7, k1
            vmovups [ptrq]{k7}, m2

            ; Filter upwards
            dec yq
            .x_scalar_loop_y_up:
                sub ptrq, strideq
                kmovw k7, k1
                vmovups m3{k7}, [ptrq]
                FMULADD_PS m2, m2, m0, m3, m2
                kmovw k7, k1
                vmovups [ptrq]{k7}, m2

                dec yq
                cmp yq, 0
                jg .x_scalar_loop_y_up

            inc stepq
            cmp stepq, stepsq
            jl .loop_step_scalar

    .end_scalar:
    RET
%endmacro

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
VERTI_SLICE
%endif

%if HAVE_AVX512_EXTERNAL
INIT_ZMM avx512
VERTI_SLICE
%endif

/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <float.h>
#include <string.h>
#include "checkasm.h"
#include "libavfilter/exposure.h"

#define PIXELS 256
#define BUF_SIZE (PIXELS * 4)

#define randomize_buffers(buf, size)             \
    do {                                         \
        int j;                                   \
        float *tmp_buf = (float *)buf;           \
        for (j = 0; j < size; j++)               \
            tmp_buf[j] = (float)(rnd() & 0xFF); \
    } while (0)

void checkasm_check_vf_exposure(void)
{
    float *dst_ref[BUF_SIZE] = { 0 };
    float *dst_new[BUF_SIZE] = { 0 };
    ExposureContext s;

    s.exposure = 0.5f;
    s.black = 0.1f;
    s.scale = 1.f / (exp2f(-s.exposure) - s.black);

    randomize_buffers(dst_ref, PIXELS);
    memcpy(dst_new, dst_ref, BUF_SIZE);

    ff_exposure_init(&s);

    if (check_func(s.exposure_func, "exposure")) {
        declare_func(void, float *dst, int length, float black, float scale);
        call_ref(dst_ref, PIXELS, s.black, s.scale);
        call_new(dst_new, PIXELS, s.black, s.scale);
        if (!float_near_abs_eps_array(dst_ref, dst_new, 0.01f, PIXELS))
            fail();
        bench_new(dst_new, PIXELS, s.black, s.scale);
    }
    report("exposure");
}

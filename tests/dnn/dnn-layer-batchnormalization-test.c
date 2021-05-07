/*
 * Copyright (c) 2021
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

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "libavfilter/dnn/dnn_backend_native_layer_batchnormalization.h"
#include "libavutil/avassert.h"

#define EPSON 0.00001

static int test(void)
{
    // the input data is generated with below python code.
    /*
    import numpy as np

    in_data = np.random.rand(1, 5, 4, 3)

    print('input_data:')
    print(in_data.shape)
    print(list(in_data.flatten()))
    */
    DnnLayerBatchnormalizationParams params;
    DnnOperand operands[2];
    int32_t input_indexes[1];
    float input_data[1*5*4*3] = {
        0.03592029475149139, 0.8177031019998039, 0.7439828967882774, 0.12351433689912827, 0.10151796406770697, 0.3746884445121409, 0.17609107818009584,
        0.9180125678699762, 0.413786266728411, 0.15388845485290492, 0.6527147025219426, 0.8921708075708997, 0.9315511108099018, 0.6089771636865586,
        0.9096167333314585, 0.35798486932034357, 0.242417367935445, 0.3354539420380003, 0.5885835336919439, 0.2330763092626813, 0.19528398816193004,
        0.3537750057949528, 0.8908506440029637, 0.6745876262153178, 0.7975390896917102, 0.42115552951085633, 0.026252384244236926, 0.5987492499419249,
        0.2969445642379439, 0.6612326239920433, 0.5186612970859783, 0.9154125186721356, 0.11647732132450173, 0.32631441601002287, 0.5222573083721183,
        0.47294093689301375, 0.009744493333811666, 0.034835869186598645, 0.14643674017261454, 0.06114675456320906, 0.07678534832197992, 0.1968478741676435,
        0.3126360178524745, 0.6361975996395828, 0.18012602596211258, 0.8609077898378013, 0.8446431696067955, 0.7497824163136522, 0.8651808467465752,
        0.922506696896085, 0.6057714540829449, 0.6226843061111148, 0.3894522432026163, 0.15585932050469298, 0.5738772930525835, 0.4978427887293949,
        0.6015976578669766, 0.9169836327963268, 0.6957180046975255, 0.49003527036757355
    };

    float expected_output[1*5*4*3];
    float *output;
    int32_t channel = 3;
    float mean[3] = {0.1, 0.15, 0.14};
    float variance[3] = {0.12, 0.21, 0.24};
    float variance_eps = 0.0001;
    float scale = 1.0;
    float offset = 0.1;

    for (int i = 0; i < 1*5*4*3; ++i) {
        expected_output[i] = scale * (input_data[i] - mean[i % channel]) / sqrt(variance[i % channel] + variance_eps) + offset;
    }

    params.channel = channel;
    params.mean = av_malloc_array(params.channel, sizeof(*params.mean));
    if (!params.mean) {
        return 1;
    }
    params.mean[0] = mean[0];
    params.mean[1] = mean[1];
    params.mean[2] = mean[2];
    params.variance = av_malloc_array(params.channel, sizeof(*params.variance));
    if (!params.variance) {
        av_freep(&params.mean);
        return 1;
    }
    params.variance[0] = variance[0];
    params.variance[1] = variance[1];
    params.variance[2] = variance[2];
    params.scale = scale;
    params.offset = offset;
    params.variance_eps = variance_eps;

    operands[0].data = input_data;
    operands[0].dims[0] = 1;
    operands[0].dims[1] = 5;
    operands[0].dims[2] = 4;
    operands[0].dims[3] = 3;
    operands[1].data = NULL;

    input_indexes[0] = 0;
    ff_dnn_execute_layer_batchnormalization(operands, input_indexes, 1, &params, NULL);
    output = operands[1].data;
    for (int i = 0; i < sizeof(expected_output) / sizeof(float); i++) {
        if (fabs(output[i] - expected_output[i]) > EPSON) {
            printf("at index %d, output: %f, expected_output: %f\n", i, output[i], expected_output[i]);
            av_freep(&params.mean);
            av_freep(&params.variance);
            av_freep(&output);
            return 1;
        }
    }
    av_freep(&params.mean);
    av_freep(&params.variance);
    av_freep(&output);
    return 0;
}

int main(int argc, char **argv)
{
    return test();
}

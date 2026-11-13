// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib_ops/
// src/tlib_relu.cpp (Apache-2.0, Copyright 2026 lspr98): includes
// <algorithm> for std::max (tlib_tensor.h no longer pulls it in); a
// profile stage (profile.h).

#include "tlib_relu.h"

#include <algorithm>

#include <profile.h>

namespace tlib::ops
{

    Tensor<float> relu(const TensorView<float> &x)
    {
        STT_PROF(kElementwise);
        Tensor<float> y(x.Shape(), false, 0);

        const float *x_data{x.DataImm()};
        float *y_data{y.Data()};

        for (uint32_t i = 0; i < x.Numel(); i++)
        {
            y_data[i] = std::max<float>(x_data[i], 0.0f);
        }

        return y;
    }

} // tlib::ops
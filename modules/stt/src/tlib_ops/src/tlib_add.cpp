// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib_ops/
// src/tlib_add.cpp (Apache-2.0, Copyright 2026 lspr98): profile stages
// (profile.h).

#include "tlib_add.h"

#include <algorithm>

#include <profile.h>

namespace tlib::ops
{

    Tensor<float> add(const TensorView<float> &a, const TensorView<float> &b)
    {
        STT_PROF(kElementwise);
        assert(a.Dim() == b.Dim());

        const auto a_shape = a.Shape();
        const auto b_shape = b.Shape();

        assert(std::equal(a_shape.begin(), a_shape.end(), b_shape.begin()));

        Tensor<float> y(std::move(a_shape), false, 0);

        const float *a_data{a.DataImm()}, *b_data{b.DataImm()};
        float *y_data{y.Data()};

        for (uint32_t i = 0; i < y.Numel(); i++)
        {
            y_data[i] = a_data[i] + b_data[i];
        }

        return y;
    }

    Tensor<float> add_scale(const TensorView<float> &a, const TensorView<float> &b, const float b_scale)
    {
        STT_PROF(kElementwise);
        assert(a.Dim() == b.Dim());

        const auto a_shape = a.Shape();
        const auto b_shape = b.Shape();

        assert(std::equal(a_shape.begin(), a_shape.end(), b_shape.begin()));

        Tensor<float> y(std::move(a_shape), false, 0);

        const float *a_data{a.DataImm()}, *b_data{b.DataImm()};
        float *y_data{y.Data()};

        for (uint32_t i = 0; i < y.Numel(); i++)
        {
            y_data[i] = (a_data[i] + b_data[i]) * b_scale;
        }

        return y;
    }

    Tensor<float> add_scale_b(const TensorView<float> &a, const TensorView<float> &b, const float b_scale)
    {
        STT_PROF(kElementwise);
        assert(a.Dim() == b.Dim());

        const auto a_shape = a.Shape();
        const auto b_shape = b.Shape();

        assert(std::equal(a_shape.begin(), a_shape.end(), b_shape.begin()));

        Tensor<float> y(std::move(a_shape), false, 0);

        const float *a_data{a.DataImm()}, *b_data{b.DataImm()};
        float *y_data{y.Data()};

        for (uint32_t i = 0; i < y.Numel(); i++)
        {
            y_data[i] = a_data[i] + b_data[i] * b_scale;
        }

        return y;
    }

} // tlib::ops
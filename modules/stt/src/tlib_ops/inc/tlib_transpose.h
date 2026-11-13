// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib_ops/
// inc/tlib_transpose.h (Apache-2.0, Copyright 2026 lspr98): the output
// shape is a tlib::Dims instead of a std::vector; the copy walks x's
// strides row by row instead of dividing each element's coordinates out of
// its index (the same element order); a profile stage (profile.h).

#ifndef TLIB_TRANSPOSE_H_
#define TLIB_TRANSPOSE_H_

#include <cassert>
#include <utility>

#include <profile.h>
#include <tlib_tensor.h>

namespace tlib::ops
{

    /**
     * Transposes the given matrix x by swapping the two specified dimensions.
     * Important: Currently, only transposition between adjacent dimensions is supported.
     *
     * @param x         [...] dtype tensor, input
     * @param dim0      First dimension to swap
     * @param dim1      Second dimension to swap
     * @param location  Location for output buffer
     * @returns         [...] dtype tensor, output
     */

    template <typename dtype>
    inline Tensor<dtype> transpose(const TensorView<dtype> &x, const uint8_t dim0, const uint8_t dim1, const heap::Type location = heap::kDefaultLocation)
    {
        STT_PROF(kTranspose);
        assert(dim0 <= x.Dim());
        assert(dim1 <= x.Dim());
        assert(dim0 != dim1);

        auto y_shape = x.Shape();
        std::swap(y_shape.at(dim0), y_shape.at(dim1));
        Tensor<dtype> y(Dims(y_shape), x.Quantized(), x.Shift(), location);

        dtype *y_data{y.Data()};
        const dtype *x_data{x.DataImm()};

        const auto y_dim = y.Dim();
        auto x_stride = x.Stride();
        std::swap(x_stride.at(dim0), x_stride.at(dim1));

        // y in order, one row of its last dimension at a time; x's element
        // for y's coordinates c is at sum(c[j] * x_stride[j]). The
        // coordinates of the dimensions before the last count up like an
        // odometer, `base` following them.
        const uint32_t n = static_cast<uint32_t>(x.Numel());
        if (n == 0)
        {
            return y;
        }
        const uint32_t last = static_cast<uint32_t>(y_dim - 1);
        const uint32_t row = y_shape.at(last);
        const uint32_t step = x_stride.at(last);
        uint32_t coord[Dims::kMax] = {};
        uint32_t base = 0;
        for (uint32_t done = 0; done < n; done += row)
        {
            const dtype *src = x_data + base;
            for (uint32_t c = 0; c < row; c++)
            {
                *y_data++ = *src;
                src += step;
            }
            for (uint32_t j = last; j-- > 0;)
            {
                base += x_stride[j];
                if (++coord[j] < y_shape[j])
                {
                    break;
                }
                base -= coord[j] * x_stride[j];
                coord[j] = 0;
            }
        }

        return y;
    }

} // tlib::ops

#endif
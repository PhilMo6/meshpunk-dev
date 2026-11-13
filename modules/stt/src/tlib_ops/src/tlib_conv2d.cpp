// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib_ops/
// src/tlib_conv2d.cpp (Apache-2.0, Copyright 2026 lspr98): unfold walks
// channel, kernel row and kernel column in nested loops instead of dividing
// them out of each column index (the same bytes in the same order);
// profile stages (profile.h).

#include "tlib_conv2d.h"

#include <profile.h>
#include <tlib_heap.h>
#include <tlib_linear.h>
#include <tlib_transpose.h>

namespace tlib::ops
{

    Tensor<int8_t> unfold(const TensorView<int8_t> &x, const uint8_t kh, const uint8_t kw,
                          const uint8_t sh, const uint8_t sw, const uint8_t ph, const uint8_t pw,
                          const uint8_t dh, const uint8_t dw, const uint8_t oh, const uint8_t ow, const heap::Type location)
    {

        assert(x.Dim() == 3);

        const int32_t h{static_cast<int32_t>(x.Shape(1))}, w{static_cast<int32_t>(x.Shape(2))};
        const uint32_t out_rows = oh * ow, out_cols{x.Shape(0) * kh * kw};

        Tensor<int8_t> y({out_rows, out_cols}, x.Quantized(), x.Shift(), location);

        const int8_t *x_data{x.DataImm()};
        int8_t *y_data{y.Data()};
        const uint32_t channels{x.Shape(0)};

        // Row r = output position (oi, oj); its columns in order of
        // channel ci, kernel row ki, kernel column kj (column
        // ci * kh * kw + ki * kw + kj), 0 where the kernel is off the input.
        for (uint32_t r = 0; r < out_rows; r++)
        {
            const int32_t h_base = static_cast<int32_t>((r / ow) * sh) - ph;
            const int32_t w_base = static_cast<int32_t>((r % ow) * sw) - pw;

            for (uint32_t ci = 0; ci < channels; ci++)
            {
                const int8_t *x_ch = x_data + ci * h * w;
                for (uint32_t ki = 0; ki < kh; ki++)
                {
                    const int32_t h_src = h_base + static_cast<int32_t>(dh * ki);
                    const bool row_in = h_src >= 0 && h_src < h;
                    for (uint32_t kj = 0; kj < kw; kj++)
                    {
                        const int32_t w_src = w_base + static_cast<int32_t>(dw * kj);
                        *y_data++ = (row_in && w_src >= 0 && w_src < w) ? x_ch[h_src * w + w_src] : 0;
                    }
                }
            }
        }

        return y;
    }

    Tensor<int8_t> conv2d_relu(const TensorView<int8_t> &x, const TensorView<int8_t> &w, const TensorView<int32_t> &b,
                               const uint8_t oc, const uint8_t kh, const uint8_t kw, const uint8_t sh, const uint8_t sw,
                               const uint8_t ph, const uint8_t pw, const uint8_t dh, const uint8_t dw, const uint8_t y_shift)
    {
        STT_PROF(kConv2d);

        // Get input and output height and width
        const uint32_t ih = x.Shape(1);
        const uint32_t iw = x.Shape(2);
        const uint32_t oh = (ih + 2 * ph - dh * (kh - 1) - 1) / sh + 1;
        const uint32_t ow = (iw + 2 * pw - dw * (kw - 1) - 1) / sw + 1;

        // Unfold input tensor
        auto x_unfolded = unfold(x, kh, kw, sh, sw, ph, pw, dh, dw, oh, ow, heap::Type::PSRAM);

        // Calculate transposed convolution result
        auto y_T = linear_relu(x_unfolded, w, b, y_shift);

        // Transpose result
        auto y = transpose(y_T, 0, 1);

        // View result with correct shape
        y.View({oc, oh, ow});

        return y;
    }

    Tensor<int8_t> conv2d_relu(const TensorView<int8_t> &x, const TensorView<int8_t> &w,
                               const uint8_t oc, const uint8_t kh, const uint8_t kw, const uint8_t sh, const uint8_t sw,
                               const uint8_t ph, const uint8_t pw, const uint8_t dh, const uint8_t dw, const uint8_t y_shift)
    {
        STT_PROF(kConv2d);

        // Get input and output height and width
        const uint32_t ih = x.Shape(1);
        const uint32_t iw = x.Shape(2);
        const uint32_t oh = (ih + 2 * ph - dh * (kh - 1) - 1) / sh + 1;
        const uint32_t ow = (iw + 2 * pw - dw * (kw - 1) - 1) / sw + 1;

        // Unfold input tensor
        auto x_unfolded = unfold(x, kh, kw, sh, sw, ph, pw, dh, dw, oh, ow, heap::Type::SRAM);

        // Calculate convolution result
        auto y = linear_relu(w, x_unfolded, y_shift);

        // View result with correct shape
        y.View({oc, oh, ow});

        return y;
    }

} // tlib::ops
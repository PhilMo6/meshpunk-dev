// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib_ops/
// src/tlib_bmm.cpp (Apache-2.0, Copyright 2026 lspr98): bmm_relpos for the
// builds with the portable kernels.

#include "tlib_bmm.h"

#include <tlib_tensor.h>
#include <tlib_linear_impl.h>

#ifdef STT_KERNELS_C
#include <port.h>
#include <tlib_kernels_c.h>
#endif

namespace tlib::ops
{

    Tensor<float> bmm(const TensorView<float> &a, const TensorView<float> &b)
    {
        assert(a.Dim() == 3 && b.Dim() == 3);
        assert(a.Shape(0) == b.Shape(0));
        assert(a.Shape(2) == b.Shape(2));

        const uint32_t y_pages = a.Shape(0);
        const uint32_t y_rows = a.Shape(1);
        const uint32_t y_cols = b.Shape(1);
        const uint32_t n_inner = a.Shape(2);

        auto y = tlib::Tensor<float>({y_pages, y_rows, y_cols}, false, 0);

        float *y_data{y.Data()};
        const float *a_data{a.DataImm()}, *b_data{b.DataImm()};
        for (uint32_t p = 0; p < y_pages; p++)
        {
            impl::linear_impl(a_data, b_data, y_data, n_inner, y_rows, y_cols);
            a_data += a.Shape(1) * a.Shape(2);
            b_data += b.Shape(1) * b.Shape(2);
            y_data += y.Shape(1) * y.Shape(2);
        }

        return y;
    }

#ifdef STT_KERNELS_C
    Tensor<float> bmm_relpos(const TensorView<float> &q, const TensorView<float> &pos)
    {
        assert(q.Dim() == 3 && pos.Dim() == 3);
        assert(q.Shape(0) == pos.Shape(0));
        assert(q.Shape(2) == pos.Shape(2));
        const uint32_t pages = q.Shape(0);
        const uint32_t n = q.Shape(1);
        const uint32_t k = q.Shape(2);
        assert(n > 0 && pos.Shape(1) == 2 * n - 1);
        assert(k % 4 == 0);

        auto y = tlib::Tensor<float>({pages, n, n}, false, 0);

        float *y_data{y.Data()};
        const float *q_data{q.DataImm()}, *pos_data{pos.DataImm()};
        for (uint32_t p = 0; p < pages; p++)
        {
            kernels_c::relpos_f32(q_data, pos_data, y_data, k / 4, n);
            q_data += n * k;
            pos_data += (2 * n - 1) * k;
            y_data += n * n;
            stt_tile_done();
        }

        return y;
    }
#endif

} // tlib::ops

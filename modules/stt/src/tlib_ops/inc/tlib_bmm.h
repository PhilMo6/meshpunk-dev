// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib_ops/
// inc/tlib_bmm.h (Apache-2.0, Copyright 2026 lspr98): bmm_relpos for the
// builds with the portable kernels.

#ifndef TLIB_BMM_H_
#define TLIB_BMM_H_

#include <tlib_tensor.h>

namespace tlib::ops
{

    /**
     * Performs a batched matrix multiplication, y[i] = a[i]*b[i]^T
     * 
     * @param   [p, n, k] float tensor, input a
     * @param   [p, m, k] float tensor, input b
     *
     * @return  [p, n, m] float tensor, output
     */
    Tensor<float> bmm(const TensorView<float> &a, const TensorView<float> &b);

#ifdef STT_KERNELS_C
    /**
     * The attention's position scores (builds with the portable kernels):
     * q [p, n, d], pos [p, 2n - 1, d] -> [p, n, n], element [h][r][c] =
     * q[h][r] . pos[h][n - 1 - r + c], the elements of bmm(q, pos) that
     * conformer_attention.cpp's relative shift and slice keep, each the
     * same sum (tlib_kernels_c.h relpos).
     */
    Tensor<float> bmm_relpos(const TensorView<float> &q, const TensorView<float> &pos);
#endif

} // tlib::ops

#endif
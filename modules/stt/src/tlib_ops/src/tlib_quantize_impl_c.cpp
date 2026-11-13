// tlib_quantize_impl_c.cpp — tlib_quantize_impl.h for the builds without the
// ESP32-S3 assembly (build.ps1 -Variant s3c and rv32): the portable kernel
// of tlib_kernels_c.h, for the shifts the S3 kernels cover (0 to 7).

#include "tlib_quantize_impl.h"
#include "tlib_kernels_c.h"

#include <cassert>

namespace tlib::ops::impl
{

    void quantize_impl(const float *a, int8_t *a_q, uint32_t n, uint8_t shift)
    {
        assert(n % 4 == 0);
        assert(shift < 8);
        kernels_c::quantize(a, a_q, n / 4, shift);
    }

} // tlib::ops::impl

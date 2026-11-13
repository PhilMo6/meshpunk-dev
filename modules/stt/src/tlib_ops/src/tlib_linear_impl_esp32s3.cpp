// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib_ops/
// src_esp32s3/tlib_linear_impl_esp32s3.cpp (Apache-2.0, Copyright 2026
// lspr98): the assembly kernels are upstream's, unchanged. What changed is
// how they are fed: B goes through the internal-SRAM tile in blocks of whole
// rows (copied from memory, or read from the model file when it stays on SD),
// the matching bias rows go through the bias tile, a block's output is
// scattered into y's columns, and the plugin gets a call after each block
// (port.h stt_tile_done): the block driver, Tiled, is in tlib_linear_tiled.h.
// One core: the co-worker split is gone.

#include "tlib_linear_impl.h"
#include "tlib_linear_tiled.h"

#include <cassert>

extern "C"
{

    void tlib_linear_b_relu_impl_lshift_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint8_t lshift);
    void tlib_linear_b_relu_impl_rshift_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint8_t rshift);

    void tlib_linear_relu_impl_lshift_esp32s3(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint8_t lshift);
    void tlib_linear_relu_impl_rshift_esp32s3(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint8_t rshift);

    void tlib_linear_deq_impl_shift00_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift01_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift02_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift03_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift04_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift05_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift06_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift07_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift08_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift09_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift10_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift11_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift12_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift13_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_deq_impl_shift14_esp32s3(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);

    void tlib_linear_b_deq_impl_shift00_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift01_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift02_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift03_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift04_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift05_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift06_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift07_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift08_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift09_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift10_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift11_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift12_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift13_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);
    void tlib_linear_b_deq_impl_shift14_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m);

    void tlib_linear_impl_esp32s3(const float *a, const float *b, float *y, const uint32_t k4, const uint32_t n, const uint32_t m);
}

namespace tlib::ops::impl
{
    namespace
    {
        using DeqKernel = void (*)(const int8_t *, const int8_t *, float *, const uint32_t, const uint32_t, const uint32_t);
        using BDeqKernel = void (*)(const int8_t *, const int8_t *, const int32_t *, float *, const uint32_t, const uint32_t, const uint32_t);

        const DeqKernel kDeq[15] = {
            tlib_linear_deq_impl_shift00_esp32s3, tlib_linear_deq_impl_shift01_esp32s3,
            tlib_linear_deq_impl_shift02_esp32s3, tlib_linear_deq_impl_shift03_esp32s3,
            tlib_linear_deq_impl_shift04_esp32s3, tlib_linear_deq_impl_shift05_esp32s3,
            tlib_linear_deq_impl_shift06_esp32s3, tlib_linear_deq_impl_shift07_esp32s3,
            tlib_linear_deq_impl_shift08_esp32s3, tlib_linear_deq_impl_shift09_esp32s3,
            tlib_linear_deq_impl_shift10_esp32s3, tlib_linear_deq_impl_shift11_esp32s3,
            tlib_linear_deq_impl_shift12_esp32s3, tlib_linear_deq_impl_shift13_esp32s3,
            tlib_linear_deq_impl_shift14_esp32s3,
        };

        const BDeqKernel kBDeq[15] = {
            tlib_linear_b_deq_impl_shift00_esp32s3, tlib_linear_b_deq_impl_shift01_esp32s3,
            tlib_linear_b_deq_impl_shift02_esp32s3, tlib_linear_b_deq_impl_shift03_esp32s3,
            tlib_linear_b_deq_impl_shift04_esp32s3, tlib_linear_b_deq_impl_shift05_esp32s3,
            tlib_linear_b_deq_impl_shift06_esp32s3, tlib_linear_b_deq_impl_shift07_esp32s3,
            tlib_linear_b_deq_impl_shift08_esp32s3, tlib_linear_b_deq_impl_shift09_esp32s3,
            tlib_linear_b_deq_impl_shift10_esp32s3, tlib_linear_b_deq_impl_shift11_esp32s3,
            tlib_linear_b_deq_impl_shift12_esp32s3, tlib_linear_b_deq_impl_shift13_esp32s3,
            tlib_linear_b_deq_impl_shift14_esp32s3,
        };
    } // namespace

    void linear_b_relu_impl_lshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t lshift, const int32_t b_stream)
    {
        assert(k % 16 == 0);
        const uint32_t k16 = k / 16;
        Tiled<int8_t>(b, b_stream, k, c, y, n, m, [&](const uint8_t *bt, const int32_t *ct, int8_t *yt, const uint32_t mt) {
            tlib_linear_b_relu_impl_lshift_esp32s3(a, reinterpret_cast<const int8_t *>(bt), ct, yt, k16, n, mt, lshift);
        });
    }

    void linear_b_relu_impl_rshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t rshift, const int32_t b_stream)
    {
        assert(k % 16 == 0);
        const uint32_t k16 = k / 16;
        Tiled<int8_t>(b, b_stream, k, c, y, n, m, [&](const uint8_t *bt, const int32_t *ct, int8_t *yt, const uint32_t mt) {
            tlib_linear_b_relu_impl_rshift_esp32s3(a, reinterpret_cast<const int8_t *>(bt), ct, yt, k16, n, mt, rshift);
        });
    }

    void linear_relu_impl_lshift(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t lshift, const int32_t b_stream)
    {
        assert(k % 16 == 0);
        const uint32_t k16 = k / 16;
        Tiled<int8_t>(b, b_stream, k, nullptr, y, n, m, [&](const uint8_t *bt, const int32_t *, int8_t *yt, const uint32_t mt) {
            tlib_linear_relu_impl_lshift_esp32s3(a, reinterpret_cast<const int8_t *>(bt), yt, k16, n, mt, lshift);
        });
    }

    void linear_relu_impl_rshift(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t rshift, const int32_t b_stream)
    {
        assert(k % 16 == 0);
        const uint32_t k16 = k / 16;
        Tiled<int8_t>(b, b_stream, k, nullptr, y, n, m, [&](const uint8_t *bt, const int32_t *, int8_t *yt, const uint32_t mt) {
            tlib_linear_relu_impl_rshift_esp32s3(a, reinterpret_cast<const int8_t *>(bt), yt, k16, n, mt, rshift);
        });
    }

    void linear_deq_impl(const int8_t *a, const int8_t *b, float *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t shift, const int32_t b_stream)
    {
        assert(shift < 15);
        assert(k % 16 == 0);
        const uint32_t k16 = k / 16;
        const DeqKernel kernel = kDeq[shift];
        Tiled<float>(b, b_stream, k, nullptr, y, n, m, [&](const uint8_t *bt, const int32_t *, float *yt, const uint32_t mt) {
            kernel(a, reinterpret_cast<const int8_t *>(bt), yt, k16, n, mt);
        });
    }

    void linear_b_deq_impl(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t shift, const int32_t b_stream)
    {
        assert(shift < 15);
        assert(k % 16 == 0);
        const uint32_t k16 = k / 16;
        const BDeqKernel kernel = kBDeq[shift];
        Tiled<float>(b, b_stream, k, c, y, n, m, [&](const uint8_t *bt, const int32_t *ct, float *yt, const uint32_t mt) {
            kernel(a, reinterpret_cast<const int8_t *>(bt), ct, yt, k16, n, mt);
        });
    }

    void linear_impl(const float *a, const float *b, float *y, const uint32_t k, const uint32_t n, const uint32_t m, const int32_t b_stream)
    {
        assert(k % 4 == 0);
        const uint32_t k4 = k / 4;
        Tiled<float>(b, b_stream, k * sizeof(float), nullptr, y, n, m, [&](const uint8_t *bt, const int32_t *, float *yt, const uint32_t mt) {
            tlib_linear_impl_esp32s3(a, reinterpret_cast<const float *>(bt), yt, k4, n, mt);
        });
    }

} // tlib::ops::impl

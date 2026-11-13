// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib_ops/
// inc/tlib_linear_impl.h (Apache-2.0, Copyright 2026 lspr98): every kernel
// takes b_stream, the model-file id of a B that stays on SD (-1: b is in
// memory); linear_b_relu_deq_impl (no S3 kernel upstream) is gone.
//
// y[n x m] = a[n x k] . b[m x k]^T (+ c[m]), row-major. The kernels re-read
// all of b once per row of a, so b goes through the internal-SRAM tile
// (port.h stt_tile) in blocks of whole rows (tlib_linear_impl_esp32s3.cpp).

#ifndef TLIB_LINEAR_IMPL_H_
#define TLIB_LINEAR_IMPL_H_

#include <cstdint>

namespace tlib::ops::impl {

void linear_b_relu_impl_lshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t lshift, const int32_t b_stream = -1);
void linear_b_relu_impl_rshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t rshift, const int32_t b_stream = -1);
void linear_relu_impl_lshift(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t lshift, const int32_t b_stream = -1);
void linear_relu_impl_rshift(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t rshift, const int32_t b_stream = -1);
void linear_b_deq_impl(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t shift, const int32_t b_stream = -1);
void linear_deq_impl(const int8_t *a, const int8_t *b, float *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t shift, const int32_t b_stream = -1);
void linear_impl(const float *a, const float *b, float *y, const uint32_t k, const uint32_t n, const uint32_t m, const int32_t b_stream = -1);

} // tlib::ops::impl

#endif

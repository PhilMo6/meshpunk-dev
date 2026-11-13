// tlib_kernels_c.h — portable C++ versions of upstream conformer-stt-s3's
// ESP32-S3 kernels (tlib_ops/kernels/*_esp32s3.s, Apache-2.0, Copyright
// 2026 lspr98), written to give the same results bit for bit;
// test/kcheck.cpp runs each against its S3 kernel on an ESP32-S3.
//
// a is [n x k], b is [m x k], y is [n x m], row-major, y's rows ldy
// elements apart (ldy >= m; the S3 kernels write them m apart); k16 / k4 =
// the row length in 16-byte / 4-float blocks, as the S3 kernels take it
// (the int8 kernels need k16 > 0).
//   int8 kernels: s = the int32 sum over i < k of a[r][i] * b[j][i] (the S3
//     reads its 40-bit accumulator back as the low 32 bits; int8 rows of
//     the model's lengths never reach 2^31; the P4 build, STT_KERNELS_P4,
//     takes the sums from its 128-bit multiply-accumulate,
//     kernels_p4/tlib_dots_s8_esp32p4.S, test/kcheck_p4.cpp checking them
//     against the C sums on a P4), then
//     _deq   (s [+ c[j]]) as float, times 2^-shift          FLOAT.S
//     _relu  (s [+ c[j]]) shifted right (arithmetic) or left (wrapping),
//            no rounding, clamped to [-128, 127], then max 0  SRA/SLL,
//                                                           CLAMPS 7, MAX
//   f32:     four lane sums (lane = column mod 4), each a chain of fused
//            multiply-adds, added as ((lane3 + lane2) + lane1) + lane0
//                                                           MADD.S, ADD.S
//   dw_deq:  one depthwise-convolution channel: y[t] = (the int32 sum of
//            w against x row t, plus bias) as float, times 2^-shift, for
//            t < n (x [n x k]); linear_b_deq's value for x row t against w
//            with bias, all n from one row of sums
//   relpos:  the attention's position scores: y [n x n], y[r][c] = the f32
//            sum of a row r and b row n - 1 - r + c (a [n x k], b
//            [2n - 1 x k]), which is what the f32 kernel's [n x 2n - 1]
//            product has there after conformer_attention.cpp's relative
//            shift and slice
//   quantize: x times 2^shift rounded to the nearest integer, ties to
//            even, clamped to [-128, 127]; NaN gives 127      ROUND.S,
//            (on RISC-V by FCVT.W.S)                        CLAMPS 7

#ifndef TLIB_KERNELS_C_H_
#define TLIB_KERNELS_C_H_

#include <cstdint>

namespace tlib::ops::kernels_c {

void linear_b_relu_lshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, uint32_t k16, uint32_t n, uint32_t m, uint32_t ldy, uint8_t lshift);
void linear_b_relu_rshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, uint32_t k16, uint32_t n, uint32_t m, uint32_t ldy, uint8_t rshift);
void linear_relu_lshift(const int8_t *a, const int8_t *b, int8_t *y, uint32_t k16, uint32_t n, uint32_t m, uint32_t ldy, uint8_t lshift);
void linear_relu_rshift(const int8_t *a, const int8_t *b, int8_t *y, uint32_t k16, uint32_t n, uint32_t m, uint32_t ldy, uint8_t rshift);
void linear_deq(const int8_t *a, const int8_t *b, float *y, uint32_t k16, uint32_t n, uint32_t m, uint32_t ldy, uint8_t shift);
void linear_b_deq(const int8_t *a, const int8_t *b, const int32_t *c, float *y, uint32_t k16, uint32_t n, uint32_t m, uint32_t ldy, uint8_t shift);
void dw_deq(const int8_t *w, const int8_t *x, int32_t bias, float *y, uint32_t k16, uint32_t n, uint8_t shift);
void linear_f32(const float *a, const float *b, float *y, uint32_t k4, uint32_t n, uint32_t m, uint32_t ldy);
void relpos_f32(const float *a, const float *b, float *y, uint32_t k4, uint32_t n);
void quantize(const float *a, int8_t *a_q, uint32_t k4, uint8_t shift);

} // tlib::ops::kernels_c

#endif

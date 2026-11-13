// tlib_kernels_c.cpp — see tlib_kernels_c.h.

#include "tlib_kernels_c.h"

#include <cstddef>

#include <profile.h>

#ifdef STT_KERNELS_P4
extern "C" void tlib_dots_s8_esp32p4(const int8_t *a, const int8_t *b, int32_t *out, uint32_t k16, uint32_t m);
#endif

namespace tlib::ops::kernels_c
{
    namespace
    {
        // The int32 sum of a[i] * b[i] over k values (k a positive multiple
        // of 16). An integer sum is the same in any order: four partial
        // sums let the loads and multiplies overlap. A do-while: with a
        // zero-trip path, GCC 8.4 for the ESP32-S3 stops with an internal
        // error under -fPIC (a float zero from the literal pool:
        // movsf_internal, "insn does not satisfy its constraints").
        inline int32_t Dot(const int8_t *a, const int8_t *b, const uint32_t k)
        {
            int32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            uint32_t i = 0;
            do
            {
                s0 += static_cast<int32_t>(a[i + 0]) * b[i + 0];
                s1 += static_cast<int32_t>(a[i + 1]) * b[i + 1];
                s2 += static_cast<int32_t>(a[i + 2]) * b[i + 2];
                s3 += static_cast<int32_t>(a[i + 3]) * b[i + 3];
                i += 4;
            } while (i < k);
            return (s0 + s1) + (s2 + s3);
        }

        // Columns per RowDots call (the kernels walk wider rows in blocks).
        constexpr uint32_t kSumBlock = 256;
        int32_t g_sums[kSumBlock];

        // sums[j] = the sum of row a against row j of b (rows of 16 * k16),
        // for j < m <= kSumBlock: on the P4 the 128-bit multiply-accumulate
        // (kernels_p4/tlib_dots_s8_esp32p4.S), elsewhere Dot.
        inline void RowDots(const int8_t *a, const int8_t *b, int32_t *sums, const uint32_t k16, const uint32_t m)
        {
            STT_PROF(kSums);
#ifdef STT_KERNELS_P4
            tlib_dots_s8_esp32p4(a, b, sums, k16, m);
#else
            const uint32_t k = k16 * 16;
            for (uint32_t j = 0; j < m; j++)
            {
                sums[j] = Dot(a, b + static_cast<size_t>(j) * k, k);
            }
#endif
        }

        // For each row r of a and each column j: y[r * ldy + j] = post(sum,
        // j), the sums kSumBlock columns at a time. y is written only
        // there (__restrict: an int8 store does not make the compiler
        // reload what post uses).
        template <typename T_out, typename Post>
        inline void Int8Rows(const int8_t *a, const int8_t *b, T_out *__restrict y, const uint32_t k16, const uint32_t n,
                             const uint32_t m, const uint32_t ldy, Post &&post)
        {
            STT_PROF(kInt8Post);
            const uint32_t k = k16 * 16;
            for (uint32_t r = 0; r < n; r++)
            {
                const int8_t *ar = a + static_cast<size_t>(r) * k;
                T_out *__restrict yr = y + static_cast<size_t>(r) * ldy;
                for (uint32_t j0 = 0; j0 < m; j0 += kSumBlock)
                {
                    const uint32_t mt = (m - j0 < kSumBlock) ? (m - j0) : kSumBlock;
                    RowDots(ar, b + static_cast<size_t>(j0) * k, g_sums, k16, mt);
                    // Four outputs at a time, all four computed before any
                    // is stored: independent, so the in-order core
                    // overlaps their load, convert and multiply latencies.
                    uint32_t j = 0;
                    for (; j + 4 <= mt; j += 4)
                    {
                        const T_out v0 = post(g_sums[j + 0], j0 + j + 0);
                        const T_out v1 = post(g_sums[j + 1], j0 + j + 1);
                        const T_out v2 = post(g_sums[j + 2], j0 + j + 2);
                        const T_out v3 = post(g_sums[j + 3], j0 + j + 3);
                        yr[j0 + j + 0] = v0;
                        yr[j0 + j + 1] = v1;
                        yr[j0 + j + 2] = v2;
                        yr[j0 + j + 3] = v3;
                    }
                    for (; j < mt; j++)
                    {
                        yr[j0 + j] = post(g_sums[j], j0 + j);
                    }
                }
            }
        }

        // ADD.N: a 32-bit add that wraps.
        inline int32_t AddWrap(const int32_t s, const int32_t c)
        {
            return static_cast<int32_t>(static_cast<uint32_t>(s) + static_cast<uint32_t>(c));
        }

        // SRA / SLL by the low five bits of the shift (SSR / SSL take those).
        inline int32_t ShiftRight(const int32_t v, const uint8_t rshift)
        {
            return v >> (rshift & 31u);
        }

        inline int32_t ShiftLeft(const int32_t v, const uint8_t lshift)
        {
            return static_cast<int32_t>(static_cast<uint32_t>(v) << (lshift & 31u));
        }

        // CLAMPS 7 then MAX 0.
        inline int8_t ClampRelu(const int32_t v)
        {
            if (v > 127)
            {
                return 127;
            }
            if (v < 0)
            {
                return 0;
            }
            return static_cast<int8_t>(v);
        }

        // FLOAT.S's scale: 2^-shift, exact.
        inline float InvPow2(const uint8_t shift)
        {
            return 1.0f / static_cast<float>(1u << shift);
        }

        // ROUND.S then CLAMPS 7, for v = x * 2^shift (exact).
#if defined(__riscv)
        // FCVT.W.S rounding to nearest, ties to even (NaN gives 2^31 - 1,
        // magnitudes past the int32 range saturate), then the clamp: the
        // same result as the portable form below for every float.
        inline int8_t RoundClamp(const float v)
        {
            int32_t r;
            asm("fcvt.w.s %0, %1, rne" : "=r"(r) : "f"(v));
            return static_cast<int8_t>(r > 127 ? 127 : (r < -128 ? -128 : r));
        }
#else
        // Below 2^22 in magnitude, adding and taking away 1.5 * 2^23 rounds
        // v to the nearest integer, ties to even (the sum's ulp is 1).
        inline int8_t RoundClamp(const float v)
        {
            if (v != v)
            {
                return 127;
            }
            if (v >= 127.5f)
            {
                return 127;
            }
            if (v <= -128.5f)
            {
                return -128;
            }
            const float r = (v + 12582912.0f) - 12582912.0f;
            return static_cast<int8_t>(static_cast<int32_t>(r));
        }
#endif
    } // namespace

    void linear_b_relu_lshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint32_t ldy, const uint8_t lshift)
    {
        Int8Rows(a, b, y, k16, n, m, ldy, [c, lshift](const int32_t s, const uint32_t j) {
            return ClampRelu(ShiftLeft(AddWrap(s, c[j]), lshift));
        });
    }

    void linear_b_relu_rshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint32_t ldy, const uint8_t rshift)
    {
        Int8Rows(a, b, y, k16, n, m, ldy, [c, rshift](const int32_t s, const uint32_t j) {
            return ClampRelu(ShiftRight(AddWrap(s, c[j]), rshift));
        });
    }

    void linear_relu_lshift(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint32_t ldy, const uint8_t lshift)
    {
        Int8Rows(a, b, y, k16, n, m, ldy, [lshift](const int32_t s, const uint32_t) {
            return ClampRelu(ShiftLeft(s, lshift));
        });
    }

    void linear_relu_rshift(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint32_t ldy, const uint8_t rshift)
    {
        Int8Rows(a, b, y, k16, n, m, ldy, [rshift](const int32_t s, const uint32_t) {
            return ClampRelu(ShiftRight(s, rshift));
        });
    }

    void linear_deq(const int8_t *a, const int8_t *b, float *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint32_t ldy, const uint8_t shift)
    {
        const float scale = InvPow2(shift);
        Int8Rows(a, b, y, k16, n, m, ldy, [scale](const int32_t s, const uint32_t) {
            return static_cast<float>(s) * scale;
        });
    }

    void linear_b_deq(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint32_t ldy, const uint8_t shift)
    {
        const float scale = InvPow2(shift);
        Int8Rows(a, b, y, k16, n, m, ldy, [c, scale](const int32_t s, const uint32_t j) {
            return static_cast<float>(AddWrap(s, c[j])) * scale;
        });
    }

    void dw_deq(const int8_t *w, const int8_t *x, const int32_t bias, float *y, const uint32_t k16, const uint32_t n, const uint8_t shift)
    {
        const float scale = InvPow2(shift);
        Int8Rows(w, x, y, k16, 1, n, n, [bias, scale](const int32_t s, const uint32_t) {
            return static_cast<float>(AddWrap(s, bias)) * scale;
        });
    }

    namespace
    {
        // y[r][c] for the R rows at a and the C columns at b (rows of k,
        // y rows ldy apart): each of the R * C sums is the f32 kernel's own
        // (four lanes of fused multiply-adds in order of i, then
        // ((lane3 + lane2) + lane1) + lane0); computing them together loads
        // each value of a and b once per block instead of once per sum.
        template <uint32_t R, uint32_t C>
        inline void F32Block(const float *a, const float *b, float *y, const uint32_t k, const uint32_t ldy)
        {
            float s[R][C][4] = {};
            for (uint32_t i = 0; i < k; i += 4)
            {
#pragma GCC unroll 4
                for (uint32_t l = 0; l < 4; l++)
                {
                    float x[R], w[C];
#pragma GCC unroll 2
                    for (uint32_t r = 0; r < R; r++)
                    {
                        x[r] = a[static_cast<size_t>(r) * k + i + l];
                    }
#pragma GCC unroll 2
                    for (uint32_t c = 0; c < C; c++)
                    {
                        w[c] = b[static_cast<size_t>(c) * k + i + l];
                    }
#pragma GCC unroll 2
                    for (uint32_t r = 0; r < R; r++)
                    {
#pragma GCC unroll 2
                        for (uint32_t c = 0; c < C; c++)
                        {
                            s[r][c][l] = __builtin_fmaf(x[r], w[c], s[r][c][l]);
                        }
                    }
                }
            }
#pragma GCC unroll 2
            for (uint32_t r = 0; r < R; r++)
            {
#pragma GCC unroll 2
                for (uint32_t c = 0; c < C; c++)
                {
                    y[static_cast<size_t>(r) * ldy + c] = ((s[r][c][3] + s[r][c][2]) + s[r][c][1]) + s[r][c][0];
                }
            }
        }

        // R rows of a against all m columns of b.
        template <uint32_t R>
        inline void F32Rows(const float *a, const float *b, float *y, const uint32_t k, const uint32_t m, const uint32_t ldy)
        {
            uint32_t j = 0;
            for (; j + 2 <= m; j += 2)
            {
                F32Block<R, 2>(a, b + static_cast<size_t>(j) * k, y + j, k, ldy);
            }
            if (j < m)
            {
                F32Block<R, 1>(a, b + static_cast<size_t>(j) * k, y + j, k, ldy);
            }
        }
    } // namespace

    void linear_f32(const float *a, const float *b, float *y, const uint32_t k4, const uint32_t n, const uint32_t m, const uint32_t ldy)
    {
        STT_PROF(kF32);
        const uint32_t k = k4 * 4;
        uint32_t r = 0;
        for (; r + 2 <= n; r += 2)
        {
            F32Rows<2>(a + static_cast<size_t>(r) * k, b, y + static_cast<size_t>(r) * ldy, k, m, ldy);
        }
        if (r < n)
        {
            F32Rows<1>(a + static_cast<size_t>(r) * k, b, y + static_cast<size_t>(r) * ldy, k, m, ldy);
        }
    }

    void relpos_f32(const float *a, const float *b, float *y, const uint32_t k4, const uint32_t n)
    {
        STT_PROF(kF32);
        const uint32_t k = k4 * 4;
        uint32_t r = 0;
        // Rows r and r + 1 together: row r's column c is b row j0 + c
        // (j0 = n - 1 - r), row r + 1's column c is b row j0 - 1 + c, so a
        // block of b rows j, j + 1 lands at row r columns j - j0 + (0, 1)
        // and row r + 1 columns j - j0 + (1, 2): y rows n + 1 apart.
        for (; r + 2 <= n; r += 2)
        {
            const float *ar = a + static_cast<size_t>(r) * k;
            float *yr = y + static_cast<size_t>(r) * n;
            const uint32_t j0 = n - 1 - r;
            F32Block<1, 1>(ar + k, b + static_cast<size_t>(j0 - 1) * k, yr + n, k, n);   // row r + 1, column 0
            const uint32_t j_end = j0 + n - 1;
            uint32_t j = j0;
            for (; j + 2 <= j_end; j += 2)
            {
                F32Block<2, 2>(ar, b + static_cast<size_t>(j) * k, yr + (j - j0), k, n + 1);
            }
            if (j < j_end)
            {
                F32Block<2, 1>(ar, b + static_cast<size_t>(j) * k, yr + (j - j0), k, n + 1);
            }
            F32Block<1, 1>(ar, b + static_cast<size_t>(j_end) * k, yr + n - 1, k, n);    // row r, column n - 1
        }
        if (r < n)
        {
            F32Rows<1>(a + static_cast<size_t>(r) * k, b + static_cast<size_t>(n - 1 - r) * k,
                       y + static_cast<size_t>(r) * n, k, n, n);
        }
    }

    void quantize(const float *a, int8_t *a_q, const uint32_t k4, const uint8_t shift)
    {
        STT_PROF(kQuantize);
        const float scale = static_cast<float>(1u << shift);
        const uint32_t n = k4 * 4;
#pragma GCC unroll 4
        for (uint32_t i = 0; i < n; i++)
        {
            a_q[i] = RoundClamp(a[i] * scale);
        }
    }

} // tlib::ops::kernels_c

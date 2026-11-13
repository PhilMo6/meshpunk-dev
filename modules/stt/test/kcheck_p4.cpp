// kcheck_p4.cpp — the P4 kernel check (build.ps1 -Variant kcheck_p4): an
// audio plugin for the ESP32-P4 that runs the P4's int8 sums
// (src/tlib_ops/kernels_p4/tlib_dots_s8_esp32p4.S, block loop on a branch),
// the same sums with the block loop rearranged (test/kcheck_p4_loops.S) and
// on the zero-overhead loop (test/kcheck_p4_hwloop.S) against plain C sums.
// Command "kcheck": T: lines, then D.
//   sums    every row length / width / extreme regime, and the output past
//           the last column untouched;
//   stress  the engine's placement (b in an internal block, as the tile;
//           a in PSRAM) and widths (k 176 x 93 columns, k 704 x 23: a full
//           16 KB tile), each loop run for 4 s against sums computed once,
//           with a 1 ms sleep every 100 ms (the idle task's turn on core 1:
//           the task watchdog) and the P4's other tasks preempting it;
//   time    each loop and C at three engine shapes, CPU cycles per call;
//   f32     tlib_kernels_c.cpp's float kernel (rows and columns in 2 x 2
//           blocks) against the one-sum-at-a-time form it replaced, bit for
//           bit, at the attention's shapes and odd ones, and both timed;
//   relpos  tlib_kernels_c.cpp's position scores against the float
//           kernel's full product read as the relative shift keeps it;
//   quantize tlib_kernels_c.cpp's quantize step (FCVT.W.S on RISC-V)
//           against its portable rounding, edge values and random floats.
// Every buffer the vector loads read is in a granted block (16-byte
// aligned); the module's own data is not (the loader places it 8-aligned).

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../src/audio/audio_plugin_abi.h"

#include <tlib_kernels_c.h>

extern "C" void tlib_dots_s8_esp32p4(const int8_t *a, const int8_t *b, int32_t *out, uint32_t k16, uint32_t m);
extern "C" void kc_dots_rot(const int8_t *a, const int8_t *b, int32_t *out, uint32_t k16, uint32_t m);
extern "C" void kc_dots_u2(const int8_t *a, const int8_t *b, int32_t *out, uint32_t k16, uint32_t m);
extern "C" void kc_dots_u4(const int8_t *a, const int8_t *b, int32_t *out, uint32_t k16, uint32_t m);
extern "C" void kc_dots_p2(const int8_t *a, const int8_t *b, int32_t *out, uint32_t k16, uint32_t m);
extern "C" void kc_dots_s8_hwloop(const int8_t *a, const int8_t *b, int32_t *out, uint32_t k16, uint32_t m);

namespace
{
    using DotsFn = void (*)(const int8_t *, const int8_t *, int32_t *, uint32_t, uint32_t);

    struct Loop
    {
        const char *name;
        DotsFn fn;
    };

    const Loop kLoops[] = {
        { "kernel", tlib_dots_s8_esp32p4 },
        { "rotated", kc_dots_rot },
        { "unrolled x2", kc_dots_u2 },
        { "unrolled x4", kc_dots_u4 },
        { "two ahead x4", kc_dots_p2 },
        { "hardware loop", kc_dots_s8_hwloop },
    };
    constexpr uint32_t kNumLoops = sizeof(kLoops) / sizeof(kLoops[0]);

    constexpr uint32_t kMaxK16 = 64;
    constexpr uint32_t kMaxM = 96;
    constexpr int32_t kSentinel = 0x5A5A5A5A;

    // Block 0 (internal): the tile, b of stress and time. Block 1 (PSRAM):
    // a of stress and time, then a and b of sums.
    constexpr uint32_t kTileBytes = 16 * 1024;
    constexpr uint32_t kStressABytes = 2 * 1024;
    constexpr uint32_t kSumsABytes = kMaxK16 * 16 + 16;
    constexpr uint32_t kSumsBBytes = kMaxM * kMaxK16 * 16;
    // Block 2 (PSRAM): the f32 and relpos checks' floats at their largest
    // shape (k 44, n 88, 175 = 2n - 1 positions): relpos's q, p, full and
    // shifted products and y (f32's a, b, y and y_ref fit in it).
    constexpr uint32_t kF32Bytes = 4 * (44 * 88 + 44 * 175 + 2 * 88 * 175 + 88 * 88);

    const AudioPlugHostApi *g_api = nullptr;
    int8_t *g_tile = nullptr;
    int8_t *g_sa = nullptr;   // stress / time a
    int8_t *g_ca = nullptr;   // sums a
    int8_t *g_cb = nullptr;   // sums b
    float *g_f32 = nullptr;
    int32_t g_out[kMaxM + 4];
    int32_t g_ref[kMaxM];

    inline uint32_t Cycles(void)
    {
        uint32_t c;
        asm volatile("rdcycle %0" : "=r"(c));
        return c;
    }

    void Publish(const char *s)
    {
        const uint32_t len = static_cast<uint32_t>(strlen(s));
        for (int tries = 0; tries < 500 && !g_api->publish(s, len); tries++)
        {
            g_api->delay_ms(20);
        }
    }

    void Say(const char *fmt, ...)
    {
        char line[240];
        line[0] = 'T';
        line[1] = ':';
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line + 2, sizeof(line) - 2, fmt, ap);
        va_end(ap);
        Publish(line);
    }

    uint32_t g_seed = 0x9E3779B9u;

    uint32_t Rand(void)
    {
        uint32_t x = g_seed;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        g_seed = x;
        return x;
    }

    int32_t RefDot(const int8_t *a, const int8_t *b, const uint32_t k)
    {
        int32_t s = 0;
        for (uint32_t i = 0; i < k; i++)
        {
            s += static_cast<int32_t>(a[i]) * b[i];
        }
        return s;
    }

    int8_t RandInt8(const int regime, const uint32_t i, const bool is_b)
    {
        const uint32_t r = Rand();
        int8_t v = static_cast<int8_t>(r >> 8);
        if (regime == 0 && (r & 15) == 0) v = -128;
        if (regime == 0 && (r & 15) == 1) v = 127;
        if (regime == 1) v = -128;
        if (regime == 2) v = 127;
        if (regime == 3) v = is_b ? ((i & 1) ? -128 : 127) : ((i & 1) ? 127 : -127);
        return v;
    }

    // regime 0: random with -128 / 127 often; 1: every value -128; 2: every
    // value 127; 3: alternating signs.
    void Fill(int8_t *a, int8_t *b, const int regime, const uint32_t k, const uint32_t m)
    {
        for (uint32_t i = 0; i < k + 16; i++)
        {
            a[i] = RandInt8(regime, i, false);
        }
        for (uint32_t i = 0; i < m * k; i++)
        {
            b[i] = RandInt8(regime, i, true);
        }
    }

    void CheckSums(const Loop &loop)
    {
        const uint32_t k16s[] = { 1, 2, 3, 4, 5, 11, 16, 44, 64 };
        const uint32_t ms[] = { 0, 1, 7, 33, 64 };
        uint32_t values = 0, differ = 0, clobbered = 0;
        char first[160] = { 0 };
        for (const uint32_t k16 : k16s)
        {
            for (const uint32_t m : ms)
            {
                for (int regime = 0; regime < 4; regime++)
                {
                    const uint32_t k = k16 * 16;
                    Fill(g_ca, g_cb, regime, k, m);
                    for (uint32_t j = 0; j < kMaxM + 4; j++)
                    {
                        g_out[j] = kSentinel;
                    }
                    loop.fn(g_ca, g_cb, g_out, k16, m);
                    for (uint32_t j = 0; j < m; j++)
                    {
                        const int32_t want = RefDot(g_ca, g_cb + static_cast<size_t>(j) * k, k);
                        values++;
                        if (g_out[j] != want)
                        {
                            if (differ == 0)
                            {
                                snprintf(first, sizeof(first), "k16 %u m %u regime %d col %u: p4 %d, c %d",
                                         static_cast<unsigned>(k16), static_cast<unsigned>(m), regime,
                                         static_cast<unsigned>(j), static_cast<int>(g_out[j]), static_cast<int>(want));
                            }
                            differ++;
                        }
                    }
                    for (uint32_t j = m; j < kMaxM + 4; j++)
                    {
                        if (g_out[j] != kSentinel)
                        {
                            clobbered++;
                        }
                    }
                }
            }
        }
        if (differ == 0 && clobbered == 0)
        {
            Say("PASS sums %s: %u sums", loop.name, static_cast<unsigned>(values));
        }
        else
        {
            Say("FAIL sums %s: %u of %u differ, %u words written past the end; first: %s", loop.name,
                static_cast<unsigned>(differ), static_cast<unsigned>(values), static_cast<unsigned>(clobbered), first);
        }
    }

    // b in the internal block, a in PSRAM; the loop runs `ms` milliseconds
    // against sums computed once.
    void Stress(const Loop &loop, const uint32_t ms)
    {
        const uint32_t shapes[2][2] = { { 11, 93 }, { 44, 23 } };   // k16, m (16 KB of b)
        uint32_t values = 0, differ = 0, rounds = 0;
        char first[160] = { 0 };
        for (const auto &sh : shapes)
        {
            const uint32_t k16 = sh[0], m = sh[1], k = k16 * 16;
            Fill(g_sa, g_tile, 0, k, m);
            for (uint32_t j = 0; j < m; j++)
            {
                g_ref[j] = RefDot(g_sa, g_tile + static_cast<size_t>(j) * k, k);
            }
            const uint32_t t0 = g_api->ticks_ms();
            uint32_t slept = t0;
            while (g_api->ticks_ms() - t0 < ms / 2)
            {
                if (g_api->ticks_ms() - slept >= 100)
                {
                    g_api->delay_ms(1);
                    slept = g_api->ticks_ms();
                }
                loop.fn(g_sa, g_tile, g_out, k16, m);
                rounds++;
                for (uint32_t j = 0; j < m; j++)
                {
                    values++;
                    if (g_out[j] != g_ref[j])
                    {
                        if (differ == 0)
                        {
                            snprintf(first, sizeof(first), "k16 %u round %u col %u: p4 %d, c %d (diff %d)",
                                     static_cast<unsigned>(k16), static_cast<unsigned>(rounds), static_cast<unsigned>(j),
                                     static_cast<int>(g_out[j]), static_cast<int>(g_ref[j]),
                                     static_cast<int>(g_out[j] - g_ref[j]));
                        }
                        differ++;
                    }
                }
            }
        }
        if (differ == 0)
        {
            Say("PASS stress %s: %u sums in %u calls", loop.name, static_cast<unsigned>(values), static_cast<unsigned>(rounds));
        }
        else
        {
            Say("FAIL stress %s: %u of %u sums differ in %u calls; first: %s", loop.name, static_cast<unsigned>(differ),
                static_cast<unsigned>(values), static_cast<unsigned>(rounds), first);
        }
    }

    void CRows(const int8_t *a, const int8_t *b, int32_t *out, const uint32_t k16, const uint32_t m)
    {
        const uint32_t k = k16 * 16;
        for (uint32_t j = 0; j < m; j++)
        {
            out[j] = RefDot(a, b + static_cast<size_t>(j) * k, k);
        }
    }

    // CPU cycles per call (best of `reps` calls), b in the internal block,
    // a in PSRAM, at the engine's shapes: k 176 x 93 columns (attention and
    // convolution layers), k 704 x 23 (feed-forward down), k 32 x 1 (the
    // depthwise convolution: one column per call).
    void TimeSums(void)
    {
        const uint32_t shapes[3][2] = { { 11, 93 }, { 44, 23 }, { 2, 1 } };
        for (const auto &sh : shapes)
        {
            const uint32_t k16 = sh[0], m = sh[1];
            Fill(g_sa, g_tile, 0, k16 * 16, m);
            char line[200];
            int at = snprintf(line, sizeof(line), "time k %u x %u (cycles per call, %u multiply-adds):",
                              static_cast<unsigned>(k16 * 16), static_cast<unsigned>(m), static_cast<unsigned>(k16 * 16 * m));
            for (uint32_t v = 0; v <= kNumLoops; v++)
            {
                const DotsFn fn = v < kNumLoops ? kLoops[v].fn : CRows;
                const char *name = v < kNumLoops ? kLoops[v].name : "C";
                uint32_t best = 0xFFFFFFFFu;
                const uint32_t reps = (m == 1) ? 2000 : 200;
                for (uint32_t r = 0; r < reps; r++)
                {
                    const uint32_t c0 = Cycles();
                    fn(g_sa, g_tile, g_out, k16, m);
                    const uint32_t c = Cycles() - c0;
                    if (c < best)
                    {
                        best = c;
                    }
                }
                at += snprintf(line + at, sizeof(line) - at, " %s %u%s", name, static_cast<unsigned>(best),
                               v < kNumLoops ? "," : "");
                if (at >= static_cast<int>(sizeof(line)))
                {
                    at = sizeof(line) - 1;
                }
                g_api->delay_ms(1);
            }
            Say("%s", line);
        }
    }

    // The f32 kernel as it was: one sum at a time.
    void F32Ref(const float *a, const float *b, float *y, const uint32_t k4, const uint32_t n, const uint32_t m)
    {
        const uint32_t k = k4 * 4;
        for (uint32_t r = 0; r < n; r++)
        {
            const float *ar = a + static_cast<size_t>(r) * k;
            const float *bj = b;
            for (uint32_t j = 0; j < m; j++, bj += k)
            {
                float l0 = 0.0f, l1 = 0.0f, l2 = 0.0f, l3 = 0.0f;
                for (uint32_t i = 0; i < k; i += 4)
                {
                    l3 = __builtin_fmaf(ar[i + 3], bj[i + 3], l3);
                    l2 = __builtin_fmaf(ar[i + 2], bj[i + 2], l2);
                    l1 = __builtin_fmaf(ar[i + 1], bj[i + 1], l1);
                    l0 = __builtin_fmaf(ar[i + 0], bj[i + 0], l0);
                }
                *y++ = ((l3 + l2) + l1) + l0;
            }
        }
    }

    float RandFloat(void)
    {
        // a sign, a 23-bit fraction and exponents 2^-8 .. 2^7
        const uint32_t r = Rand();
        uint32_t bits = (r & 0x807FFFFFu) | ((119u + (Rand() & 15u)) << 23);
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

    // In block 2: a [n x k], b [m x k], y and y_ref [n x m].
    void CheckF32(void)
    {
        const uint32_t shapes[][3] = { { 44, 88, 88 }, { 44, 88, 175 }, { 44, 64, 127 }, { 4, 1, 1 },
                                       { 8, 3, 5 }, { 44, 1, 7 }, { 12, 7, 2 }, { 0, 3, 3 } };
        float *fa = g_f32;
        uint32_t values = 0, differ = 0;
        char first[160] = { 0 };
        for (const auto &sh : shapes)
        {
            const uint32_t k = sh[0], n = sh[1], m = sh[2];
            float *fb = fa + n * k;
            float *fy = fb + m * k;
            float *fr = fy + n * m;
            for (uint32_t i = 0; i < n * k; i++) fa[i] = RandFloat();
            for (uint32_t i = 0; i < m * k; i++) fb[i] = RandFloat();
            tlib::ops::kernels_c::linear_f32(fa, fb, fy, k / 4, n, m, m);
            F32Ref(fa, fb, fr, k / 4, n, m);
            for (uint32_t i = 0; i < n * m; i++)
            {
                values++;
                if (memcmp(&fy[i], &fr[i], sizeof(float)) != 0)
                {
                    if (differ == 0)
                    {
                        uint32_t got, want;
                        memcpy(&got, &fy[i], 4);
                        memcpy(&want, &fr[i], 4);
                        snprintf(first, sizeof(first), "k %u n %u m %u at %u: %08X, was %08X", static_cast<unsigned>(k),
                                 static_cast<unsigned>(n), static_cast<unsigned>(m), static_cast<unsigned>(i),
                                 static_cast<unsigned>(got), static_cast<unsigned>(want));
                    }
                    differ++;
                }
            }
        }
        if (differ == 0)
        {
            Say("PASS f32 blocks: %u values bit for bit", static_cast<unsigned>(values));
        }
        else
        {
            Say("FAIL f32 blocks: %u of %u values differ; first: %s", static_cast<unsigned>(differ),
                static_cast<unsigned>(values), first);
        }
        // The attention's q.k^T shape at 88 frames, cycles per call.
        const uint32_t k = 44, n = 88, m = 88;
        float *fb = fa + n * k;
        float *fy = fb + m * k;
        uint32_t best_new = 0xFFFFFFFFu, best_old = 0xFFFFFFFFu;
        for (int rep = 0; rep < 5; rep++)
        {
            uint32_t c0 = Cycles();
            tlib::ops::kernels_c::linear_f32(fa, fb, fy, k / 4, n, m, m);
            uint32_t c = Cycles() - c0;
            if (c < best_new) best_new = c;
            c0 = Cycles();
            F32Ref(fa, fb, fy, k / 4, n, m);
            c = Cycles() - c0;
            if (c < best_old) best_old = c;
            g_api->delay_ms(1);
        }
        Say("time f32 k 44 x 88 rows x 88 columns (%u multiply-adds): blocks %u cycles, one at a time %u cycles",
            static_cast<unsigned>(k * n * m), static_cast<unsigned>(best_new), static_cast<unsigned>(best_old));
    }

    // conformer_attention.cpp's RelativePositionalShift on one page [n x m]:
    // skip n - 1 values, copy m - n + 1, then n - 1 times a 0 and m values.
    void ShiftRef(const float *x, float *y, const uint32_t n, const uint32_t m)
    {
        x += n - 1;
        memcpy(y, x, sizeof(float) * (m - n + 1));
        y += m - n + 1;
        x += m - n + 1;
        for (uint32_t i = 0; i < n - 1; i++)
        {
            *y++ = 0.0f;
            memcpy(y, x, sizeof(float) * m);
            x += m;
            y += m;
        }
    }

    // kernels_c::relpos_f32 against the f32 kernel's full [n x 2n - 1]
    // product put through ShiftRef and sliced to n columns, bit for bit;
    // in block 2.
    void CheckRelpos(void)
    {
        const uint32_t shapes[][2] = { { 44, 1 }, { 44, 2 }, { 44, 3 }, { 8, 7 }, { 44, 64 }, { 44, 72 },
                                       { 44, 88 }, { 4, 5 } };
        uint32_t values = 0, differ = 0;
        char first[120] = { 0 };
        for (const auto &sh : shapes)
        {
            const uint32_t k = sh[0], n = sh[1], m = 2 * n - 1;
            float *q = g_f32;
            float *p = q + n * k;
            float *full = p + m * k;
            float *shifted = full + n * m;
            float *y = shifted + n * m;
            for (uint32_t i = 0; i < n * k; i++) q[i] = RandFloat();
            for (uint32_t i = 0; i < m * k; i++) p[i] = RandFloat();
            for (uint32_t i = 0; i < n * n; i++) y[i] = 0.0f;
            tlib::ops::kernels_c::linear_f32(q, p, full, k / 4, n, m, m);
            ShiftRef(full, shifted, n, m);
            tlib::ops::kernels_c::relpos_f32(q, p, y, k / 4, n);
            for (uint32_t r = 0; r < n; r++)
            {
                for (uint32_t c = 0; c < n; c++)
                {
                    values++;
                    const float want = shifted[r * m + c];
                    if (memcmp(&y[r * n + c], &want, sizeof(float)) != 0)
                    {
                        if (differ == 0)
                        {
                            snprintf(first, sizeof(first), "k %u n %u at [%u][%u]", static_cast<unsigned>(k),
                                     static_cast<unsigned>(n), static_cast<unsigned>(r), static_cast<unsigned>(c));
                        }
                        differ++;
                    }
                }
            }
        }
        if (differ == 0)
        {
            Say("PASS relpos: %u values bit for bit", static_cast<unsigned>(values));
        }
        else
        {
            Say("FAIL relpos: %u of %u values differ; first: %s", static_cast<unsigned>(differ),
                static_cast<unsigned>(values), first);
        }
    }

    // The portable quantize step (tlib_kernels_c.cpp's RoundClamp off
    // RISC-V).
    int8_t RoundClampRef(const float v)
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

    // kernels_c::quantize (FCVT.W.S on the P4) against RoundClampRef for
    // every shift: the halves and edges around the clamp, zeros,
    // infinities, NaNs, the largest floats, then random floats of every
    // exponent.
    void CheckQuantize(void)
    {
        const uint32_t kEdges[] = {
            0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u,
            0x7F7FFFFFu, 0xFF7FFFFFu, 0x00000001u, 0x80000001u, 0x3F000000u, 0xBF000000u, 0x3FC00000u,
            0xBFC00000u, 0x40200000u, 0xC0200000u, 0x42FF0000u, 0x42FE0000u, 0x42FD0000u, 0xC3008000u,
            0xC3000000u, 0xC2FF0000u, 0x43000000u, 0x4AFFFFFFu, 0xCAFFFFFFu, 0x4B000000u, 0x4F000000u,
            0xCF000000u, 0x4F800000u,
        };
        const uint32_t n_edges = sizeof(kEdges) / sizeof(kEdges[0]);
        const uint32_t n = 4096;   // floats per shift, in block 2
        float *in = g_f32;
        int8_t *out = reinterpret_cast<int8_t *>(g_f32 + n);
        uint32_t values = 0, differ = 0;
        char first[120] = { 0 };
        for (uint8_t shift = 0; shift < 8; shift++)
        {
            for (uint32_t i = 0; i < n; i++)
            {
                uint32_t bits = i < n_edges ? kEdges[i] : Rand();
                memcpy(&in[i], &bits, sizeof(float));
            }
            tlib::ops::kernels_c::quantize(in, out, n / 4, shift);
            const float scale = static_cast<float>(1u << shift);
            for (uint32_t i = 0; i < n; i++)
            {
                const int8_t want = RoundClampRef(in[i] * scale);
                values++;
                if (out[i] != want)
                {
                    if (differ == 0)
                    {
                        uint32_t bits;
                        memcpy(&bits, &in[i], 4);
                        snprintf(first, sizeof(first), "shift %u, x %08X: %d, portable %d", static_cast<unsigned>(shift),
                                 static_cast<unsigned>(bits), static_cast<int>(out[i]), static_cast<int>(want));
                    }
                    differ++;
                }
            }
        }
        if (differ == 0)
        {
            Say("PASS quantize: %u values, the portable rounding's", static_cast<unsigned>(values));
        }
        else
        {
            Say("FAIL quantize: %u of %u values differ; first: %s", static_cast<unsigned>(differ),
                static_cast<unsigned>(values), first);
        }
    }

    void CpuClock(void)
    {
        const uint32_t c0 = Cycles();
        const uint32_t t0 = g_api->ticks_ms();
        g_api->delay_ms(200);
        const uint32_t c = Cycles() - c0;
        const uint32_t t = g_api->ticks_ms() - t0;
        Say("cpu: %u cycles in %u ms (%u MHz)", static_cast<unsigned>(c), static_cast<unsigned>(t),
            static_cast<unsigned>(t ? c / (t * 1000u) : 0));
    }

    bool kc_init(const AudioPlugHostApi *api, void *const *blocks)
    {
        g_api = api;
        g_tile = static_cast<int8_t *>(blocks[0]);
        int8_t *p = static_cast<int8_t *>(blocks[1]);
        g_sa = p;
        g_ca = p + kStressABytes;
        g_cb = g_ca + kSumsABytes;
        g_f32 = static_cast<float *>(blocks[2]);
        return true;
    }

    void kc_worker(const AudioPlugHostApi *api)
    {
        char cmd[AUDIOPLUG_CMD_MAX + 1];
        while (!api->should_stop())
        {
            api->wait(200);
            uint32_t n;
            while (!api->should_stop() && (n = api->command(cmd, AUDIOPLUG_CMD_MAX)) != 0)
            {
                cmd[n] = 0;
                if (strcmp(cmd, "kcheck") == 0)
                {
                    const uint32_t t0 = api->ticks_ms();
                    g_seed = 0x9E3779B9u;
                    CpuClock();
                    for (const Loop &loop : kLoops)
                    {
                        CheckSums(loop);
                        api->delay_ms(1);
                    }
                    for (const Loop &loop : kLoops)
                    {
                        Stress(loop, 4000);
                    }
                    TimeSums();
                    CheckF32();
                    CheckRelpos();
                    CheckQuantize();
                    Say("kcheck took %u ms", static_cast<unsigned>(api->ticks_ms() - t0));
                }
                else
                {
                    Publish("E:unknown command");
                }
                Publish("D");
            }
        }
    }

    void kc_status(char *out, uint32_t n)
    {
        snprintf(out, n, "P4 kernel check");
    }

    void kc_deinit(const AudioPlugHostApi *api)
    {
        (void)api;
        g_api = nullptr;
    }
} // namespace

extern "C" __attribute__((visibility("default"))) const AudioPlugDesc audioplug_ops = {
    AUDIO_PLUGIN_ABI_VERSION,
    "kcheck_p4",
    { { { kTileBytes + 16, AUDIOPLUG_MEM_INTERNAL },
        { kStressABytes + kSumsABytes + kSumsBBytes, AUDIOPLUG_MEM_PSRAM },
        { kF32Bytes, AUDIOPLUG_MEM_PSRAM },
        { 0, AUDIOPLUG_MEM_NONE } },
      8 * 1024 },
    kc_init,
    kc_worker,
    kc_status,
    kc_deinit,
};

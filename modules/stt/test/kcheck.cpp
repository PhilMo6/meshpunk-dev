// kcheck.cpp — the kernel check (build.ps1 -Variant kcheck): an audio plugin
// for the ESP32-S3 that runs each of the engine's S3 kernels and its
// portable version (tlib_kernels_c.h, dsp/fft4r.h, dsp/exact_angle.h) on the
// same inputs and compares the outputs bit for bit. Command "kcheck": one
// T: line per check ("PASS <name>: <n> values" or "FAIL <name>: ..." with
// the first difference), then D. Inputs: a fixed-seed generator plus edge
// values (int8 -128 and 127, exact rounding ties, infinities, NaN, -0,
// subnormals).

#include <math.h>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../src/audio/audio_plugin_abi.h"

#include <tlib_kernels_c.h>
#include <dsp/exact_angle.h>
#include <dsp/fft4r.h>

extern "C"
{
    void tlib_linear_b_relu_impl_lshift_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint8_t lshift);
    void tlib_linear_b_relu_impl_rshift_esp32s3(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint8_t rshift);
    void tlib_linear_relu_impl_lshift_esp32s3(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint8_t lshift);
    void tlib_linear_relu_impl_rshift_esp32s3(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k16, const uint32_t n, const uint32_t m, const uint8_t rshift);
#define DEQ(s) void tlib_linear_deq_impl_shift##s##_esp32s3(const int8_t *, const int8_t *, float *, const uint32_t, const uint32_t, const uint32_t);
#define BDEQ(s) void tlib_linear_b_deq_impl_shift##s##_esp32s3(const int8_t *, const int8_t *, const int32_t *, float *, const uint32_t, const uint32_t, const uint32_t);
#define QUANT(s) void tlib_quantize_shift##s##_esp32s3(const float *a, int8_t *a_q, uint32_t k4);
    DEQ(00) DEQ(01) DEQ(02) DEQ(03) DEQ(04) DEQ(05) DEQ(06) DEQ(07)
    DEQ(08) DEQ(09) DEQ(10) DEQ(11) DEQ(12) DEQ(13) DEQ(14)
    BDEQ(00) BDEQ(01) BDEQ(02) BDEQ(03) BDEQ(04) BDEQ(05) BDEQ(06) BDEQ(07)
    BDEQ(08) BDEQ(09) BDEQ(10) BDEQ(11) BDEQ(12) BDEQ(13) BDEQ(14)
    QUANT(00) QUANT(01) QUANT(02) QUANT(03) QUANT(04) QUANT(05) QUANT(06) QUANT(07)
    void tlib_linear_impl_esp32s3(const float *a, const float *b, float *y, const uint32_t k4, const uint32_t n, const uint32_t m);
}

namespace kc = tlib::ops::kernels_c;

namespace
{
    using DeqKernel = void (*)(const int8_t *, const int8_t *, float *, const uint32_t, const uint32_t, const uint32_t);
    using BDeqKernel = void (*)(const int8_t *, const int8_t *, const int32_t *, float *, const uint32_t, const uint32_t, const uint32_t);
    using QuantKernel = void (*)(const float *, int8_t *, uint32_t);

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
    const QuantKernel kQuant[8] = {
        tlib_quantize_shift00_esp32s3, tlib_quantize_shift01_esp32s3,
        tlib_quantize_shift02_esp32s3, tlib_quantize_shift03_esp32s3,
        tlib_quantize_shift04_esp32s3, tlib_quantize_shift05_esp32s3,
        tlib_quantize_shift06_esp32s3, tlib_quantize_shift07_esp32s3,
    };

    const AudioPlugHostApi *g_api = nullptr;

    // ── Output ───────────────────────────────────────────────────────────────

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

    // One check's tally: values compared, values that differ, the first.
    struct Tally
    {
        const char *name;
        uint32_t values;
        uint32_t differ;
        char first[160];
    };

    void Begin(Tally &t, const char *name)
    {
        t.name = name;
        t.values = 0;
        t.differ = 0;
        t.first[0] = 0;
    }

    void Report(const Tally &t)
    {
        if (t.differ == 0)
        {
            Say("PASS %s: %u values", t.name, static_cast<unsigned>(t.values));
        }
        else
        {
            Say("FAIL %s: %u of %u differ; first: %s", t.name, static_cast<unsigned>(t.differ),
                static_cast<unsigned>(t.values), t.first);
        }
    }

    uint32_t Bits(const float f)
    {
        uint32_t u;
        memcpy(&u, &f, sizeof(u));
        return u;
    }

    float FromBits(const uint32_t u)
    {
        float f;
        memcpy(&f, &u, sizeof(f));
        return f;
    }

    // ── Inputs ───────────────────────────────────────────────────────────────

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

    // Full range with -128 / 127 often, or small values (the shifted sums
    // then land between the clamps).
    void FillInt8(int8_t *p, const size_t len, const bool small)
    {
        for (size_t i = 0; i < len; i++)
        {
            const uint32_t r = Rand();
            if (small)
            {
                p[i] = static_cast<int8_t>(static_cast<int32_t>(r % 13) - 6);
            }
            else if ((r & 15) == 0)
            {
                p[i] = -128;
            }
            else if ((r & 15) == 1)
            {
                p[i] = 127;
            }
            else
            {
                p[i] = static_cast<int8_t>(r >> 8);
            }
        }
    }

    // A float of random sign and mantissa, its exponent 2^lo .. 2^hi; one
    // in 32 is zero.
    float RandFloat(const int lo, const int hi)
    {
        const uint32_t r = Rand();
        if ((r & 31) == 0)
        {
            return 0.0f;
        }
        const int e = lo + static_cast<int>(Rand() % static_cast<uint32_t>(hi - lo + 1));
        const uint32_t bits = (r & 0x80000000u) | (static_cast<uint32_t>(e + 127) << 23) | (Rand() & 0x7FFFFFu);
        return FromBits(bits);
    }

    // ── Buffers (16-byte aligned; the S3 kernels read one block past a) ─────

    constexpr uint32_t kMaxN = 5;
    constexpr uint32_t kMaxM = 33;
    constexpr uint32_t kMaxK = 256;
    constexpr uint32_t kQuantLen = 1024;

    alignas(16) int8_t g_a8[kMaxN * kMaxK + 16];
    alignas(16) int8_t g_b8[kMaxM * kMaxK + 16];
    alignas(16) int32_t g_c32[kMaxM];
    alignas(16) int8_t g_y8_s[kMaxN * kMaxM];
    alignas(16) int8_t g_y8_c[kMaxN * kMaxM];
    alignas(16) float g_yf_s[kMaxN * kMaxM];
    alignas(16) float g_yf_c[kMaxN * kMaxM];
    alignas(16) float g_af[kMaxN * kMaxK + 4];
    alignas(16) float g_bf[kMaxM * kMaxK + 4];
    alignas(16) float g_qx[kQuantLen];
    alignas(16) int8_t g_q_s[kQuantLen];
    alignas(16) int8_t g_q_c[kQuantLen];
    alignas(16) float g_table_s[2048];
    alignas(16) float g_table_c[2048];
    alignas(16) float g_fft_s[512];
    alignas(16) float g_fft_c[512];

    struct Shape
    {
        uint32_t n, m, k;
    };
    const Shape kShapes[] = { { 1, 1, 16 }, { 3, 17, 64 }, { 5, 33, 256 }, { 2, 7, 176 } };

    void CompareInt8(Tally &t, const int8_t *s, const int8_t *c, const uint32_t len, const char *what)
    {
        for (uint32_t i = 0; i < len; i++)
        {
            t.values++;
            if (s[i] != c[i])
            {
                if (t.differ == 0)
                {
                    snprintf(t.first, sizeof(t.first), "%s [%u]: s3 %d, c %d", what, static_cast<unsigned>(i), s[i], c[i]);
                }
                t.differ++;
            }
        }
    }

    void CompareFloat(Tally &t, const float *s, const float *c, const uint32_t len, const char *what)
    {
        for (uint32_t i = 0; i < len; i++)
        {
            t.values++;
            if (Bits(s[i]) != Bits(c[i]))
            {
                if (t.differ == 0)
                {
                    snprintf(t.first, sizeof(t.first), "%s [%u]: s3 0x%08x, c 0x%08x", what, static_cast<unsigned>(i),
                             static_cast<unsigned>(Bits(s[i])), static_cast<unsigned>(Bits(c[i])));
                }
                t.differ++;
            }
        }
    }

    // ── The checks ───────────────────────────────────────────────────────────

    void CheckInt8Linear(void)
    {
        Tally relu_l, relu_r, brelu_l, brelu_r, deq, bdeq;
        Begin(relu_l, "linear_relu_lshift");
        Begin(relu_r, "linear_relu_rshift");
        Begin(brelu_l, "linear_b_relu_lshift");
        Begin(brelu_r, "linear_b_relu_rshift");
        Begin(deq, "linear_deq");
        Begin(bdeq, "linear_b_deq");
        char what[64];
        for (const Shape &sh : kShapes)
        {
            for (int regime = 0; regime < 2; regime++)
            {
                const bool small = regime == 1;
                FillInt8(g_a8, sh.n * sh.k + 16, small);
                FillInt8(g_b8, sh.m * sh.k, small);
                for (uint32_t j = 0; j < sh.m; j++)
                {
                    const uint32_t r = Rand();
                    g_c32[j] = small ? static_cast<int32_t>(r % 401) - 200 : static_cast<int32_t>(r % 2097153) - 1048576;
                }
                const uint32_t k16 = sh.k / 16;
                const uint32_t out = sh.n * sh.m;
                for (uint8_t s = 0; s < 16; s++)
                {
                    snprintf(what, sizeof(what), "n%u m%u k%u %s shift %u", static_cast<unsigned>(sh.n),
                             static_cast<unsigned>(sh.m), static_cast<unsigned>(sh.k), small ? "small" : "full", s);
                    tlib_linear_relu_impl_lshift_esp32s3(g_a8, g_b8, g_y8_s, k16, sh.n, sh.m, s);
                    kc::linear_relu_lshift(g_a8, g_b8, g_y8_c, k16, sh.n, sh.m, sh.m, s);
                    CompareInt8(relu_l, g_y8_s, g_y8_c, out, what);
                    tlib_linear_relu_impl_rshift_esp32s3(g_a8, g_b8, g_y8_s, k16, sh.n, sh.m, s);
                    kc::linear_relu_rshift(g_a8, g_b8, g_y8_c, k16, sh.n, sh.m, sh.m, s);
                    CompareInt8(relu_r, g_y8_s, g_y8_c, out, what);
                    tlib_linear_b_relu_impl_lshift_esp32s3(g_a8, g_b8, g_c32, g_y8_s, k16, sh.n, sh.m, s);
                    kc::linear_b_relu_lshift(g_a8, g_b8, g_c32, g_y8_c, k16, sh.n, sh.m, sh.m, s);
                    CompareInt8(brelu_l, g_y8_s, g_y8_c, out, what);
                    tlib_linear_b_relu_impl_rshift_esp32s3(g_a8, g_b8, g_c32, g_y8_s, k16, sh.n, sh.m, s);
                    kc::linear_b_relu_rshift(g_a8, g_b8, g_c32, g_y8_c, k16, sh.n, sh.m, sh.m, s);
                    CompareInt8(brelu_r, g_y8_s, g_y8_c, out, what);
                    if (s < 15)
                    {
                        kDeq[s](g_a8, g_b8, g_yf_s, k16, sh.n, sh.m);
                        kc::linear_deq(g_a8, g_b8, g_yf_c, k16, sh.n, sh.m, sh.m, s);
                        CompareFloat(deq, g_yf_s, g_yf_c, out, what);
                        kBDeq[s](g_a8, g_b8, g_c32, g_yf_s, k16, sh.n, sh.m);
                        kc::linear_b_deq(g_a8, g_b8, g_c32, g_yf_c, k16, sh.n, sh.m, sh.m, s);
                        CompareFloat(bdeq, g_yf_s, g_yf_c, out, what);
                    }
                }
            }
        }
        Report(relu_l);
        Report(relu_r);
        Report(brelu_l);
        Report(brelu_r);
        Report(deq);
        Report(bdeq);
    }

    void CheckF32Linear(void)
    {
        Tally t;
        Begin(t, "linear_f32");
        const Shape shapes[] = { { 1, 1, 4 }, { 3, 17, 64 }, { 5, 33, 132 }, { 4, 9, 256 } };
        char what[64];
        for (const Shape &sh : shapes)
        {
            for (int range = 0; range < 2; range++)
            {
                const int lo = range == 0 ? -10 : -30;
                const int hi = range == 0 ? 10 : 30;
                for (uint32_t i = 0; i < sh.n * sh.k + 4; i++)
                {
                    g_af[i] = RandFloat(lo, hi);
                }
                for (uint32_t i = 0; i < sh.m * sh.k; i++)
                {
                    g_bf[i] = RandFloat(lo, hi);
                }
                snprintf(what, sizeof(what), "n%u m%u k%u range %d", static_cast<unsigned>(sh.n),
                         static_cast<unsigned>(sh.m), static_cast<unsigned>(sh.k), hi);
                tlib_linear_impl_esp32s3(g_af, g_bf, g_yf_s, sh.k / 4, sh.n, sh.m);
                kc::linear_f32(g_af, g_bf, g_yf_c, sh.k / 4, sh.n, sh.m, sh.m);
                CompareFloat(t, g_yf_s, g_yf_c, sh.n * sh.m, what);
            }
        }
        Report(t);
    }

    void CheckQuantize(void)
    {
        Tally t;
        Begin(t, "quantize");
        char what[48];
        for (uint8_t s = 0; s < 8; s++)
        {
            const float inv = 1.0f / static_cast<float>(1u << s);
            uint32_t len = 0;
            // ties: (j + 0.5) / 2^s, and the values next to them
            for (int j = -131; j <= 130; j++)
            {
                const float tie = (static_cast<float>(j) + 0.5f) * inv;
                g_qx[len++] = tie;
                g_qx[len++] = FromBits(Bits(tie) + 1);
            }
            const float special[] = {
                INFINITY, -INFINITY, NAN, 0.0f, -0.0f, FromBits(0x00000001u), FromBits(0x80400000u),
                127.5f * inv, -128.5f * inv, 127.49999f * inv, -128.49998f * inv, 1e30f, -1e30f,
                FromBits(0x7F7FFFFFu), FromBits(0xFF7FFFFFu), 0.5f * inv, -0.5f * inv, 1.5f * inv,
            };
            for (const float v : special)
            {
                g_qx[len++] = v;
            }
            while (len < kQuantLen)
            {
                g_qx[len++] = (static_cast<float>(static_cast<int32_t>(Rand() % 70001) - 35000) / 125.0f) * inv;
            }
            snprintf(what, sizeof(what), "shift %u", s);
            kQuant[s](g_qx, g_q_s, kQuantLen / 4);
            kc::quantize(g_qx, g_q_c, kQuantLen / 4, s);
            CompareInt8(t, g_q_s, g_q_c, kQuantLen, what);
        }
        Report(t);
    }

    void CheckTables(void)
    {
        Tally t;
        Begin(t, "fft4r_init");
        const bool ok_s = dsp::fft4r_init(g_table_s, 256);
        const bool ok_c = dsp::fft4r_init_c(g_table_c, 256);
        if (!ok_s || !ok_c)
        {
            Say("FAIL fft4r_init: returned %d (s3) and %d (c)", ok_s ? 1 : 0, ok_c ? 1 : 0);
            return;
        }
        CompareFloat(t, g_table_s, g_table_c, 1024, "table");
        Report(t);

        Tally w;
        Begin(w, "hann angles");
        for (uint32_t i = 0; i < 512; i++)
        {
            const float s3 = (M_PI * i) / static_cast<float>(511);
            const float c = dsp::exact_angle(dsp::kPiBits, i, 511);
            CompareFloat(w, &s3, &c, 1, "i");
        }
        Report(w);

        Tally r;
        Begin(r, "exact_angle random");
        for (uint32_t n = 0; n < 4000; n++)
        {
            const uint32_t i = Rand() % 2048;
            const uint32_t d = 1 + Rand() % 65535;
            const bool two = (n & 1) != 0;
            const float s3 = two ? static_cast<float>(2 * M_PI * i / static_cast<float>(d))
                                 : static_cast<float>((M_PI * i) / static_cast<float>(d));
            const float c = dsp::exact_angle(two ? dsp::kTwoPiBits : dsp::kPiBits, i, d);
            char what[40];
            snprintf(what, sizeof(what), "i %u d %u %s", static_cast<unsigned>(i), static_cast<unsigned>(d), two ? "2pi" : "pi");
            CompareFloat(r, &s3, &c, 1, what);
        }
        Report(r);
    }

    void CheckFft(void)
    {
        Tally f, b;
        Begin(f, "fft4r");
        Begin(b, "cplx2real");
        char what[32];
        for (int frame = 0; frame < 8; frame++)
        {
            for (int i = 0; i < 512; i++)
            {
                float v;
                if (frame == 0)
                {
                    v = (i == 0) ? 1.0f : 0.0f;
                }
                else if (frame == 1)
                {
                    v = cosf(0.12345f * static_cast<float>(i));
                }
                else
                {
                    v = static_cast<float>(static_cast<int32_t>(Rand() % 65536) - 32768) / 32768.0f;
                }
                g_fft_s[i] = v;
                g_fft_c[i] = v;
            }
            snprintf(what, sizeof(what), "frame %d", frame);
            dsps_fft4r_fc32_aes3_(g_fft_s, 256, g_table_s, 512);
            dsp::fft4r_c(g_fft_c, 256, g_table_s, 512);
            CompareFloat(f, g_fft_s, g_fft_c, 512, what);
            dsp::bit_rev4r(g_fft_s, 256);
            memcpy(g_fft_c, g_fft_s, sizeof(g_fft_s));
            dsps_cplx2real_fc32_ae32_(g_fft_s, 256, g_table_s, 512);
            dsp::cplx2real_c(g_fft_c, 256, g_table_s, 512);
            CompareFloat(b, g_fft_s, g_fft_c, 512, what);
        }
        Report(f);
        Report(b);
    }

    void RunAll(void)
    {
        const uint32_t t0 = g_api->ticks_ms();
        g_seed = 0x9E3779B9u;
        CheckInt8Linear();
        CheckF32Linear();
        CheckQuantize();
        CheckTables();
        CheckFft();
        Say("kcheck took %u ms", static_cast<unsigned>(g_api->ticks_ms() - t0));
    }

    // ── Plugin entry points ──────────────────────────────────────────────────

    bool kc_init(const AudioPlugHostApi *api, void *const *blocks)
    {
        (void)blocks;
        g_api = api;
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
                    RunAll();
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
        snprintf(out, n, "kernel check");
    }

    void kc_deinit(const AudioPlugHostApi *api)
    {
        (void)api;
        g_api = nullptr;
    }
} // namespace

// A failed assert (newlib's <assert.h> calls this): logged, then the trap
// stops the device with the plugin's backtrace.
extern "C" void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    if (g_api)
    {
        g_api->log("FAIL: assert %s at %s:%d (%s)", expr, file, line, func ? func : "?");
    }
    __builtin_trap();
}

extern "C" __attribute__((visibility("default"))) const AudioPlugDesc audioplug_ops = {
    AUDIO_PLUGIN_ABI_VERSION,
    "kcheck",
    { { { 0, AUDIOPLUG_MEM_NONE },
        { 0, AUDIOPLUG_MEM_NONE },
        { 0, AUDIOPLUG_MEM_NONE },
        { 0, AUDIOPLUG_MEM_NONE } },
      8 * 1024 },
    kc_init,
    kc_worker,
    kc_status,
    kc_deinit,
};

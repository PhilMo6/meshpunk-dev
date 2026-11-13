// fft4r_c.cpp — see fft4r.h. C versions of this folder's ESP32-S3 code
// from esp-dsp 1.8.2 (Apache-2.0, Copyright 2018-2025 Espressif Systems
// (Shanghai) CO LTD): fft4r_c does what fft4r_aes3.S does and cplx2real_c
// what cplx2real_ae32.c does — the same float operations in the same order,
// a madd.s / msub.s being one fused multiply-add here — and fft4r_init_c
// fills the table fft4r_init fills, its angles from exact_angle.h.

#include "fft4r.h"
#include "exact_angle.h"

#include <math.h>

namespace dsp
{

    bool fft4r_init_c(float *table, int max_fft_size)
    {
        if (max_fft_size <= 0 || (max_fft_size & (max_fft_size - 1)) != 0)
        {
            return false;
        }
        const int table_size = max_fft_size * 2;
        for (int i = 0; i < table_size; i++)
        {
            // fft4r_init: float angle = 2 * M_PI * i / (float)table_size;
            const float angle = exact_angle(kTwoPiBits, static_cast<uint32_t>(i), static_cast<uint32_t>(table_size));
            table[2 * i + 0] = cosf(angle);
            table[2 * i + 1] = sinf(angle);
        }
        return true;
    }

#ifdef STT_KERNELS_C
    bool fft4r_init(float *table, int max_fft_size)
    {
        return fft4r_init_c(table, max_fft_size);
    }
#endif

    // n a power of 4 (the caller passes kNFFT / 2). The comments name the
    // S3 code's registers.
    void fft4r_c(float *data, int n, const float *table, int table_size)
    {
        int log2n = 0;
        while ((1 << log2n) < n)
        {
            log2n++;
        }
        int log4n = log2n >> 1;
        int len = n;
        int m = 2;
        int w_step = table_size >> (log2n - 1);   // floats per k for w0
        while (log4n > 0)
        {
            len >>= 2;
            for (int j = 0; j < m; j += 2)
            {
                float *p0 = data + 2 * ((j * len) << 1);
                float *p1 = p0 + 2 * len;
                float *p2 = p1 + 2 * len;
                float *p3 = p2 + 2 * len;
                const float *w0 = table;
                const float *w1 = table;
                const float *w2 = table;
                for (int k = 0; k < len; k++)
                {
                    const float in0re = p0[0], in0im = p0[1];
                    const float in2re = p2[0], in2im = p2[1];
                    const float s02im = in0im + in2im;    // f5
                    const float d02im = in0im - in2im;    // f7
                    const float s02re = in0re + in2re;    // f4
                    const float d02re = in0re - in2re;    // f6
                    const float in1re = p1[0], in1im = p1[1];
                    const float in3re = p3[0], in3im = p3[1];
                    const float s13im = in1im + in3im;    // f9
                    const float d13im = in1im - in3im;    // f11
                    const float s13re = in1re + in3re;    // f8
                    const float d13re = in1re - in3re;    // f10
                    const float bf2im = s02im - s13im;    // f1
                    const float bf0im = s02im + s13im;    // f5
                    const float bf1re = d02re + d13im;    // f2
                    const float bf3re = d02re - d13im;    // f6
                    const float bf2re = s02re - s13re;    // f0
                    const float bf0re = s02re + s13re;    // f4
                    const float bf1im = d02im - d13re;    // f3
                    const float bf3im = d02im + d13re;    // f7
                    p0[0] = bf0re;
                    p0[1] = bf0im;
                    p1[0] = __builtin_fmaf(bf1im, w0[1], bf1re * w0[0]);    // madd.s f4, f3, f12
                    p1[1] = __builtin_fmaf(-bf1re, w0[1], bf1im * w0[0]);   // msub.s f5, f2, f12
                    p2[0] = __builtin_fmaf(bf2im, w1[1], bf2re * w1[0]);    // madd.s f8, f1, f13
                    p2[1] = __builtin_fmaf(-bf2re, w1[1], bf2im * w1[0]);   // msub.s f9, f0, f13
                    p3[0] = __builtin_fmaf(bf3im, w2[1], bf3re * w2[0]);    // madd.s f10, f7, f14
                    p3[1] = __builtin_fmaf(-bf3re, w2[1], bf3im * w2[0]);   // msub.s f11, f6, f14
                    w0 += w_step;
                    w1 += 2 * w_step;
                    w2 += 3 * w_step;
                    p0 += 2;
                    p1 += 2;
                    p2 += 2;
                    p3 += 2;
                }
            }
            m <<= 2;
            w_step <<= 2;
            log4n--;
        }
    }

    // The comments name cplx2real_ae32.c's registers.
    void cplx2real_c(float *data, int fft_points, const float *table, int table_size)
    {
        const int wind_step = table_size / fft_points;
        const float *win0 = table + wind_step;
        const float *win1 = table + wind_step * 2;

        float *ptr_inv = data + fft_points * 2;
        const float dc0 = data[0];
        const float dc1 = data[1];
        data[0] = dc0 + dc1;    // f6
        data[1] = dc0 - dc1;    // f7
        ptr_inv -= 4;

        const int count = fft_points >> 2;
        for (int t = 0; t < count; t++)
        {
            float f1 = data[3];
            const float f3 = ptr_inv[3];
            float f0 = data[2];
            const float f2 = ptr_inv[2];
            float f7 = data[5];
            const float f9 = ptr_inv[1];
            float f6 = data[4];
            const float f8 = ptr_inv[0];
            const float f5 = f1 - f3;
            const float f4 = f0 + f2;
            const float f11 = f7 - f9;
            const float f10 = f6 + f8;
            f1 = f1 + f3;
            f0 = f0 - f2;
            f7 = f7 + f9;
            f6 = f6 - f8;
            // tw1 = (t2, t3), tw2 = (t8, t9)
            float t3 = f1 * win0[1];
            float t2 = f0 * win0[1];
            float t9 = f7 * win1[1];
            float t8 = f6 * win1[1];
            t3 = __builtin_fmaf(f0, win0[0], t3);     // madd.s f3, f0, f12
            t2 = __builtin_fmaf(-f1, win0[0], t2);    // msub.s f2, f1, f12
            t9 = __builtin_fmaf(f6, win1[0], t9);     // madd.s f9, f6, f13
            t8 = __builtin_fmaf(-f7, win1[0], t8);    // msub.s f8, f7, f13
            win0 += 2 * wind_step;
            win1 += 2 * wind_step;

            const float o1 = f5 - t3;
            const float o0 = f4 - t2;
            const float o3 = t3 + f5;
            const float o2 = f4 + t2;
            const float o7 = f11 - t9;
            const float o6 = f10 - t8;
            const float o9 = t9 + f11;
            const float o8 = f10 + t8;

            data[3] = o1 * 0.5f;
            data[2] = o0 * 0.5f;
            ptr_inv[3] = o3 * -0.5f;
            ptr_inv[2] = o2 * 0.5f;
            data[5] = o7 * 0.5f;
            data[4] = o6 * 0.5f;
            data += 4;
            ptr_inv[1] = o9 * -0.5f;
            ptr_inv[0] = o8 * 0.5f;
            ptr_inv -= 4;
        }
    }

} // namespace dsp

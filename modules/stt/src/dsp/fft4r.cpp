// fft4r.cpp — see fft4r.h. Ported from esp-dsp 1.8.2 modules/fft/float/
// dsps_fft4r_fc32_ansi.c (Apache-2.0, Copyright 2018-2019 Espressif Systems
// (Shanghai) PTE LTD): dsps_fft4r_init_fc32 (the twiddle table only) and
// dsps_bit_rev4r_direct_fc32_ansi, arithmetic unchanged; the esp_err_t
// returns, the initialized flag and the global table pointer are gone (the
// caller passes its table). fft4r_init here computes in double: the S3 and
// kernel-check builds; the builds without the S3 assembly get theirs from
// fft4r_c.cpp.

#include "fft4r.h"

#include <math.h>

namespace
{
    // log2(x) for a power of two, -1 otherwise.
    int power_of_two(int x)
    {
        int p = 0;
        while ((1 << p) < x)
        {
            p++;
        }
        return ((1 << p) == x) ? p : -1;
    }
} // namespace

namespace dsp
{

#ifndef STT_KERNELS_C
    bool fft4r_init(float *table, int max_fft_size)
    {
        if (max_fft_size <= 0 || power_of_two(max_fft_size) < 0)
        {
            return false;
        }
        const int table_size = max_fft_size * 2;
        for (int i = 0; i < table_size; i++)
        {
            float angle = 2 * M_PI * i / (float)table_size;
            table[2 * i + 0] = cosf(angle);
            table[2 * i + 1] = sinf(angle);
        }
        return true;
    }
#endif

    void bit_rev4r(float *data, int N)
    {
        int log2N = power_of_two(N);
        int log4N = log2N >> 1;

        float r_temp, i_temp;
        for (int i = 0; i < N; i++)
        {
            int cnt;
            int xx;
            int bits2;
            xx = 0;
            cnt = log4N;
            int j = i;
            while (cnt > 0)
            {
                bits2 = j & 0x3;
                xx = (xx << 2) + bits2;
                j = j >> 2;
                cnt--;
            }
            if (i < xx)
            {
                r_temp = data[i * 2 + 0];
                i_temp = data[i * 2 + 1];
                data[i * 2 + 0] = data[xx * 2 + 0];
                data[i * 2 + 1] = data[xx * 2 + 1];
                data[xx * 2 + 0] = r_temp;
                data[xx * 2 + 1] = i_temp;
            }
        }
    }

} // namespace dsp

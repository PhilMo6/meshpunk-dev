// fft4r.h — the front end's FFT (conformer_preprocessing_impl), from
// Espressif's esp-dsp 1.8.2 (Apache-2.0). The ESP32-S3 build runs the code
// upstream conformer-stt-s3 runs (esp-dsp with CONFIG_DSP_OPTIMIZED):
//   fft4r      S3 assembly, fft4r_aes3.S (dsps_fft4r_fc32_aes3_.S)
//   cplx2real  FPU assembly, cplx2real_ae32.c (dsps_fft4r_fc32_ae32.c)
//   fft4r_init and bit_rev4r: C, fft4r.cpp (dsps_fft4r_fc32_ansi.c). The
//   S3 build reverses the digits through a lookup table instead; both move
//   the same values to the same places.
// The builds without the S3 assembly (STT_KERNELS_C: build.ps1 -Variant s3c
// and rv32) run fft4r_c.cpp instead: fft4r_c and cplx2real_c (the same
// operations in the same order, in C) and fft4r_init_c (the same table,
// computed without double-precision arithmetic). The kernel check
// (-Variant kcheck) links both sets.

#ifndef FFT4R_H_
#define FFT4R_H_

#include <cstdint>

#ifndef STT_KERNELS_C
extern "C"
{
    int dsps_fft4r_fc32_aes3_(float *data, int n, const float *table, int table_size);
    int dsps_cplx2real_fc32_ae32_(float *data, int n, const float *table, int table_size);
}
#endif

namespace dsp
{
    // Fills table (2 * 2 * max_fft_size floats: cos, sin pairs) for complex
    // FFTs of up to max_fft_size points. false when max_fft_size is not a
    // power of two.
    bool fft4r_init(float *table, int max_fft_size);
    bool fft4r_init_c(float *table, int max_fft_size);

    // In-place radix-4 FFT of n complex points (2n floats, re/im); n a power
    // of 4. Output in digit-reversed order.
    void fft4r_c(float *data, int n, const float *table, int table_size);

    // Undo the digit reversal of fft4r (n complex points).
    void bit_rev4r(float *data, int n);

    // The real-signal step after a 2n-point real FFT computed as an n-point
    // complex FFT.
    void cplx2real_c(float *data, int n, const float *table, int table_size);

#ifdef STT_KERNELS_C
    inline void fft4r(float *data, int n, const float *table, int table_size)
    {
        fft4r_c(data, n, table, table_size);
    }

    inline void cplx2real(float *data, int n, const float *table, int table_size)
    {
        cplx2real_c(data, n, table, table_size);
    }
#else
    inline void fft4r(float *data, int n, const float *table, int table_size)
    {
        dsps_fft4r_fc32_aes3_(data, n, table, table_size);
    }

    inline void cplx2real(float *data, int n, const float *table, int table_size)
    {
        dsps_cplx2real_fc32_ae32_(data, n, table, table_size);
    }
#endif
} // namespace dsp

#endif

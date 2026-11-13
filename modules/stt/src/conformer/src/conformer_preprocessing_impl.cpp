// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/conformer/
// src_esp32s3/conformer_preprocessing_impl_esp32s3.cpp (Apache-2.0,
// Copyright 2026 lspr98): the FFT calls go to esp-dsp's code copied into
// dsp/ (fft4r.h) instead of the esp-dsp component; the same functions the
// component's ESP32-S3 build runs, with the table passed in. The code here
// is the same for every build (fft4r.h picks the FFT).

#include "conformer_preprocessing.h"

#include <cstring>

#include <config.h>
#include <dsp/fft4r.h>

namespace conformer {

// Complex points of the radix-4 FFT (a kNFFT-point real FFT), and the size
// of its twiddle table in (cos, sin) pairs (esp-dsp dsps_fft4r_w_table_size).
static constexpr int kFftPoints{kNFFT >> 1};
static constexpr int kFftTableSize{kFftPoints * 2};

Preprocessor::Preprocessor(void) {
    fft_buffer_ = tlib::Tensor<float>({kNFFT}, false, 0);
    fft_coefficients_ = tlib::Tensor<float>({static_cast<uint32_t>(kNFFT * 4)}, false, 0);
    scaled_hann_window_ = tlib::Tensor<float>({kNFFT}, false, 0);
    InitializeScaledHannWindow(scaled_hann_window_, kNFFT);
    const bool ok = dsp::fft4r_init(fft_coefficients_.Data(), kFftPoints);
    assert(ok);
    (void)ok;
}

void Preprocessor::STFTStep(const float * x_data, float * y_data) {
    const uint16_t n_freq{static_cast<uint16_t>(kNFFT / 2)};
    float * fft_buffer_data{fft_buffer_.Data()};
    memcpy(fft_buffer_data, x_data, sizeof(float) * kNFFT);
    // Perform radix 4 fft of size kNFFT
    dsp::fft4r(fft_buffer_data, n_freq, fft_coefficients_.DataImm(), kFftTableSize);
    dsp::bit_rev4r(fft_buffer_data, n_freq);
    dsp::cplx2real(fft_buffer_data, n_freq, fft_coefficients_.DataImm(), kFftTableSize);

    for(uint16_t i = 0; i < n_freq; i++) {
        y_data[i] = fft_buffer_data[2*i] * fft_buffer_data[2*i] + fft_buffer_data[2*i + 1] * fft_buffer_data[2*i + 1];
    }
}

} // conformer

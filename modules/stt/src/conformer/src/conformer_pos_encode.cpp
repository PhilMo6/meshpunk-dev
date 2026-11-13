// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/conformer/
// src/conformer_pos_encode.cpp (Apache-2.0, Copyright 2026 lspr98): the
// 2n-1 rows of the positional-encoding table a clip uses are read from SD
// (ReadRows) instead of copied out of the mapped table; the outputs are
// moved into the tuple instead of copied.

#include "conformer_pos_encode.h"

#include <utility>

#include <conformer_tensor_ids.h>
#include <tlib_flash.h>

namespace conformer
{

    static constexpr const float kScale{13.2664991614216f}; /*!< sqrt(kEmbeddingDimension) */

    std::tuple<tlib::Tensor<float>, tlib::Tensor<int8_t>> PosEncode(const tlib::TensorView<float> &x)
    {
        const auto pos_enc = tlib::flash::StreamTensor<int8_t>(POS_ENC_Q_INT8);
        const uint32_t enc_rows{pos_enc.Shape(0)};
        const uint32_t enc_cols{pos_enc.Shape(1)};

        const uint32_t in_len = x.Shape(0);
        assert(enc_rows / 2 > in_len);
        assert(enc_cols == x.Shape(1));

        const uint32_t center_pos{enc_rows / 2 + 1};
        const uint32_t start_pos{center_pos - in_len};
        const uint32_t end_pos{center_pos + in_len - 1};

        auto x_pos_enc = tlib::Tensor<int8_t>({end_pos - start_pos, enc_cols}, pos_enc.Quantized(), pos_enc.Shift());
        const bool ok = tlib::flash::ReadRows(POS_ENC_Q_INT8, start_pos, end_pos - start_pos, x_pos_enc.Data());
        assert(ok && "positional encoding read failed");
        (void)ok;

        auto x_scaled = tlib::Tensor<float>(x.Shape(), false, 0);
        const float *x_data{x.DataImm()};
        float *x_scaled_data{x_scaled.Data()};
        for (uint32_t i = 0; i < x_scaled.Numel(); i++)
        {
            x_scaled_data[i] = x_data[i] * kScale;
        }

        return {std::move(x_scaled), std::move(x_pos_enc)};
    }

} // conformer

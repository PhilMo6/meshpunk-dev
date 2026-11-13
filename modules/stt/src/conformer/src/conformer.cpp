// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/conformer/
// src/conformer.cpp (Apache-2.0, Copyright 2026 lspr98): the decoder weight
// stays on SD (StreamTensor) instead of being copied whole into SRAM; after
// each layer the plugin's stt_layer_done() runs (it drains the microphone
// and can end the pass: Forward then returns an empty tensor).

#include "conformer.h"

#include <conformer_tensor_ids.h>
#include <conformer_pos_encode.h>
#include <conformer_layer.h>

#include <tlib_ops.h>
#include <tlib_flash.h>

#include <port.h>

namespace conformer
{

    // Input shift for linear layer of the decoder
    static constexpr const uint8_t kDecoderInputShift{5};

    tlib::Tensor<float> Forward(const tlib::TensorView<float> &pre_enc)
    {
        auto [x, pos_enc] = PosEncode(pre_enc);

        for (uint16_t i = 0; i < kLayers; i++)
        {
            x = Layer(x, pos_enc, i);
            if (stt_layer_done(i))
            {
                return tlib::Tensor<float>();
            }
        }

        auto x_q = tlib::ops::quantize(x, kDecoderInputShift);

        x = tlib::ops::linear_deq(
            x_q,
            tlib::flash::StreamTensor<int8_t>(DECODER_LINEAR_WEIGHT_Q_INT8),
            tlib::flash::LoadTensor<int32_t>(DECODER_LINEAR_BIAS_Q_INT32, tlib::heap::Type::SRAM));

        return x;
    }

} // conformer

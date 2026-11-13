// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib_ops/
// inc/tlib_cat.h (Apache-2.0, Copyright 2026 lspr98): the tensors come as
// an array and a count instead of a std::vector; a profile stage
// (profile.h).

#ifndef TLIB_CAT_H_
#define TLIB_CAT_H_

#include <cstddef>

#include <profile.h>
#include <tlib_tensor.h>

namespace tlib::ops
{

    /**
     * Concatenates 2D tensors along the first dimension.
     *
     * @param   tensors Array of p [n, m] dtype tensors to be concatenated
     * @param   count   p
     *
     * @returns [n*p, m] dtype tensor, concatenation result
     */
    template <typename dtype>
    inline Tensor<dtype> cat(const tlib::Tensor<dtype> *tensors, const size_t count) {
        STT_PROF(kElementwise);
        assert(count > 0);
        assert(tensors[0].Dim() == 2);
        const uint32_t dim_1 = tensors[0].Shape(1);

        uint32_t dim_0 = 0;
        for (size_t i = 0; i < count; i++)
        {
            assert(tensors[i].Dim() == 2);
            assert(tensors[i].Shape(1) == dim_1);
            dim_0 += tensors[i].Shape(0);
        }

        Tensor<dtype> y({dim_0, dim_1}, false, 0);
        dtype *y_data{y.Data()};
        const auto max_bytes_copy{y.Bytes()};
        uint32_t n_bytes_copied{0};

        for (size_t i = 0; i < count; i++)
        {
            const auto n_elements = tensors[i].Numel();
            const auto n_bytes = tensors[i].Bytes();
            assert(n_bytes_copied + n_bytes <= max_bytes_copy);
            memcpy(y_data, tensors[i].DataImm(), n_bytes);
            y_data += n_elements;
            n_bytes_copied += n_bytes;
        }

        return y;
    }

} // tlib::ops

#endif

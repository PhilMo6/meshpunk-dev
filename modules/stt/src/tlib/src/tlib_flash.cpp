// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib/src/
// tlib_flash.cpp (Apache-2.0, Copyright 2026 lspr98): the file format and
// its parsing are upstream's (magic "CONFORMER1", tensor count, file size,
// then per tensor: type, dims, shape[4], stride[4], shift, numel,
// quantized, data at the next 16-byte boundary); the bytes come from SD
// through stt_model_read instead of a flash mapping; resident tensors go to
// up to two blocks (AddResidentRegion).

#include "../inc/tlib_flash.h"

#include <cassert>
#include <cstdint>
#include <cstring>

#include <config.h>
#include <port.h>

enum class TensorType : uint8_t
{
    UINT8 = 0U,
    INT8 = 1U,
    UINT16 = 2U,
    INT16 = 3U,
    UINT32 = 4U,
    INT32 = 5U,
    INT64 = 6U,
    FLOAT32 = 7U
};

namespace
{
    // Bytes of one tensor header on file: type, dims, shape[4], stride[4],
    // shift, numel, quantized.
    constexpr uint32_t kTensorHeaderBytes{1 + 1 + 4 * kFlashTensorMaxDims + 4 * kFlashTensorMaxDims + 1 + 4 + 1};
    constexpr const char kFileMagic[] = "CONFORMER1";   // 10 characters + NUL on file

    struct TensorInfo
    {
        TensorType type;
        uint8_t dims;
        uint8_t shift;
        bool quantized;
        uint32_t shape[kFlashTensorMaxDims];
        uint32_t numel;
        uint32_t data_offset;   // absolute file offset
        void *resident;         // copy in the resident block, or nullptr
    };

    // Resident memory: the block Initialize gets (the tensor table first),
    // then the one AddResidentRegion adds; each allocation takes the first
    // block with room for it.
    struct Region
    {
        uint8_t *base;
        size_t size;
        size_t used;
    };
    constexpr uint8_t kMaxRegions{2};
    Region regions_[kMaxRegions];
    uint8_t n_regions_{0};
    TensorInfo *table_{nullptr};
    uint32_t n_tensors_{0};

    void *ResidentAlloc(const size_t bytes)
    {
        for (uint8_t r = 0; r < n_regions_; r++)
        {
            Region &g = regions_[r];
            const size_t at = (g.used + 15) & ~static_cast<size_t>(15);
            if (at + bytes <= g.size)
            {
                g.used = at + bytes;
                return g.base + at;
            }
        }
        return nullptr;
    }

    uint32_t ReadU32(const uint8_t *p)
    {
        uint32_t v;
        memcpy(&v, p, sizeof(v));
        return v;
    }

    uint32_t TypeBytes(const TensorType t)
    {
        switch (t)
        {
        case TensorType::UINT8:
        case TensorType::INT8:
            return 1;
        case TensorType::UINT16:
        case TensorType::INT16:
            return 2;
        case TensorType::UINT32:
        case TensorType::INT32:
        case TensorType::FLOAT32:
            return 4;
        case TensorType::INT64:
            return 8;
        }
        return 0;
    }

    const TensorInfo &Info(const uint32_t tensor_id)
    {
        assert(table_ != nullptr && tensor_id < n_tensors_);
        return table_[tensor_id];
    }

    template <typename dtype>
    constexpr bool CheckTensorType(const TensorType &tensor_type);

    template <>
    constexpr bool CheckTensorType<int8_t>(const TensorType &tensor_type)
    {
        return tensor_type == TensorType::INT8;
    }

    template <>
    constexpr bool CheckTensorType<int16_t>(const TensorType &tensor_type)
    {
        return tensor_type == TensorType::INT16;
    }

    template <>
    constexpr bool CheckTensorType<int32_t>(const TensorType &tensor_type)
    {
        return tensor_type == TensorType::INT32;
    }

    template <>
    constexpr bool CheckTensorType<float>(const TensorType &tensor_type)
    {
        return tensor_type == TensorType::FLOAT32;
    }

    tlib::Dims InfoShape(const TensorInfo &t)
    {
        tlib::Dims d(t.dims);
        for (uint8_t i = 0; i < t.dims; i++)
        {
            d[i] = t.shape[i];
        }
        return d;
    }
} // namespace

namespace tlib::flash
{

    FlashError Initialize(uint8_t *resident, const size_t resident_size)
    {
        regions_[0] = Region{resident, resident_size, 0};
        n_regions_ = 1;
        table_ = nullptr;
        n_tensors_ = 0;

        // File header: magic + NUL, tensor count, file size; then one u32
        // offset per tensor (relative to the end of the meta partition).
        uint8_t head[11 + 4 + 8];
        if (!stt_model_read(0, head, sizeof(head)))
        {
            return FlashError::MetaSectorMountFailed;
        }
        if (memcmp(head, kFileMagic, sizeof(kFileMagic)) != 0)
        {
            return FlashError::MagicInvalid;
        }
        const uint32_t n = ReadU32(head + 11);
        uint64_t file_size;
        memcpy(&file_size, head + 15, sizeof(file_size));
        if (n == 0 || n > (kFlashMetaPartitionSize - sizeof(head)) / 4)
        {
            return FlashError::TensorCountInvalid;
        }
        if (file_size == 0)
        {
            return FlashError::FileSizeInvalid;
        }

        table_ = static_cast<TensorInfo *>(ResidentAlloc(n * sizeof(TensorInfo)));
        uint32_t *offsets = static_cast<uint32_t *>(ResidentAlloc(n * sizeof(uint32_t)));
        if (!table_ || !offsets)
        {
            return FlashError::ResidentFull;
        }
        if (!stt_model_read(sizeof(head), offsets, n * sizeof(uint32_t)))
        {
            return FlashError::MetaSectorMountFailed;
        }

        for (uint32_t i = 0; i < n; i++)
        {
            uint8_t h[kTensorHeaderBytes];
            const uint32_t at = kFlashMetaPartitionSize + offsets[i];
            if (!stt_model_read(at, h, sizeof(h)))
            {
                return FlashError::TensorSectorMountFailed;
            }
            TensorInfo &t = table_[i];
            t.type = static_cast<TensorType>(h[0]);
            t.dims = h[1];
            if (t.dims == 0 || t.dims > kFlashTensorMaxDims || TypeBytes(t.type) == 0)
            {
                return FlashError::TensorSectorMountFailed;
            }
            for (uint8_t d = 0; d < kFlashTensorMaxDims; d++)
            {
                t.shape[d] = ReadU32(h + 2 + 4 * d);
            }
            const uint8_t *p = h + 2 + 8 * kFlashTensorMaxDims;   // past shape and stride
            t.shift = p[0];
            t.numel = ReadU32(p + 1);
            t.quantized = p[5] != 0;
            const uint32_t header_end = at + kTensorHeaderBytes;
            t.data_offset = (header_end + (kFlashTensorAlignment - 1)) & ~static_cast<uint32_t>(kFlashTensorAlignment - 1);
            t.resident = nullptr;
        }
        n_tensors_ = n;
        return FlashError::None;
    }

    void AddResidentRegion(uint8_t *block, const size_t size)
    {
        assert(n_regions_ > 0 && n_regions_ < kMaxRegions);
        regions_[n_regions_++] = Region{block, size, 0};
    }

    FlashError MakeResident(const uint32_t tensor_id)
    {
        TensorInfo &t = table_[tensor_id];
        if (t.resident)
        {
            return FlashError::None;
        }
        const uint32_t bytes = t.numel * TypeBytes(t.type);
        void *p = ResidentAlloc(bytes);
        if (!p)
        {
            return FlashError::ResidentFull;
        }
        if (!stt_model_read(t.data_offset, p, bytes))
        {
            return FlashError::TensorSectorMountFailed;
        }
        t.resident = p;
        return FlashError::None;
    }

    size_t ResidentUsed(void)
    {
        size_t used = 0;
        for (uint8_t r = 0; r < n_regions_; r++)
        {
            used += regions_[r].used;
        }
        return used;
    }

    uint32_t TensorCount(void) { return n_tensors_; }

    template <typename dtype>
    TensorView<dtype> MapTensor(const uint32_t tensor_id)
    {
        const TensorInfo &t = Info(tensor_id);
        assert(CheckTensorType<dtype>(t.type));
        assert(t.resident != nullptr && "MapTensor of a tensor that is not resident");
        Dims shape = InfoShape(t);
        Dims stride(t.dims);
        uint32_t s = 1;
        for (size_t i = t.dims; i-- > 0;)
        {
            stride[i] = s;
            s *= shape[i];
        }
        return TensorView<dtype>(std::move(shape), std::move(stride), static_cast<const dtype *>(t.resident), t.shift, t.quantized);
    }

    template TensorView<int8_t> MapTensor<int8_t>(const uint32_t tensor_id);
    template TensorView<int16_t> MapTensor<int16_t>(const uint32_t tensor_id);
    template TensorView<int32_t> MapTensor<int32_t>(const uint32_t tensor_id);
    template TensorView<float> MapTensor<float>(const uint32_t tensor_id);

    template <typename dtype>
    Tensor<dtype> LoadTensor(const uint32_t tensor_id, const heap::Type type)
    {
        const TensorInfo &t = Info(tensor_id);
        assert(CheckTensorType<dtype>(t.type));
        Tensor<dtype> y(InfoShape(t), t.quantized, t.shift, type);
        if (t.resident)
        {
            memcpy(y.Data(), t.resident, y.Bytes());
        }
        else
        {
            const bool ok = stt_model_read(t.data_offset, y.Data(), static_cast<uint32_t>(y.Bytes()));
            assert(ok && "model tensor read failed");
            (void)ok;
        }
        return y;
    }

    template Tensor<int8_t> LoadTensor<int8_t>(const uint32_t tensor_id, const heap::Type type);
    template Tensor<int16_t> LoadTensor<int16_t>(const uint32_t tensor_id, const heap::Type type);
    template Tensor<int32_t> LoadTensor<int32_t>(const uint32_t tensor_id, const heap::Type type);
    template Tensor<float> LoadTensor<float>(const uint32_t tensor_id, const heap::Type type);

    template <typename dtype>
    TensorView<dtype> StreamTensor(const uint32_t tensor_id)
    {
        const TensorInfo &t = Info(tensor_id);
        assert(CheckTensorType<dtype>(t.type));
        return TensorView<dtype>(InfoShape(t), t.shift, t.quantized, tensor_id);
    }

    template TensorView<int8_t> StreamTensor<int8_t>(const uint32_t tensor_id);
    template TensorView<float> StreamTensor<float>(const uint32_t tensor_id);

    bool ReadRows(const uint32_t tensor_id, const uint32_t row0, const uint32_t n_rows, void *dst)
    {
        const TensorInfo &t = Info(tensor_id);
        const uint32_t row_bytes = (t.numel / t.shape[0]) * TypeBytes(t.type);
        assert(row0 + n_rows <= t.shape[0]);
        if (t.resident)
        {
            memcpy(dst, static_cast<const uint8_t *>(t.resident) + row0 * row_bytes, n_rows * row_bytes);
            return true;
        }
        return stt_model_read(t.data_offset + row0 * row_bytes, dst, n_rows * row_bytes);
    }

    const uint8_t *ResidentData(const uint32_t tensor_id)
    {
        return static_cast<const uint8_t *>(Info(tensor_id).resident);
    }

} // tlib::flash

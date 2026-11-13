// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib/inc/
// tlib_flash.h (Apache-2.0, Copyright 2026 lspr98): the model file is read
// from SD through the plugin host (port.h stt_model_read) instead of being
// memory-mapped from flash. Tensors can be copied once into resident memory
// (MakeResident; MapTensor views them; up to two blocks, AddResidentRegion);
// every other tensor is read when used (LoadTensor), or stays on SD for the
// tiled kernels to read row by row (StreamTensor, ReadRows).

#ifndef TLIB_FLASH_H_
#define TLIB_FLASH_H_

#include <cstddef>
#include <cstdint>

#include <tlib_tensor.h>
#include <tlib_heap.h>

namespace tlib::flash
{

    /**
     * Possible error codes when initialzing flash
     */
    enum class FlashError : uint8_t
    {
        None = 0U,
        MagicInvalid = 1U,
        TensorCountInvalid = 2U,
        FileSizeInvalid = 3U,
        MetaSectorMountFailed = 4U,    // the file header could not be read
        TensorSectorMountFailed = 5U,  // a tensor header could not be read
        Uninitialized = 6U,
        ResidentFull = 7U              // the resident blocks are too small
    };

    /**
     * Reads the model file's header and every tensor's header. The tensor
     * table and the resident tensors live in `resident` (16-byte aligned).
     */
    FlashError Initialize(uint8_t *resident, const size_t resident_size);

    /**
     * A second block for resident tensors (16-byte aligned), after
     * Initialize: each MakeResident takes the first of the two blocks with
     * room for the tensor.
     */
    void AddResidentRegion(uint8_t *block, const size_t size);

    /**
     * Copies tensor `tensor_id` into resident memory (once).
     */
    FlashError MakeResident(const uint32_t tensor_id);

    /**
     * Bytes of the resident blocks in use.
     */
    size_t ResidentUsed(void);

    /**
     * Number of tensors in the model file.
     */
    uint32_t TensorCount(void);

    /**
     * A read-only view of a resident tensor (MakeResident).
     */
    template <typename dtype>
    TensorView<dtype> MapTensor(const uint32_t tensor_id);

    /**
     * Reads a tensor from the model file into the given heap (a copy of the
     * resident data for a resident tensor).
     */
    template <typename dtype>
    Tensor<dtype> LoadTensor(const uint32_t tensor_id, const heap::Type type = heap::Type::PSRAM);

    /**
     * A view of a tensor whose data stays on SD (TensorView::StreamId); the
     * tiled kernels read its rows (ReadRows).
     */
    template <typename dtype>
    TensorView<dtype> StreamTensor(const uint32_t tensor_id);

    /**
     * Reads rows [row0, row0 + n_rows) of a tensor into dst. A row is
     * Numel / Shape(0) elements.
     */
    bool ReadRows(const uint32_t tensor_id, const uint32_t row0, const uint32_t n_rows, void *dst);

    /**
     * A resident tensor's data (16-byte aligned), nullptr for one that is
     * not resident.
     */
    const uint8_t *ResidentData(const uint32_t tensor_id);

} // tlib::flash

#endif

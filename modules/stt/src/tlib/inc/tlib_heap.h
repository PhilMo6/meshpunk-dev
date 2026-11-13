// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib/inc/
// tlib_heap.h (Apache-2.0, Copyright 2026 lspr98): one heap in a PSRAM block
// the plugin is granted serves both locations (the kernels' SRAM operand goes
// through the internal tile instead, tlib_linear_impl); the Summary printer
// is gone; usage figures for the plugin's status.

#ifndef TLIB_HEAP_H_
#define TLIB_HEAP_H_

#include <cstddef>
#include <cstdint>

namespace tlib::heap
{

    /**
     * Possible heap type/location. SRAM and PSRAM requests are both served
     * from the one PSRAM heap.
     */
    enum class Type : uint8_t
    {
        SRAM = 0U,
        PSRAM = 1U,
        UNKNOWN = 2U
    };

    // Default location for tensors
    inline constexpr const heap::Type kDefaultLocation{heap::Type::PSRAM};

    /**
     * Initializes the heap in the given buffer (16-byte aligned).
     */
    void Initialize(uint8_t *buffer, const size_t size);

    /**
     * Allocates size bytes (16-byte aligned). heap_type is accepted for the
     * upstream call sites; every request comes from the one heap. nullptr
     * (logged) when the heap is full.
     */
    void *Allocate(const Type heap_type, size_t size);

    /*
     * Frees a previously allocated buffer.
     */
    void Free(void *buffer);

    /*
     * PSRAM when the buffer is in the heap, UNKNOWN otherwise.
     */
    Type Locate(const void *buffer);

    /**
     * Bytes from the heap start to the end of the highest live block, the
     * highest that has reached since the last ResetMaxUsage(), and the heap
     * size.
     */
    size_t Usage(void);
    size_t MaxUsage(void);
    size_t Capacity(void);
    void ResetMaxUsage(void);

    /**
     * Number of live blocks.
     */
    size_t BlockCount(void);

} // tlib::heap

#endif

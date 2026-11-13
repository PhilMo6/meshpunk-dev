// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib/src/
// tlib_heap.cpp (Apache-2.0, Copyright 2026 lspr98): the block list is a
// fixed array kept in address order instead of std::list; one heap serves
// both locations; the Summary printer is gone. The allocation policy is
// upstream's: first fit in address order, then after the last block.

#include "tlib_heap.h"

#include <cassert>

#include <config.h>
#include <port.h>

namespace tlib::heap
{

    namespace utils
    {

        constexpr size_t AlignSize(const size_t size)
        {
            size_t remainder = size % kHeapAlignment;
            if (remainder == 0)
            {
                return size;
            }
            else
            {
                return size + (kHeapAlignment - remainder);
            }
        }

    } // namespace utils

    struct Block
    {
        uint8_t *buffer;
        size_t size;
    };

    static uint8_t *buffer_{nullptr};
    static size_t buffer_size_{0};
    static Block blocks_[kHeapMaxBlocks];
    static size_t n_blocks_{0};
    static size_t max_usage_{0};

    static size_t EndOffset(const Block &b)
    {
        return static_cast<size_t>(b.buffer + b.size - buffer_);
    }

    static void Insert(const size_t at, uint8_t *p, const size_t size)
    {
        for (size_t j = n_blocks_; j > at; j--)
        {
            blocks_[j] = blocks_[j - 1];
        }
        blocks_[at] = Block{p, size};
        n_blocks_++;
        const size_t end = EndOffset(blocks_[at]);
        if (end > max_usage_)
        {
            max_usage_ = end;
        }
    }

    void Initialize(uint8_t *buffer, const size_t size)
    {
        assert(reinterpret_cast<uintptr_t>(buffer) % kHeapAlignment == 0);
        buffer_ = buffer;
        buffer_size_ = size;
        n_blocks_ = 0;
        max_usage_ = 0;
    }

    void *Allocate(const Type heap_type, size_t size)
    {
        (void)heap_type;
        size = utils::AlignSize(size);

        if (size > buffer_size_)
        {
            stt_log("FAIL: tensor heap: %u bytes asked, the heap is %u", (unsigned)size, (unsigned)buffer_size_);
            return nullptr;
        }
        if (n_blocks_ >= kHeapMaxBlocks)
        {
            stt_log("FAIL: tensor heap: all %u blocks live", (unsigned)kHeapMaxBlocks);
            return nullptr;
        }

        // First gap between live blocks (address order) that fits.
        uint8_t *prev_end = buffer_;
        for (size_t i = 0; i < n_blocks_; i++)
        {
            if (static_cast<size_t>(blocks_[i].buffer - prev_end) >= size)
            {
                Insert(i, prev_end, size);
                return prev_end;
            }
            prev_end = blocks_[i].buffer + blocks_[i].size;
        }

        // After the last block.
        if (static_cast<size_t>((buffer_ + buffer_size_) - prev_end) >= size)
        {
            Insert(n_blocks_, prev_end, size);
            return prev_end;
        }

        stt_log("FAIL: tensor heap full: %u bytes asked, %u of %u in use in %u blocks",
                (unsigned)size, (unsigned)Usage(), (unsigned)buffer_size_, (unsigned)n_blocks_);
        return nullptr;
    }

    void Free(void *buffer)
    {
        for (size_t i = 0; i < n_blocks_; i++)
        {
            if (blocks_[i].buffer == buffer)
            {
                for (size_t j = i; j + 1 < n_blocks_; j++)
                {
                    blocks_[j] = blocks_[j + 1];
                }
                n_blocks_--;
                return;
            }
        }
        assert(buffer == nullptr && "tensor heap: free of an unknown block");
    }

    Type Locate(const void *buffer)
    {
        const uint8_t *p = static_cast<const uint8_t *>(buffer);
        return (p >= buffer_ && p < buffer_ + buffer_size_) ? Type::PSRAM : Type::UNKNOWN;
    }

    size_t Usage(void)
    {
        return n_blocks_ ? EndOffset(blocks_[n_blocks_ - 1]) : 0;
    }

    size_t MaxUsage(void) { return max_usage_; }

    size_t Capacity(void) { return buffer_size_; }

    void ResetMaxUsage(void) { max_usage_ = Usage(); }

    size_t BlockCount(void) { return n_blocks_; }

} // tlib::heap

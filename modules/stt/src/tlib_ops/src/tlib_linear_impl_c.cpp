// tlib_linear_impl_c.cpp — tlib_linear_impl.h for the builds without the
// ESP32-S3 assembly (build.ps1 -Variant s3c and rv32): the portable kernels
// of tlib_kernels_c.h (their int8 kernels need k > 0) over B in blocks.
// Unlike the S3 path (tlib_linear_tiled.h: every block copied into the SRAM
// tile, its output into a heap buffer and then into y's columns), B is
// read where it is — in memory, or a resident model tensor — and only a
// tensor on SD is read into the tile; the kernels write each block's
// columns straight into y (y's rows m apart).

#include "tlib_linear_impl.h"
#include "tlib_kernels_c.h"

#include <cassert>
#include <cstddef>

#include <config.h>
#include <port.h>
#include <profile.h>
#include <tlib_flash.h>

namespace tlib::ops::impl
{
    namespace
    {
        /**
         * Runs `call(b_block, c_block, m0, mt)` for each block of B rows
         * [m0, m0 + mt), then stt_tile_done(): a block is the rows that fit
         * kTileBytes (each row of A re-reads the block), c_block = c + m0
         * (or nullptr). B = b in memory, or the rows of model tensor
         * b_stream: its resident copy, or read from SD into the tile.
         */
        template <typename Call>
        void Blocks(const void *b, const int32_t b_stream, const uint32_t b_row_bytes, const int32_t *c,
                    const uint32_t m, Call &&call)
        {
            assert(b_row_bytes > 0 && b_row_bytes <= kTileBytes);
            assert(b != nullptr || b_stream >= 0);
            const uint8_t *b_mem = static_cast<const uint8_t *>(b);
            if (b_stream >= 0)
            {
                b_mem = flash::ResidentData(static_cast<uint32_t>(b_stream));
            }
            uint32_t rows = kTileBytes / b_row_bytes;
            if (rows > m)
            {
                rows = m;
            }
            for (uint32_t m0 = 0; m0 < m; m0 += rows)
            {
                const uint32_t mt = (m - m0 < rows) ? (m - m0) : rows;
                const uint8_t *bt;
                if (b_mem != nullptr)
                {
                    bt = b_mem + static_cast<size_t>(m0) * b_row_bytes;
                }
                else
                {
                    STT_PROF(kTileCopy);
                    uint8_t *tile = stt_tile();
                    const bool ok = flash::ReadRows(static_cast<uint32_t>(b_stream), m0, mt, tile);
                    assert(ok && "weight rows read failed");
                    (void)ok;
                    bt = tile;
                }
                call(bt, c != nullptr ? c + m0 : nullptr, m0, mt);
                stt_tile_done();
            }
        }
    } // namespace

    void linear_b_relu_impl_lshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t lshift, const int32_t b_stream)
    {
        assert(k > 0 && k % 16 == 0);
        const uint32_t k16 = k / 16;
        Blocks(b, b_stream, k, c, m, [&](const uint8_t *bt, const int32_t *ct, const uint32_t m0, const uint32_t mt) {
            kernels_c::linear_b_relu_lshift(a, reinterpret_cast<const int8_t *>(bt), ct, y + m0, k16, n, mt, m, lshift);
        });
    }

    void linear_b_relu_impl_rshift(const int8_t *a, const int8_t *b, const int32_t *c, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t rshift, const int32_t b_stream)
    {
        assert(k > 0 && k % 16 == 0);
        const uint32_t k16 = k / 16;
        Blocks(b, b_stream, k, c, m, [&](const uint8_t *bt, const int32_t *ct, const uint32_t m0, const uint32_t mt) {
            kernels_c::linear_b_relu_rshift(a, reinterpret_cast<const int8_t *>(bt), ct, y + m0, k16, n, mt, m, rshift);
        });
    }

    void linear_relu_impl_lshift(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t lshift, const int32_t b_stream)
    {
        assert(k > 0 && k % 16 == 0);
        const uint32_t k16 = k / 16;
        Blocks(b, b_stream, k, nullptr, m, [&](const uint8_t *bt, const int32_t *, const uint32_t m0, const uint32_t mt) {
            kernels_c::linear_relu_lshift(a, reinterpret_cast<const int8_t *>(bt), y + m0, k16, n, mt, m, lshift);
        });
    }

    void linear_relu_impl_rshift(const int8_t *a, const int8_t *b, int8_t *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t rshift, const int32_t b_stream)
    {
        assert(k > 0 && k % 16 == 0);
        const uint32_t k16 = k / 16;
        Blocks(b, b_stream, k, nullptr, m, [&](const uint8_t *bt, const int32_t *, const uint32_t m0, const uint32_t mt) {
            kernels_c::linear_relu_rshift(a, reinterpret_cast<const int8_t *>(bt), y + m0, k16, n, mt, m, rshift);
        });
    }

    void linear_deq_impl(const int8_t *a, const int8_t *b, float *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t shift, const int32_t b_stream)
    {
        assert(shift < 15);
        assert(k > 0 && k % 16 == 0);
        const uint32_t k16 = k / 16;
        Blocks(b, b_stream, k, nullptr, m, [&](const uint8_t *bt, const int32_t *, const uint32_t m0, const uint32_t mt) {
            kernels_c::linear_deq(a, reinterpret_cast<const int8_t *>(bt), y + m0, k16, n, mt, m, shift);
        });
    }

    void linear_b_deq_impl(const int8_t *a, const int8_t *b, const int32_t *c, float *y, const uint32_t k, const uint32_t n, const uint32_t m, const uint8_t shift, const int32_t b_stream)
    {
        assert(shift < 15);
        assert(k > 0 && k % 16 == 0);
        const uint32_t k16 = k / 16;
        Blocks(b, b_stream, k, c, m, [&](const uint8_t *bt, const int32_t *ct, const uint32_t m0, const uint32_t mt) {
            kernels_c::linear_b_deq(a, reinterpret_cast<const int8_t *>(bt), ct, y + m0, k16, n, mt, m, shift);
        });
    }

    void linear_impl(const float *a, const float *b, float *y, const uint32_t k, const uint32_t n, const uint32_t m, const int32_t b_stream)
    {
        assert(k % 4 == 0);
        const uint32_t k4 = k / 4;
        Blocks(b, b_stream, k * sizeof(float), nullptr, m, [&](const uint8_t *bt, const int32_t *, const uint32_t m0, const uint32_t mt) {
            kernels_c::linear_f32(a, reinterpret_cast<const float *>(bt), y + m0, k4, n, mt, m);
        });
    }

} // tlib::ops::impl

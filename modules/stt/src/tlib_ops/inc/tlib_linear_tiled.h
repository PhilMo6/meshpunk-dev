// tlib_linear_tiled.h — the block driver the S3 assembly kernels run under
// (tlib_linear_impl_esp32s3.cpp; the portable kernels have their own,
// tlib_linear_impl_c.cpp Blocks). From
// tlib_linear_impl_esp32s3.cpp (Modified for Meshpunk from conformer-stt-s3
// libs/tlib_ops/src_esp32s3/tlib_linear_impl_esp32s3.cpp, Apache-2.0,
// Copyright 2026 lspr98), unchanged but for its profile stages (profile.h).

#ifndef TLIB_LINEAR_TILED_H_
#define TLIB_LINEAR_TILED_H_

#include <cassert>
#include <cstring>

#include <config.h>
#include <port.h>
#include <profile.h>
#include <tlib_flash.h>
#include <tlib_heap.h>

namespace tlib::ops::impl
{
    namespace
    {
        /**
         * Runs `call(b_tile, c_tile, y_out, mt)` once per block of B rows,
         * then stt_tile_done(). B = b in memory, or the rows of model tensor
         * b_stream; row = k elements of b_row_bytes. With one block (all of
         * B fits the tile) the kernel writes y directly; otherwise each
         * block's [n x mt] output goes to a heap buffer and is scattered
         * into y's columns.
         */
        template <typename T_out, typename Call>
        void Tiled(const void *b, const int32_t b_stream, const uint32_t b_row_bytes, const int32_t *c,
                   T_out *y, const uint32_t n, const uint32_t m, Call &&call)
        {
            uint8_t *tile = stt_tile();
            int32_t *c_tile = stt_bias_tile();
            assert(b_row_bytes > 0 && b_row_bytes <= kTileBytes);
            assert(b != nullptr || b_stream >= 0);

            uint32_t rows = kTileBytes / b_row_bytes;
            if (c != nullptr && rows > kBiasTileBytes / sizeof(int32_t))
            {
                rows = kBiasTileBytes / sizeof(int32_t);
            }
            if (rows > m)
            {
                rows = m;
            }

            T_out *y_block = nullptr;
            if (rows < m)
            {
                y_block = static_cast<T_out *>(heap::Allocate(heap::Type::PSRAM, static_cast<size_t>(n) * rows * sizeof(T_out)));
                assert(y_block != nullptr);
            }

            for (uint32_t m0 = 0; m0 < m; m0 += rows)
            {
                const uint32_t mt = (m - m0 < rows) ? (m - m0) : rows;
                const int32_t *c_cur = nullptr;
                {
                    STT_PROF(kTileCopy);
                    if (b_stream >= 0)
                    {
                        const bool ok = flash::ReadRows(static_cast<uint32_t>(b_stream), m0, mt, tile);
                        assert(ok && "weight rows read failed");
                        (void)ok;
                    }
                    else
                    {
                        memcpy(tile, static_cast<const uint8_t *>(b) + static_cast<size_t>(m0) * b_row_bytes, static_cast<size_t>(mt) * b_row_bytes);
                    }
                    if (c != nullptr)
                    {
                        memcpy(c_tile, c + m0, mt * sizeof(int32_t));
                        c_cur = c_tile;
                    }
                }
                if (mt == m)
                {
                    call(tile, c_cur, y, mt);
                }
                else
                {
                    call(tile, c_cur, y_block, mt);
                    STT_PROF(kTileCopy);
                    for (uint32_t r = 0; r < n; r++)
                    {
                        memcpy(y + static_cast<size_t>(r) * m + m0, y_block + static_cast<size_t>(r) * mt, mt * sizeof(T_out));
                    }
                }
                stt_tile_done();
            }

            if (y_block != nullptr)
            {
                heap::Free(y_block);
            }
        }
    } // namespace
} // tlib::ops::impl

#endif

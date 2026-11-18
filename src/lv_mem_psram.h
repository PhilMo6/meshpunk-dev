/**
 * @file lv_mem_psram.h
 *
 * LVGL's allocator (lv_mem_psram.c): the small-block pool.
 *
 * lv_malloc requests of up to LV_MEM_SMALL_MAX bytes are served from the
 * pool, a heap of its own (multi_heap) in one PSRAM block; larger requests,
 * and the ones the pool has no room for, from the PSRAM heap.
 */

#ifndef LV_MEM_PSRAM_H
#define LV_MEM_PSRAM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV_MEM_SMALL_POOL_BYTES (256u * 1024u)
#define LV_MEM_SMALL_MAX        1024u

/**
 * Allocate the pool's block and set the pool up. Once, before lv_init().
 * @return false when PSRAM has no block of LV_MEM_SMALL_POOL_BYTES; every
 *         request then comes from the PSRAM heap.
 */
bool lv_mem_small_pool_create(void);

/**
 * The pool's size in bytes: LV_MEM_SMALL_POOL_BYTES, or 0 without a pool.
 */
uint32_t lv_mem_small_pool_size(void);

/**
 * @param used     bytes of the pool taken now (its bookkeeping included)
 * @param peak     the most that were taken at once since the pool was made
 * @param spilled  requests of up to LV_MEM_SMALL_MAX bytes the pool had no
 *                 room for (served from the PSRAM heap), since boot
 * All 0 without a pool.
 */
void lv_mem_small_stats(uint32_t * used, uint32_t * peak, uint32_t * spilled);

#ifdef __cplusplus
}
#endif

#endif /* LV_MEM_PSRAM_H */

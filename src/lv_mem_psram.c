/**
 * @file lv_mem_psram.c
 *
 * PSRAM-backed memory allocator for LVGL.
 *
 * Routes all LVGL widget/style/object allocations to ESP32-S3 PSRAM,
 * freeing internal SRAM for BLE/WiFi/DMA.
 *
 * The display draw buffers (buf1, buf2) are separately allocated with
 * ps_malloc() in setupLvgl() -- this file handles everything else
 * LVGL allocates internally (widgets, styles, groups, etc.).
 *
 * Two sources (lv_mem_psram.h). Requests of up to LV_MEM_SMALL_MAX bytes
 * come from the small-block pool: a heap of its own (multi_heap) in one
 * PSRAM block that setupLvgl() has allocated before lv_init(), so the block
 * sits below everything allocated later. Larger requests, and the small ones
 * the pool has no room for (counted: lv_mem_small_stats), come from the
 * PSRAM heap. A block is freed and resized by where its address lies. The
 * pool takes its own lock (s_small_mux): lv_malloc is called from the LVGL
 * thread and from the tile worker's PNG decode on core 1.
 */

#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "lv_mem_psram.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <multi_heap.h>
#include <string.h>

/**********************
 *  STATIC VARIABLES
 **********************/

static multi_heap_handle_t s_small = NULL;
static uintptr_t s_small_base = 0;          /* the pool's block */
static portMUX_TYPE s_small_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_small_spilled = 0;

/**********************
 *  STATIC FUNCTIONS
 **********************/

static inline bool in_small(const void * p)
{
    return s_small_base != 0 && (uintptr_t)p >= s_small_base &&
           (uintptr_t)p < s_small_base + LV_MEM_SMALL_POOL_BYTES;
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

bool lv_mem_small_pool_create(void)
{
    if(s_small != NULL) return true;
    void * blk = heap_caps_malloc(LV_MEM_SMALL_POOL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(blk == NULL) return false;
    multi_heap_handle_t heap = multi_heap_register(blk, LV_MEM_SMALL_POOL_BYTES);
    if(heap == NULL) {
        heap_caps_free(blk);
        return false;
    }
    multi_heap_set_lock(heap, &s_small_mux);
    s_small_base = (uintptr_t)blk;
    s_small = heap;
    return true;
}

uint32_t lv_mem_small_pool_size(void)
{
    return s_small != NULL ? LV_MEM_SMALL_POOL_BYTES : 0;
}

void lv_mem_small_stats(uint32_t * used, uint32_t * peak, uint32_t * spilled)
{
    if(s_small == NULL) {
        *used = 0;
        *peak = 0;
        *spilled = 0;
        return;
    }
    *used = LV_MEM_SMALL_POOL_BYTES - (uint32_t)multi_heap_free_size(s_small);
    *peak = LV_MEM_SMALL_POOL_BYTES - (uint32_t)multi_heap_minimum_free_size(s_small);
    *spilled = s_small_spilled;
}

void lv_mem_init(void)
{
    /* PSRAM is initialized by the ESP-IDF startup code; nothing to do. */
}

void lv_mem_deinit(void)
{
    /* Nothing to tear down. */
}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;  /* Not supported -- we use the global PSRAM heap. */
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void * lv_malloc_core(size_t size)
{
    if(s_small != NULL && size <= LV_MEM_SMALL_MAX) {
        void * p = multi_heap_malloc(s_small, size);
        if(p != NULL) return p;
        s_small_spilled++;
    }
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* A PSRAM-heap block stays in the PSRAM heap whatever its new size. A pool
 * block stays in the pool while its new size is one the pool serves and has
 * room for; otherwise its contents move to a new PSRAM-heap block. */
void * lv_realloc_core(void * p, size_t new_size)
{
    if(p == NULL) return lv_malloc_core(new_size);
    if(!in_small(p)) return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if(new_size <= LV_MEM_SMALL_MAX) {
        void * q = multi_heap_realloc(s_small, p, new_size);
        if(q != NULL) return q;
        s_small_spilled++;
    }
    void * q = heap_caps_malloc(new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(q == NULL) return NULL;
    size_t old_size = multi_heap_get_allocated_size(s_small, p);
    memcpy(q, p, old_size < new_size ? old_size : new_size);
    multi_heap_free(s_small, p);
    return q;
}

void lv_free_core(void * p)
{
    if(in_small(p)) multi_heap_free(s_small, p);
    else heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    memset(mon_p, 0, sizeof(lv_mem_monitor_t));

    mon_p->total_size        = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon_p->free_size         = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    if(mon_p->total_size > 0) {
        mon_p->used_pct = 100 - (uint8_t)((uint64_t)100U * mon_p->free_size / mon_p->total_size);
    }

    if(mon_p->free_size > 0) {
        mon_p->frag_pct = 100 - (uint8_t)((uint64_t)100U * mon_p->free_biggest_size / mon_p->free_size);
    }
}

lv_result_t lv_mem_test_core(void)
{
    /* Basic sanity: try a small allocation and free it. */
    void * p = heap_caps_malloc(64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(p == NULL) return LV_RESULT_INVALID;
    heap_caps_free(p);
    return LV_RESULT_OK;
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */

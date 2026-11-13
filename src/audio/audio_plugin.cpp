// audio_plugin.cpp — loader for audio plugin modules (audio_plugin_abi.h):
// one plugin at a time, loaded beside Lua by an app, its worker on core 1
// below the mesh task, its memory granted at load and freed at unload.

#include "audio_plugin.h"
#include "audio_plugin_abi.h"
#include "mic_dev.h"
#include "mic_lua.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#if __has_include(<esp_memory_utils.h>)
#include <esp_memory_utils.h>       // esp_ptr_dma_capable (ESP-IDF 5)
#else
#include <soc/soc_memory_types.h>   // esp_ptr_dma_capable (ESP-IDF 4.4)
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include <lauxlib.h>
}

#include "elf_loader.h"
#include "../elf_host.h"        // elf_arch_path
#include "../meshpunk_fs.h"     // meshpunk_read_all
#include "../meshpunk_sync.h"   // SLog, sd_spi_take

extern void sd_spi_release();   // main.cpp

// Symbols a plugin may import. Everything else reaches the firmware through
// the AudioPlugHostApi vtable. APPEND-ONLY once plugins ship: a fielded
// plugin resolves against this table; the plugin build's UND audit is what
// grows it.
static const elf_symbol_t audioplug_exports[] = {
    { "memcpy",    (void*)memcpy },
    { "memset",    (void*)memset },
    { "memcmp",    (void*)memcmp },
    { "memmove",   (void*)memmove },
    { "strlen",    (void*)strlen },
    { "strcmp",    (void*)strcmp },
    { "strncmp",   (void*)strncmp },
    { "strchr",    (void*)strchr },
    { "strrchr",   (void*)strrchr },
    { "strstr",    (void*)strstr },
    { "strncpy",   (void*)strncpy },
    { "strtol",    (void*)strtol },
    { "snprintf",  (void*)snprintf },
    { "vsnprintf", (void*)vsnprintf },
    // libm, single precision.
    { "expf",      (void*)(float (*)(float))expf },
    { "logf",      (void*)(float (*)(float))logf },
    { "sqrtf",     (void*)(float (*)(float))sqrtf },
    { "sinf",      (void*)(float (*)(float))sinf },
    { "cosf",      (void*)(float (*)(float))cosf },
    { "floorf",    (void*)(float (*)(float))floorf },
    { "powf",      (void*)(float (*)(float, float))powf },
    { "tanhf",     (void*)(float (*)(float))tanhf },
    ELF_SYMBOL_END
};

// ── Session state ────────────────────────────────────────────────────────────

#define CMD_SLOTS    4
#define RESULT_SLOTS 8

struct PlugCmd    { uint16_t len; char data[AUDIOPLUG_CMD_MAX]; };
struct PlugResult { uint16_t len; char data[AUDIOPLUG_RESULT_MAX]; };

struct PlugFile {
    int      fd;      // -1 = slot free
    bool     sd;      // the SD bus lock brackets every call
    uint32_t size;
};

struct PlugSession {
    elf_module_t*        mod;
    const AudioPlugDesc* desc;
    void*                blocks[AUDIOPLUG_MAX_BLOCKS];      // 16-byte aligned, inside block_mem
    void*                block_mem[AUDIOPLUG_MAX_BLOCKS];   // heap_caps_malloc's pointers
    TaskHandle_t         task;
    volatile bool        stop;
    volatile bool        worker_done;
    SemaphoreHandle_t    qlock;       // the two queues below
    PlugCmd*             cmds;        // PSRAM, CMD_SLOTS
    uint8_t              cmd_head, cmd_count;
    PlugResult*          results;     // PSRAM, RESULT_SLOTS
    uint8_t              res_head, res_count;
    PlugFile             files[AUDIOPLUG_MAX_FILES];
    char                 name[24];
};

static PlugSession* s_sess = nullptr;

// ── Host API ─────────────────────────────────────────────────────────────────

static void api_log(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    SLog.printf("[%s] %s\n", s_sess ? s_sess->name : "aplug", line);
}

static uint32_t api_ticks_ms(void)       { return millis(); }
static void     api_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }

static bool api_should_stop(void) { return !s_sess || s_sess->stop; }

static void api_wait(uint32_t timeout_ms) {
    if (!s_sess || s_sess->stop) return;
    if (s_sess->cmd_count) return;    // a command is already waiting
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms ? timeout_ms : 1));
}

static bool api_mic_session_open(void) {
    return mic_lua_owner_active() && mic_capture_running();
}

static size_t api_mic_read(int16_t* out, size_t max_samples) {
    if (!out || !max_samples || !mic_capture_running()) return 0;
    return mic_capture_read(out, max_samples);
}

static uint32_t api_mic_dropped(void) { return mic_capture_dropped(); }

static uint32_t api_command(char* buf, uint32_t cap) {
    PlugSession* s = s_sess;
    if (!s || !buf || !cap) return 0;
    uint32_t n = 0;
    xSemaphoreTake(s->qlock, portMAX_DELAY);
    if (s->cmd_count) {
        const PlugCmd& c = s->cmds[s->cmd_head];
        n = c.len < cap ? c.len : cap;
        memcpy(buf, c.data, n);
        s->cmd_head = (uint8_t)((s->cmd_head + 1) % CMD_SLOTS);
        s->cmd_count--;
    }
    xSemaphoreGive(s->qlock);
    return n;
}

static bool api_publish(const char* data, uint32_t len) {
    PlugSession* s = s_sess;
    if (!s || !data || len > AUDIOPLUG_RESULT_MAX) return false;
    bool ok = false;
    xSemaphoreTake(s->qlock, portMAX_DELAY);
    if (s->res_count < RESULT_SLOTS) {
        PlugResult& r = s->results[(s->res_head + s->res_count) % RESULT_SLOTS];
        r.len = (uint16_t)len;
        memcpy(r.data, data, len);
        s->res_count++;
        ok = true;
    }
    xSemaphoreGive(s->qlock);
    return ok;
}

// Drive-prefixed path -> VFS path. false for any other prefix.
static bool vfs_path(const char* path, char* out, size_t cap, bool* sd) {
    if (!path) return false;
    if (strncmp(path, "S:/", 3) == 0)      { *sd = true;  snprintf(out, cap, "/sd/%s", path + 3); }
    else if (strncmp(path, "L:/", 3) == 0) { *sd = false; snprintf(out, cap, "/littlefs/%s", path + 3); }
    else return false;
    return true;
}

static PlugFile* file_slot(int h) {
    if (!s_sess || h < 0 || h >= AUDIOPLUG_MAX_FILES) return nullptr;
    PlugFile* f = &s_sess->files[h];
    return f->fd >= 0 ? f : nullptr;
}

static int api_file_open(const char* path) {
    PlugSession* s = s_sess;
    if (!s) return -1;
    int h = -1;
    for (int i = 0; i < AUDIOPLUG_MAX_FILES; i++) {
        if (s->files[i].fd < 0) { h = i; break; }
    }
    char p[200];
    bool sd = false;
    if (h < 0 || !vfs_path(path, p, sizeof(p), &sd)) return -1;
    if (sd) sd_spi_take();
    int fd = open(p, O_RDONLY);
    off_t size = -1;
    if (fd >= 0) {
        size = lseek(fd, 0, SEEK_END);
        if (size < 0 || lseek(fd, 0, SEEK_SET) != 0) { close(fd); fd = -1; }
    }
    if (sd) sd_spi_release();
    if (fd < 0) return -1;
    s->files[h].fd   = fd;
    s->files[h].sd   = sd;
    s->files[h].size = (uint32_t)size;
    return h;
}

static uint32_t api_file_size(int h) {
    PlugFile* f = file_slot(h);
    return f ? f->size : 0;
}

static bool api_file_seek(int h, uint32_t offset) {
    PlugFile* f = file_slot(h);
    if (!f) return false;
    if (f->sd) sd_spi_take();
    bool ok = lseek(f->fd, (off_t)offset, SEEK_SET) == (off_t)offset;
    if (f->sd) sd_spi_release();
    return ok;
}

// Outside the internal RAM the SD DMA reaches: PSRAM, mapped flash.
static bool not_dma_reachable(const void* p) {
    return !esp_ptr_dma_capable(p);
}

static int32_t api_file_read(int h, void* buf, uint32_t len) {
    PlugFile* f = file_slot(h);
    if (!f || !buf) return -1;
    if (not_dma_reachable(buf)) {
        api_log("FAIL: file_read into %p: the buffer must be internal RAM", buf);
        return -1;
    }
    if (f->sd) sd_spi_take();
    uint32_t got = 0;
    bool err = false;
    while (got < len) {
        ssize_t n = read(f->fd, (uint8_t*)buf + got, len - got);
        if (n < 0) { err = true; break; }
        if (n == 0) break;
        got += (uint32_t)n;
    }
    if (f->sd) sd_spi_release();
    return err ? -1 : (int32_t)got;
}

static void api_file_close(int h) {
    PlugFile* f = file_slot(h);
    if (!f) return;
    if (f->sd) sd_spi_take();
    close(f->fd);
    if (f->sd) sd_spi_release();
    f->fd = -1;
}

static const AudioPlugHostApi s_api = {
    AUDIO_PLUGIN_ABI_VERSION,
    &api_log,
    &api_ticks_ms,
    &api_delay_ms,
    &api_should_stop,
    &api_wait,
    &api_mic_session_open,
    &api_mic_read,
    &api_mic_dropped,
    &api_command,
    &api_publish,
    &api_file_open,
    &api_file_size,
    &api_file_seek,
    &api_file_read,
    &api_file_close,
};

// ── Load / unload ────────────────────────────────────────────────────────────

// Plugin segments: PSRAM only. A plugin that does not fit fails its load
// instead of taking internal RAM.
static void* seg_alloc(size_t size, void*) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void  seg_free(void* p, void*)      { heap_caps_free(p); }

static void worker_tramp(void* p) {
    PlugSession* s = (PlugSession*)p;
    s->desc->worker(&s_api);
    s->worker_done = true;
    vTaskSuspend(nullptr);   // deleted by unload
}

static bool desc_valid(const AudioPlugDesc* d) {
    return d && d->abi == AUDIO_PLUGIN_ABI_VERSION && d->name && d->name[0] &&
           d->init && d->worker && d->status && d->deinit && d->needs.worker_stack >= 2048;
}

// Free everything a session holds except the worker task (gone or never made).
static void session_free(PlugSession* s) {
    for (int i = 0; i < AUDIOPLUG_MAX_FILES; i++) {
        if (s->files[i].fd >= 0) {
            SLog.printf("[APLUG] %s left file %d open; closed\n", s->name, i);
            if (s->files[i].sd) sd_spi_take();
            close(s->files[i].fd);
            if (s->files[i].sd) sd_spi_release();
        }
    }
    for (int i = 0; i < AUDIOPLUG_MAX_BLOCKS; i++) {
        if (s->block_mem[i]) heap_caps_free(s->block_mem[i]);
    }
    if (s->cmds) heap_caps_free(s->cmds);
    if (s->results) heap_caps_free(s->results);
    if (s->qlock) vSemaphoreDelete(s->qlock);
    if (s->mod) elf_unload(s->mod);
    heap_caps_free(s);
}

static const char* kind_name(uint8_t kind) {
    return kind == AUDIOPLUG_MEM_INTERNAL ? "internal RAM" : "PSRAM";
}

// Load `path`; on failure the reason goes into err.
static bool plugin_load(const char* path, char* err, size_t errcap) {
    char apath[256];
    elf_arch_path(path, apath, sizeof(apath));
    if (!apath[0]) { snprintf(err, errcap, "path too long"); return false; }

    uint32_t size = 0;
    void* buf = meshpunk_read_all(apath, &size);
    if (!buf) { snprintf(err, errcap, "cannot read %s", apath); return false; }

    PlugSession* s = (PlugSession*)heap_caps_calloc(1, sizeof(PlugSession), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) { heap_caps_free(buf); snprintf(err, errcap, "no PSRAM for the session"); return false; }
    for (int i = 0; i < AUDIOPLUG_MAX_FILES; i++) s->files[i].fd = -1;

    s->mod = elf_load_ex(buf, size, audioplug_exports, seg_alloc, seg_free, nullptr);
    heap_caps_free(buf);
    if (!s->mod) {
        session_free(s);
        snprintf(err, errcap, "ELF load failed: %s (console has the reason)", apath);
        return false;
    }
    const AudioPlugDesc* d = (const AudioPlugDesc*)elf_lookup(s->mod, "audioplug_ops");
    if (!desc_valid(d)) {
        session_free(s);
        snprintf(err, errcap, "%s: no valid audioplug_ops (ABI %d)", apath, AUDIO_PLUGIN_ABI_VERSION);
        return false;
    }
    s->desc = d;
    strncpy(s->name, d->name, sizeof(s->name) - 1);
    elf_run_ctors(s->mod);

    s->qlock   = xSemaphoreCreateMutex();
    s->cmds    = (PlugCmd*)heap_caps_calloc(CMD_SLOTS, sizeof(PlugCmd), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->results = (PlugResult*)heap_caps_calloc(RESULT_SLOTS, sizeof(PlugResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->qlock || !s->cmds || !s->results) {
        session_free(s);
        snprintf(err, errcap, "no memory for the app channel");
        return false;
    }

    for (int i = 0; i < AUDIOPLUG_MAX_BLOCKS; i++) {
        const AudioPlugBlock& b = d->needs.blocks[i];
        if (!b.bytes) continue;
        uint32_t caps = (b.kind == AUDIOPLUG_MEM_INTERNAL)
                            ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
                            : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (b.kind != AUDIOPLUG_MEM_INTERNAL && b.kind != AUDIOPLUG_MEM_PSRAM) {
            snprintf(err, errcap, "%s: block %d has unknown kind %u", s->name, i, (unsigned)b.kind);
            session_free(s);
            return false;
        }
        // b.bytes + 15 from heap_caps_malloc, the block 16-byte aligned
        // inside it, so the blocks lie end to end and free into one hole.
        // heap_caps_aligned_alloc (TLSF memalign) frees a small gap block
        // in front of each block; small allocations fill those gaps while
        // the plugin runs, after unload each block is a hole of exactly its
        // own size, and an aligned request of that size cannot use it
        // (memalign asks for the size plus the alignment and a gap): the
        // plugin could not load again.
        s->block_mem[i] = heap_caps_malloc(b.bytes + 15, caps);
        s->blocks[i] = s->block_mem[i]
                           ? (void*)(((uintptr_t)s->block_mem[i] + 15) & ~(uintptr_t)15)
                           : nullptr;
        if (!s->blocks[i]) {
            unsigned largest = (unsigned)heap_caps_get_largest_free_block(caps);
            snprintf(err, errcap, "%s needs %u KB of %s in one block; the largest free is %u KB",
                     s->name, (unsigned)(b.bytes / 1024), kind_name(b.kind), largest / 1024);
            session_free(s);
            return false;
        }
    }

    s_sess = s;   // the API's calls find the session from here on
    if (!d->init(&s_api, s->blocks)) {
        s_sess = nullptr;
        session_free(s);
        snprintf(err, errcap, "%s refused to start (console has the reason)", d->name);
        return false;
    }
    if (xTaskCreatePinnedToCore(worker_tramp, "audioplug", d->needs.worker_stack, s, 1,
                                &s->task, 1) != pdPASS) {
        d->deinit(&s_api);
        s_sess = nullptr;
        snprintf(err, errcap, "%s: no internal RAM for its %u KB worker stack",
                 s->name, (unsigned)(d->needs.worker_stack / 1024));
        session_free(s);
        return false;
    }

    uint32_t ts = 0, te = 0;
    elf_text_range(s->mod, &ts, &te);
    SLog.printf("[APLUG] loaded %s (%s), text %08X-%08X\n", s->name, apath, (unsigned)ts, (unsigned)te);
    return true;
}

// The worker is asked to stop and must return; a plugin that does not
// return within this long is a plugin bug, and freeing its memory under a
// running task would fault later and elsewhere.
#define WORKER_STOP_TIMEOUT_MS 30000

static void plugin_unload(const char* reason) {
    PlugSession* s = s_sess;
    if (!s) return;
    s->stop = true;
    xTaskNotifyGive(s->task);   // the task stays valid until vTaskDelete below
    uint32_t t0 = millis();
    while (!s->worker_done) {
        if (millis() - t0 > WORKER_STOP_TIMEOUT_MS) {
            SLog.printf("[APLUG] FAIL: %s worker still running %u ms after the stop request (%s)\n",
                        s->name, (unsigned)WORKER_STOP_TIMEOUT_MS, reason);
            abort();
        }
        delay(5);
    }
    // The task is suspended (worker_tramp): its stack's low-water mark is final.
    const unsigned stack_unused = (unsigned)uxTaskGetStackHighWaterMark(s->task);
    const unsigned stack_size = (unsigned)s->desc->needs.worker_stack;
    vTaskDelete(s->task);
    s->desc->deinit(&s_api);
    s_sess = nullptr;
    char name[sizeof(s->name)];
    memcpy(name, s->name, sizeof(name));
    session_free(s);
    SLog.printf("[APLUG] unloaded %s: %s (worker stack: %u of %u bytes never used)\n",
                name, reason, stack_unused, stack_size);
}

void audio_plugin_unload_all(const char* reason) { plugin_unload(reason); }

// ── Lua bindings ─────────────────────────────────────────────────────────────

static int l_load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (s_sess) {
        lua_pushnil(L);
        lua_pushfstring(L, "plugin %s is loaded", s_sess->name);
        return 2;
    }
    char err[160];
    if (!plugin_load(path, err, sizeof(err))) {
        SLog.printf("[APLUG] load failed: %s\n", err);
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_unload(lua_State* L) {
    (void)L;
    plugin_unload("unloaded by the app");
    return 0;
}

static int l_loaded(lua_State* L) {
    if (!s_sess) { lua_pushnil(L); return 1; }
    lua_pushstring(L, s_sess->name);
    lua_pushboolean(L, !s_sess->worker_done);
    return 2;
}

static int l_write(lua_State* L) {
    size_t len = 0;
    const char* cmd = luaL_checklstring(L, 1, &len);
    PlugSession* s = s_sess;
    if (!s) { lua_pushnil(L); lua_pushstring(L, "no plugin loaded"); return 2; }
    if (len > AUDIOPLUG_CMD_MAX) {
        lua_pushnil(L);
        lua_pushfstring(L, "command longer than %d bytes", AUDIOPLUG_CMD_MAX);
        return 2;
    }
    bool ok = false;
    xSemaphoreTake(s->qlock, portMAX_DELAY);
    if (s->cmd_count < CMD_SLOTS) {
        PlugCmd& c = s->cmds[(s->cmd_head + s->cmd_count) % CMD_SLOTS];
        c.len = (uint16_t)len;
        memcpy(c.data, cmd, len);
        s->cmd_count++;
        ok = true;
    }
    xSemaphoreGive(s->qlock);
    if (!ok) { lua_pushnil(L); lua_pushstring(L, "command queue full"); return 2; }
    xTaskNotifyGive(s->task);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_read(lua_State* L) {
    PlugSession* s = s_sess;
    if (!s) { lua_pushnil(L); return 1; }
    xSemaphoreTake(s->qlock, portMAX_DELAY);
    if (!s->res_count) {
        xSemaphoreGive(s->qlock);
        lua_pushnil(L);
        return 1;
    }
    // Copy out under the lock, then hand Lua its own string after.
    PlugResult r;
    memcpy(&r, &s->results[s->res_head], sizeof(r.len) + s->results[s->res_head].len);
    s->res_head = (uint8_t)((s->res_head + 1) % RESULT_SLOTS);
    s->res_count--;
    xSemaphoreGive(s->qlock);
    lua_pushlstring(L, r.data, r.len);
    return 1;
}

static int l_status(lua_State* L) {
    PlugSession* s = s_sess;
    if (!s) { lua_pushnil(L); return 1; }
    char line[AUDIOPLUG_STATUS_MAX];
    line[0] = 0;
    s->desc->status(line, sizeof(line));
    line[sizeof(line) - 1] = 0;
    lua_pushstring(L, line);
    return 1;
}

void audio_plugin_register_lua(lua_State* L) {
    lua_register(L, "_audio_plug_load",   l_load);
    lua_register(L, "_audio_plug_unload", l_unload);
    lua_register(L, "_audio_plug_loaded", l_loaded);
    lua_register(L, "_audio_plug_write",  l_write);
    lua_register(L, "_audio_plug_read",   l_read);
    lua_register(L, "_audio_plug_status", l_status);
}

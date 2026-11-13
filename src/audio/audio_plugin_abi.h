// audio_plugin_abi.h — the contract between the firmware and an audio plugin
// module ("<id>.audioplug.elf" on Xtensa, "<id>.audioplug.rv32.elf" on
// RISC-V): a position-independent ELF that an app loads beside Lua for a
// session (_audio_plug_load, audio_plugin.cpp). One plugin at a time.
//
// The module exports one symbol, `audioplug_ops` (a const AudioPlugDesc).
// The firmware grants the memory blocks its `needs` declares all at once
// before init() (a refusal names the block that failed), runs worker() on
// a firmware task on core 1 below the mesh task, and frees the blocks after
// worker() has returned and deinit() has run. Every other service comes
// through the AudioPlugHostApi vtable; the module imports only the symbols
// in audio_plugin.cpp's export table (libc string/memory, formatting, libm).
//
// Threads: init() and status() run on the Lua task (core 0); worker() runs
// on the plugin task (core 1) and is the only caller of the mic_* and file_*
// calls. command/publish are the app channel (Lua <-> worker).

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AUDIO_PLUGIN_ABI_VERSION 1

// Memory block kinds a plugin can ask for.
#define AUDIOPLUG_MEM_NONE     0
#define AUDIOPLUG_MEM_INTERNAL 1   // internal SRAM, DMA-capable (file_read targets)
#define AUDIOPLUG_MEM_PSRAM    2

#define AUDIOPLUG_MAX_BLOCKS   4
#define AUDIOPLUG_CMD_MAX      256   // bytes per app -> plugin command
#define AUDIOPLUG_RESULT_MAX   1024  // bytes per plugin -> app result
#define AUDIOPLUG_STATUS_MAX   96    // bytes of the status line, NUL included
#define AUDIOPLUG_MAX_FILES    4     // files open at once

typedef struct {
    uint32_t bytes;   // 0 = slot unused
    uint8_t  kind;    // AUDIOPLUG_MEM_*
} AudioPlugBlock;

typedef struct {
    AudioPlugBlock blocks[AUDIOPLUG_MAX_BLOCKS];   // 16-byte aligned when granted
    uint32_t worker_stack;                          // bytes, internal SRAM
} AudioPlugNeeds;

typedef struct AudioPlugHostApi {
    uint32_t abi;                                   // == AUDIO_PLUGIN_ABI_VERSION

    // Log one line to the firmware console, prefixed with the plugin's name.
    void     (*log)(const char* fmt, ...);
    uint32_t (*ticks_ms)(void);
    void     (*delay_ms)(uint32_t ms);

    // Worker: true once the firmware wants worker() to return (unload, Lua
    // teardown, standby). wait() blocks until a command is queued, the stop
    // flag is set, or timeout_ms passes.
    bool     (*should_stop)(void);
    void     (*wait)(uint32_t timeout_ms);

    // Microphone: the session an app opened with _mic_open (mic_lua.cpp).
    // mic_read drains up to max samples (16 kHz mono s16) from its ring;
    // 0 when no session is open or the ring is empty. mic_dropped = samples
    // the ring lost to overflow this session.
    bool     (*mic_session_open)(void);
    size_t   (*mic_read)(int16_t* out, size_t max_samples);
    uint32_t (*mic_dropped)(void);

    // App channel. command(): the oldest queued command from
    // _audio_plug_write, copied into buf (truncated to cap); returns its
    // length, 0 when none is queued. publish(): queue one result for
    // _audio_plug_read (len <= AUDIOPLUG_RESULT_MAX); false when the queue
    // is full.
    uint32_t (*command)(char* buf, uint32_t cap);
    bool     (*publish)(const char* data, uint32_t len);

    // Read-only files, "S:/..." or "L:/...". file_open returns a handle
    // 0..AUDIOPLUG_MAX_FILES-1, or -1. file_read's buf must be internal RAM
    // (an AUDIOPLUG_MEM_INTERNAL block): SD DMA cannot target PSRAM; a PSRAM
    // buf is refused with -1. file_read returns the bytes read (0 at end of
    // file), -1 on error.
    int      (*file_open)(const char* path);
    uint32_t (*file_size)(int h);
    bool     (*file_seek)(int h, uint32_t offset);
    int32_t  (*file_read)(int h, void* buf, uint32_t len);
    void     (*file_close)(int h);
} AudioPlugHostApi;

typedef struct {
    uint32_t       abi;     // == AUDIO_PLUGIN_ABI_VERSION
    const char*    name;    // short id for logs, e.g. "stt"
    AudioPlugNeeds needs;

    // Lua task. blocks[i] = the granted block for needs.blocks[i] (NULL for
    // unused slots). false = refuse the load (reason logged by the plugin).
    bool (*init)(const AudioPlugHostApi* api, void* const* blocks);
    // Plugin task (core 1, below the mesh task): returns when
    // api->should_stop() is true.
    void (*worker)(const AudioPlugHostApi* api);
    // Lua task: one status line, NUL-terminated, at most n bytes.
    void (*status)(char* out, uint32_t n);
    // Lua task, after worker() returned; the blocks are freed after it.
    void (*deinit)(const AudioPlugHostApi* api);
} AudioPlugDesc;

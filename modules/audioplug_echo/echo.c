// echo.c — test plugin for the audio plugin loader (src/audio/
// audio_plugin_abi.h): answers commands so a test script can exercise every
// host call. Built twice by build.ps1: echo.audioplug.elf, and with
// -DECHO_BIG_NEEDS big.audioplug.elf, which asks for an internal block no
// device has (the load must be refused, naming the block).
//
// Commands (results come back through _audio_plug_read):
//   ping          -> "pong"
//   read <path>   -> size, a 16-byte read into the internal block, the
//                    refused read into the PSRAM block, the first bytes
//   mic           -> samples read from the open mic session in 1 s, peak
//   busy          -> 3 s of computation that checks should_stop()

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/audio/audio_plugin_abi.h"

#ifdef ECHO_BIG_NEEDS
#define ECHO_INTERNAL_BYTES (200 * 1024)
#else
#define ECHO_INTERNAL_BYTES 4096
#endif
#define ECHO_PSRAM_BYTES (64 * 1024)

static const AudioPlugHostApi* s_api;
static uint8_t*                s_int;   // internal block
static uint8_t*                s_ps;    // PSRAM block
static volatile uint32_t       s_cmds;

static void publish_str(const char* s) { s_api->publish(s, (uint32_t)strlen(s)); }

static bool echo_init(const AudioPlugHostApi* api, void* const* blocks) {
    s_api = api;
    s_int = (uint8_t*)blocks[0];
    s_ps  = (uint8_t*)blocks[1];
    api->log("init: internal block %p (%u B), PSRAM block %p (%u B)",
             (void*)s_int, (unsigned)ECHO_INTERNAL_BYTES, (void*)s_ps, (unsigned)ECHO_PSRAM_BYTES);
    return s_int != NULL && s_ps != NULL;
}

static void cmd_read(const char* path) {
    char out[200];
    int h = s_api->file_open(path);
    if (h < 0) {
        snprintf(out, sizeof(out), "read: cannot open %s", path);
        publish_str(out);
        return;
    }
    uint32_t size = s_api->file_size(h);
    int32_t got = s_api->file_read(h, s_int, 16);
    int32_t bad = s_api->file_read(h, s_ps, 16);
    s_api->file_close(h);
    int k = snprintf(out, sizeof(out), "read: size %u, internal read %d, psram read %d, head",
                     (unsigned)size, (int)got, (int)bad);
    for (int i = 0; i < got && i < 16 && k < (int)sizeof(out) - 4; i++) {
        k += snprintf(out + k, sizeof(out) - (size_t)k, " %02x", s_int[i]);
    }
    publish_str(out);
}

static void cmd_mic(void) {
    char out[160];
    if (!s_api->mic_session_open()) {
        publish_str("mic: no session open");
        return;
    }
    int16_t* buf = (int16_t*)s_int;
    const size_t cap = ECHO_INTERNAL_BYTES / sizeof(int16_t);
    uint32_t t0 = s_api->ticks_ms();
    uint32_t total = 0;
    int peak = 0;
    while (s_api->ticks_ms() - t0 < 1000 && !s_api->should_stop()) {
        size_t got = s_api->mic_read(buf, cap);
        for (size_t i = 0; i < got; i++) {
            int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) peak = v;
        }
        total += (uint32_t)got;
        if (!got) s_api->delay_ms(20);
    }
    snprintf(out, sizeof(out), "mic: %u samples in 1 s, peak %d, dropped %u",
             (unsigned)total, peak, (unsigned)s_api->mic_dropped());
    publish_str(out);
}

static void cmd_busy(void) {
    uint32_t t0 = s_api->ticks_ms();
    volatile uint32_t x = 0;
    while (s_api->ticks_ms() - t0 < 3000 && !s_api->should_stop()) {
        for (uint32_t i = 0; i < 20000; i++) x += i;
    }
    publish_str(s_api->should_stop() ? "busy: stopped early" : "busy: done");
}

static void echo_worker(const AudioPlugHostApi* api) {
    char cmd[AUDIOPLUG_CMD_MAX + 1];
    char out[AUDIOPLUG_CMD_MAX + 16];
    api->log("worker up");
    while (!api->should_stop()) {
        api->wait(200);
        uint32_t n = api->command(cmd, AUDIOPLUG_CMD_MAX);
        if (!n) continue;
        cmd[n] = 0;
        s_cmds++;
        if (strcmp(cmd, "ping") == 0)            publish_str("pong");
        else if (strncmp(cmd, "read ", 5) == 0)  cmd_read(cmd + 5);
        else if (strcmp(cmd, "mic") == 0)        cmd_mic();
        else if (strcmp(cmd, "busy") == 0)       cmd_busy();
        else {
            snprintf(out, sizeof(out), "unknown: %s", cmd);
            publish_str(out);
        }
    }
    api->log("worker returns after %u commands", (unsigned)s_cmds);
}

static void echo_status(char* out, uint32_t n) {
    snprintf(out, n, "echo: %u commands", (unsigned)s_cmds);
}

static void echo_deinit(const AudioPlugHostApi* api) {
    api->log("deinit");
    s_api = NULL;
}

const AudioPlugDesc audioplug_ops = {
    AUDIO_PLUGIN_ABI_VERSION,
    "echo",
    { { { ECHO_INTERNAL_BYTES, AUDIOPLUG_MEM_INTERNAL },
        { ECHO_PSRAM_BYTES,    AUDIOPLUG_MEM_PSRAM },
        { 0, AUDIOPLUG_MEM_NONE },
        { 0, AUDIOPLUG_MEM_NONE } },
      4096 },
    echo_init,
    echo_worker,
    echo_status,
    echo_deinit,
};

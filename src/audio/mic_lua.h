// mic_lua.h — the microphone's Lua bindings (mic_lua.cpp), generic for every
// mic app: voice notes, dictation, the test app. FW_API 16.
//
//   _mic_open(owner_name) -> ok, err   one owner at a time; err names the holder
//   _mic_read(max_bytes) -> string|nil little-endian s16 mono at 16 kHz; nil = ring empty
//   _mic_level() -> peak, dropped       |sample| peak since the previous call, ring overflow count
//   _mic_set_gain(step) -> step|nil     0..7, 6 dB per step
//   _mic_close()
//   _mic_owner() -> name|nil
//   _mic_ptt() -> held                  the mic key (T-Deck) / Record key (P4 keyboard) is down
//                                       while push-to-talk is armed or a session is open
//   _mic_ptt_arm(on) -> ok, err         arm push-to-talk without a session: the capture
//                                       task is made now and parked between sessions (a
//                                       press then needs no new internal RAM), and the key
//                                       stops being the notifications shortcut and
//                                       _mic_ptt reads it; the microphone stays off
//                                       (sounds keep playing) until the app opens a session
//
// Playback is suspended for the whole session (mic_dev.h). Lua teardown and
// standby close the session and disarm push-to-talk; _mic_close() closes
// only the session.

#pragma once

extern "C" {
#include <lua.h>
}
#include "mic_dev.h"   // mic_hw_kind / MicDevKind for _device_caps().mic

void mic_register_lua(lua_State* L);

// End the session and disarm push-to-talk from the C side (Lua teardown,
// standby); no-op without either.
void mic_lua_close_all(const char* reason);

// A session is open (audio/audio_plugin.cpp: a plugin's mic reads).
bool mic_lua_owner_active(void);

// Push-to-talk is armed or a session is open: the mic key is push-to-talk,
// not the notifications shortcut (input/input_ui.cpp).
bool mic_lua_ptt_active(void);

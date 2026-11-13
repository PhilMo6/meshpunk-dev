// mic_lua.cpp — see mic_lua.h.

#include "mic_lua.h"
#include "mic_dev.h"

#include <Arduino.h>
extern "C" {
#include <lauxlib.h>
}
#include <string.h>

#include "../meshpunk_sync.h"        // SLog
#include "../input/input_dev.h"      // input_dev_kbd_mic_held

static char s_owner[32] = "";
static bool s_ptt_armed = false;   // _mic_ptt_arm(true): the key is PTT without a session

bool mic_lua_owner_active(void) { return s_owner[0] != 0; }

bool mic_lua_ptt_active(void) { return s_ptt_armed || s_owner[0] != 0; }

static void session_close(const char* reason) {
  if (!s_owner[0]) return;
  SLog.printf("[MIC] session '%s' closed: %s\n", s_owner, reason);
  mic_capture_stop();
  s_owner[0] = 0;
}

void mic_lua_close_all(const char* reason) {
  session_close(reason);
  if (s_ptt_armed) {
    SLog.printf("[MIC] push-to-talk disarmed: %s\n", reason);
    s_ptt_armed = false;
    mic_capture_reserve(false);
  }
}

static int l_mic_open(lua_State* L) {
  const char* name = luaL_optstring(L, 1, "app");
  if (s_owner[0]) {
    lua_pushboolean(L, 0);
    lua_pushfstring(L, "in use by %s", s_owner);
    return 2;
  }
  if (mic_hw_kind() == MIC_DEV_NONE) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no microphone");
    return 2;
  }
  if (!mic_capture_start()) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "capture start failed (see the log)");
    return 2;
  }
  strncpy(s_owner, name, sizeof(s_owner) - 1);
  s_owner[sizeof(s_owner) - 1] = 0;
  lua_pushboolean(L, 1);
  return 1;
}

// The samples go through a stack chunk: a Lua buffer's bytes carry no
// alignment promise and the samples are 16-bit.
static int l_mic_read(lua_State* L) {
  size_t max_bytes = (size_t)luaL_optinteger(L, 1, 4096);
  if (!mic_capture_running()) { lua_pushnil(L); return 1; }
  size_t max_samples = max_bytes / 2;
  if (max_samples == 0) max_samples = 1;
  if (max_samples > 8192) max_samples = 8192;
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  int16_t chunk[512];
  size_t total = 0;
  while (total < max_samples) {
    size_t want = max_samples - total;
    if (want > 512) want = 512;
    size_t n = mic_capture_read(chunk, want);
    if (n == 0) break;
    luaL_addlstring(&b, (const char*)chunk, n * sizeof(int16_t));
    total += n;
  }
  if (total == 0) {
    luaL_pushresult(&b);
    lua_pop(L, 1);
    lua_pushnil(L);
    return 1;
  }
  luaL_pushresult(&b);
  return 1;
}

static int l_mic_level(lua_State* L) {
  lua_pushinteger(L, mic_capture_running() ? mic_capture_peak() : 0);
  lua_pushinteger(L, (lua_Integer)mic_capture_dropped());
  return 2;
}

static int l_mic_set_gain(lua_State* L) {
  lua_Integer step = luaL_checkinteger(L, 1);
  if (step < 0) step = 0;
  if (step > 7) step = 7;
  int r = mic_hw_set_gain_step((uint8_t)step);
  if (r < 0) lua_pushnil(L); else lua_pushinteger(L, r);
  return 1;
}

static int l_mic_close(lua_State* L) {
  (void)L;
  session_close("closed by the app");
  return 0;
}

static int l_mic_owner(lua_State* L) {
  if (s_owner[0]) lua_pushstring(L, s_owner); else lua_pushnil(L);
  return 1;
}

static int l_mic_ptt(lua_State* L) {
  lua_pushboolean(L, mic_lua_ptt_active() && input_dev_kbd_mic_held());
  return 1;
}

static int l_mic_ptt_arm(lua_State* L) {
  bool on = lua_toboolean(L, 1);
  if (on == s_ptt_armed) { lua_pushboolean(L, 1); return 1; }
  if (mic_hw_kind() == MIC_DEV_NONE) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no microphone");
    return 2;
  }
  if (!mic_capture_reserve(on)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no internal RAM for the microphone task (see the log)");
    return 2;
  }
  s_ptt_armed = on;
  lua_pushboolean(L, 1);
  return 1;
}

void mic_register_lua(lua_State* L) {
  lua_register(L, "_mic_open",     l_mic_open);
  lua_register(L, "_mic_read",     l_mic_read);
  lua_register(L, "_mic_level",    l_mic_level);
  lua_register(L, "_mic_set_gain", l_mic_set_gain);
  lua_register(L, "_mic_close",    l_mic_close);
  lua_register(L, "_mic_owner",    l_mic_owner);
  lua_register(L, "_mic_ptt",      l_mic_ptt);
  lua_register(L, "_mic_ptt_arm",  l_mic_ptt_arm);
}

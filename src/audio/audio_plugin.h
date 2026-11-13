// audio_plugin.h — loader for audio plugin modules (audio_plugin_abi.h) and
// their Lua bindings. FW_API 16.
//
//   _audio_plug_load(path) -> true | nil, err   one plugin at a time; path names
//                                               the ".elf" form (_elf_arch_path
//                                               picks this CPU's file)
//   _audio_plug_unload()                         stop the worker, free everything
//   _audio_plug_loaded() -> name, running | nil  running = its worker has not returned
//   _audio_plug_write(cmd) -> true | nil, err    queue a command (<= 256 bytes)
//   _audio_plug_read() -> result | nil           the oldest published result
//   _audio_plug_status() -> line | nil           the plugin's status line

#pragma once

extern "C" {
#include <lua.h>
}

void audio_plugin_register_lua(lua_State* L);

// Stop and unload the loaded plugin, if any (Lua teardown, standby). Blocks
// until the plugin's worker has returned.
void audio_plugin_unload_all(const char* reason);

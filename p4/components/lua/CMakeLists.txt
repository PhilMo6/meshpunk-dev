# components/lua — lib/lua (Lua 5.5.1 with the MESHPUNK edits; luaconf.h
# sets LUA_32BITS) for the P4: every .c except the two standalone programs,
# lua.c and luac.c, which define main().
set(LUA_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/lua")
file(GLOB LUA_SOURCES "${LUA_DIR}/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac)\\.c$")
idf_component_register(
    SRCS ${LUA_SOURCES}
    INCLUDE_DIRS "${LUA_DIR}")

# Copyright (c) 2018-2026 Giuseppe Marino
# SPDX-License-Identifier: BSD-3-Clause
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_INCLUDES "${LUA_INCLUDE_DIRS}")
set(CMAKE_REQUIRED_LIBRARIES "${LUA_LIBRARIES}")
# A build directory may be reused for a different Lua VM. Do not reuse probes
# from its previous header/library pair.
unset(TDLUA_STANDARD_LUA CACHE)
unset(TDLUA_LUAJIT_CAPABLE CACHE)
check_cxx_source_compiles([=[
extern "C" {
#include <lua.h>
}
#if LUA_VERSION_NUM < 502 || LUA_VERSION_NUM >= 506
#error Unsupported standard Lua version
#endif
int main() { return 0; }
]=] TDLUA_STANDARD_LUA)
if(NOT TDLUA_STANDARD_LUA)
    check_cxx_source_compiles([=[
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <luajit.h>
}
#if LUA_VERSION_NUM != 501 || LUAJIT_VERSION_NUM < 20100 || LUAJIT_VERSION_NUM >= 20200
#error LuaJIT 2.1 headers are required
#endif
int main() {
    lua_State *L = luaL_newstate();
    int yieldable = lua_isyieldable(L);
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_ON);
    lua_close(L);
    return yieldable;
}
]=] TDLUA_LUAJIT_CAPABLE)
endif()
cmake_pop_check_state()

if(TDLUA_STANDARD_LUA)
    if(TDLUA_LUA_IMPLEMENTATION STREQUAL "luajit")
        message(FATAL_ERROR "TDLUA_LUA_IMPLEMENTATION=luajit requires LuaJIT 2.1 headers and library")
    endif()
    message(STATUS "TDLua runtime: standard Lua ${LUA_VERSION_STRING}, C continuations")
elseif(TDLUA_LUAJIT_CAPABLE AND NOT TDLUA_LUA_IMPLEMENTATION STREQUAL "lua")
    set(TDLUA_USE_LUAJIT_CONTINUATION ON)
    set(TDLUA_LUA_VERSION "5.1" CACHE STRING "LuaJIT installation ABI" FORCE)
    message(STATUS "TDLua runtime: LuaJIT 2.1, cached Lua continuations")
else()
    message(FATAL_ERROR
        "TDLua Full Managed API requires Lua 5.2-5.5 or LuaJIT 2.1 with lua_isyieldable. "
        "Stock Lua 5.1 is unsupported. Select LuaJIT with -DTDLUA_LUA_IMPLEMENTATION=luajit "
        "or supply matching LUA_INCLUDE_DIR and LUA_LIBRARY paths.")
endif()

add_library(tdlua_lua_runtime INTERFACE)
if(TDLUA_USE_LUAJIT_CONTINUATION)
    target_compile_definitions(tdlua_lua_runtime INTERFACE TDLUA_USE_LUAJIT_CONTINUATION)
endif()

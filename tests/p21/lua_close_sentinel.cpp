// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// WP1/T2: lua_close with a pending observer must not invoke its callback.
// The sentinel is a C function with an external counter, so the assertion does
// not depend on any Lua value surviving the close.
#include "tdlua/binding/lua_binding.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <atomic>
#include <cstdio>

namespace {
std::atomic<int> sentinel_calls{0};

int sentinel(lua_State *)
{
    sentinel_calls.fetch_add(1, std::memory_order_relaxed);
    return 0;
}
} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) return 2;
    luaL_openlibs(L);

    lua_pushcfunction(L, luaopen_tdlua);
    if (lua_pcall(L, 0, 1, 0) != 0) {
        std::fprintf(stderr, "luaopen_tdlua failed: %s\n",
                     lua_tostring(L, -1));
        lua_close(L);
        return 3;
    }
    lua_setglobal(L, "tdlua_module");

    lua_pushcfunction(L, sentinel);
    lua_setglobal(L, "p21_sentinel");

    const char *chunk =
        "local t = tdlua_module\n"
        "t.setLogLevel(0)\n"
        "local client = t()\n"
        "local future = client:getOption({name = 'version'})\n"
        "assert(not future:ready(), 'future must be pending')\n"
        "local token = future:on_complete(p21_sentinel)\n"
        "assert(type(token) == 'userdata', 'on_complete must return a token')\n";
    if (luaL_dostring(L, chunk) != 0) {
        std::fprintf(stderr, "chunk failed: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 4;
    }

    lua_close(L);
    if (sentinel_calls.load(std::memory_order_relaxed) != 0) {
        std::fprintf(stderr, "sentinel invoked during lua_close\n");
        return 5;
    }
    std::printf("P21_LUA_CLOSE_SENTINEL_OK\n");
    return 0;
}

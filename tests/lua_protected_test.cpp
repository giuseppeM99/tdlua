// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/common/lua_protected.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void throwNonStandard(lua_State *, void *)
{
    throw 42;
}

void throwLuaErrorSentinel(lua_State *L, void *)
{
    lua_pushstring(L, "protected Lua error");
    throw TDLuaLuaError{};
}

void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

int main()
{
    lua_State *L = luaL_newstate();
    if (!L)
        return 1;
    try {
        luaL_openlibs(L);
        const int initial_top = lua_gettop(L);
        bool caught_nonstandard = false;
        try {
            tdlua::protectedLua(L, throwNonStandard, nullptr);
        } catch (int value) {
            caught_nonstandard = value == 42;
        }
        check(caught_nonstandard, "non-std exception was not rethrown after protected frame");
        check(lua_gettop(L) == initial_top, "non-std exception changed Lua stack");
        check(luaL_dostring(L, "assert(6 * 7 == 42)") == LUA_OK,
              "Lua state unusable after non-std exception");

        bool caught_lua_error = false;
        try {
            tdlua::protectedLua(L, throwLuaErrorSentinel, nullptr);
        } catch (const TDLuaLuaError &) {
            caught_lua_error = true;
        }
        check(caught_lua_error, "Lua error did not use TDLuaLuaError path");
        check(std::string(lua_tostring(L, -1)) == "protected Lua error",
              "protected Lua error object was not preserved");
        std::cout << "PASS protected Lua frame catches non-std C++ exceptions and preserves Lua errors\n";
        lua_close(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        lua_close(L);
        return 1;
    }
}

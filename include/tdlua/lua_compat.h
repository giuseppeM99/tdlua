// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <compat-5.3/compat-5.3.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

/* Resume on the calling thread. Return the Lua status and leave yielded
 * values, return values, or the error on the coroutine's stack. */
inline int tdlua_lua_resume(lua_State *coroutine, lua_State *from, int arguments)
{
#if LUA_VERSION_NUM >= 504
    int results = 0;
    return lua_resume(coroutine, from, arguments, &results);
#else
    // compat-5.3 adapts the three-argument call for Lua 5.1 and LuaJIT.
    return lua_resume(coroutine, from, arguments);
#endif
}

/* Long-lived scheduler/backend storage uses the VM's main thread rather than a
 * collectible coroutine that happened to create the client. */
inline lua_State *tdlua_lua_main_thread(lua_State *L)
{
#if LUA_VERSION_NUM >= 502
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State *main = lua_tothread(L, -1);
    lua_pop(L, 1);
    return main;
#else
    return L;
#endif
}

/* Lua 5.3 and later expose exact yieldability. Lua 5.2 uses the
 * coroutine/non-main approximation below and cannot detect a non-yieldable C
 * frame between the caller and this check. */
inline bool tdlua_lua_is_yieldable(lua_State *L)
{
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    return lua_isyieldable(L) != 0;
#elif defined(LUA_VERSION_NUM) && LUA_VERSION_NUM == 502
    const int is_main = lua_pushthread(L);
    lua_pop(L, 1);
    return is_main == 0;
#else
    (void)L;
    return false;
#endif
}

/* Lua 5.3 introduced a distinct integer value type.  Lua 5.1, Lua 5.2 and
 * LuaJIT expose lua_Integer in the C API, but their regular numeric values do
 * not provide the same reliable type distinction. */
inline bool tdlua_lua_integer_value(lua_State *L, int index, lua_Integer &value)
{
    if (lua_type(L, index) != LUA_TNUMBER) {
        return false;
    }

#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    if (lua_isinteger(L, index)) {
        value = lua_tointeger(L, index);
        return true;
    }
    return false;
#else
    const lua_Number number = lua_tonumber(L, index);
    const long double wide_number = static_cast<long double>(number);
    const long double minimum =
        static_cast<long double>((std::numeric_limits<lua_Integer>::min)());
    const long double maximum =
        static_cast<long double>((std::numeric_limits<lua_Integer>::max)());

    if (!std::isfinite(wide_number) || std::floor(wide_number) != wide_number ||
        wide_number < minimum || wide_number > maximum) {
        return false;
    }

    const lua_Integer integer = static_cast<lua_Integer>(number);
    if (static_cast<long double>(integer) != wide_number) {
        return false;
    }
    value = integer;
    return true;
#endif
}

/* Numeric table keys are allowed to be integral floats for compatibility with
 * Lua versions where every number was a float. */
inline bool tdlua_lua_key_integer_value(lua_State *L, int index, lua_Integer &value)
{
    if (tdlua_lua_integer_value(L, index, value)) {
        return true;
    }

#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    if (lua_type(L, index) != LUA_TNUMBER) {
        return false;
    }
    const lua_Number number = lua_tonumber(L, index);
    const long double wide_number = static_cast<long double>(number);
    const long double minimum =
        static_cast<long double>((std::numeric_limits<lua_Integer>::min)());
    const long double maximum =
        static_cast<long double>((std::numeric_limits<lua_Integer>::max)());
    if (!std::isfinite(wide_number) || std::floor(wide_number) != wide_number ||
        wide_number < minimum || wide_number > maximum) {
        return false;
    }
    const lua_Integer integer = static_cast<lua_Integer>(number);
    if (static_cast<long double>(integer) != wide_number) {
        return false;
    }
    value = integer;
    return true;
#else
    return false;
#endif
}

inline bool tdlua_can_push_integer(std::int64_t value)
{
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    const long double wide_value = static_cast<long double>(value);
    return wide_value >=
               static_cast<long double>((std::numeric_limits<lua_Integer>::min)()) &&
           wide_value <=
               static_cast<long double>((std::numeric_limits<lua_Integer>::max)());
#else
    const lua_Number number = static_cast<lua_Number>(value);
    return static_cast<long double>(number) == static_cast<long double>(value);
#endif
}

/* Push exactly one value. Use a Lua integer where available, an exact number
 * on older Lua versions, or a decimal string rather than rounding an int64. */
inline void tdlua_lua_push_integer(lua_State *L, std::int64_t value)
{
    if (!tdlua_can_push_integer(value)) {
        const std::string text = std::to_string(value);
        lua_pushlstring(L, text.c_str(), text.size());
        return;
    }
#if LUA_VERSION_NUM >= 503
    lua_pushinteger(L, static_cast<lua_Integer>(value));
#else
    lua_pushnumber(L, static_cast<lua_Number>(value));
#endif
}

// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <compat-5.3/compat-5.3.h>

#ifdef TDLUA_USE_LUAJIT_CONTINUATION
#include <luajit.h>
#elif LUA_VERSION_NUM < 502 && !defined(TDLUA_USE_LUA51_CONTINUATION)
#error "TDLua requires configured stock Lua 5.1, Lua 5.2+, or LuaJIT 2.1"
#endif

#if defined(TDLUA_USE_LUAJIT_CONTINUATION) || defined(TDLUA_USE_LUA51_CONTINUATION)
#define TDLUA_USE_LUA_CONTINUATION
#endif

#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <stdexcept>

// Lua 5.1-5.3 create the luaL_ref free-list header on the first unref.
// Prepare that slot before acquiring a construction reference so rollback
// only updates existing keys. Lua 5.4+ initializes its header in luaL_ref.
inline void tdlua_lua_prepare_unref(lua_State *L, int table)
{
#if LUA_VERSION_NUM < 504
    lua_rawgeti(L, table, 0);
    const bool missing = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (missing) {
        lua_pushinteger(L, 0);
        lua_rawseti(L, table, 0);
    }
#else
    (void)L;
    (void)table;
#endif
}

/* Value uservalues are direct values on Lua 5.3+ but must be tables on Lua
 * 5.1 and 5.2. Keep this adapter separate from the reference-table uservalues
 * used by continuations and scheduler storage. */
inline void tdlua_lua_set_value_uservalue(lua_State *L, int index)
{
    index = lua_absindex(L, index);
#if LUA_VERSION_NUM <= 502
    lua_newtable(L);
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, 1);
    lua_remove(L, -2);
#endif
    lua_setuservalue(L, index);
}

inline void tdlua_lua_get_value_uservalue(lua_State *L, int index)
{
    lua_getuservalue(L, index);
#if LUA_VERSION_NUM <= 502
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        lua_remove(L, -2);
    }
#endif
}

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
#elif defined(TDLUA_USE_LUA_CONTINUATION)
    static const char main_thread_key = 0;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &main_thread_key);
    lua_State *main = lua_tothread(L, -1);
    lua_pop(L, 1);
    if (main) return main;
    const bool is_main = lua_pushthread(L) != 0;
    if (!is_main) {
        lua_pop(L, 1);
        throw std::runtime_error(
#ifdef TDLUA_USE_LUA51_CONTINUATION
            "tdlua: initialize the module on the Lua 5.1 main thread first");
#else
            "tdlua: initialize the module on the LuaJIT main thread first");
#endif
    }
    lua_rawsetp(L, LUA_REGISTRYINDEX, &main_thread_key);
    return L;
#endif
}

inline void tdlua_lua_clear_uservalue(lua_State *L, int index)
{
    index = lua_absindex(L, index);
#if LUA_VERSION_NUM <= 502
    lua_newtable(L);
#else
    lua_pushnil(L);
#endif
    lua_setuservalue(L, index);
}

/* Lua 5.3 and later expose exact yieldability. Lua 5.2 uses the
 * coroutine/non-main approximation below and cannot detect a non-yieldable C
 * frame between the caller and this check. */
inline bool tdlua_lua_is_yieldable(lua_State *L)
{
#if LUA_VERSION_NUM >= 503 || defined(TDLUA_USE_LUAJIT_CONTINUATION)
    return lua_isyieldable(L) != 0;
#elif LUA_VERSION_NUM == 502 || defined(TDLUA_USE_LUA51_CONTINUATION)
    const int is_main = lua_pushthread(L);
    lua_pop(L, 1);
    return is_main == 0;
#else
    (void)L;
    return false;
#endif
}

// Call inside the binding's protected_call, before creating/submitting a
// request whose result is returned through an explicit wait without a handle.
inline void tdlua_lua_require_explicit_submission_context(lua_State *L)
{
#ifdef TDLUA_USE_LUA51_CONTINUATION
    if (!tdlua_lua_is_yieldable(L)) {
        throw std::runtime_error("tdlua: Lua 5.1 explicit request wait requires a coroutine");
    }
#else
    (void)L;
#endif
}

/* Converts an integral Lua number to lua_Integer, accepting the interval
 * [-2^digits, 2^digits) for the value bits of lua_Integer. The bounds are exact
 * powers of two, so they stay exact in every binary floating-point format.
 * Casting the limits of lua_Integer to `long double` does not: with binary64
 * `long double` it rounds 2^63-1 up to 2^63, so a number equal to 2^63 passes
 * the interval check and reaches an out-of-range conversion. That conversion is
 * undefined; on x86-64 it yields INT64_MIN and the round-trip check below
 * rejects the value, on arm64 it saturates to INT64_MAX, which round-trips, so
 * the value was silently replaced instead of rejected. */
inline bool tdlua_lua_integral_number_value(lua_State *L, int index, lua_Integer &value)
{
    const lua_Number number = lua_tonumber(L, index);
    const long double wide_number = static_cast<long double>(number);
    const int integer_digits = std::numeric_limits<lua_Integer>::digits;
    // Built from lua_Number, whose width and ABI are fixed for the target.
    const lua_Number two_to_digits =
        std::ldexp(static_cast<lua_Number>(1), integer_digits);
    const long double minimum = -static_cast<long double>(two_to_digits);
    const long double maximum = static_cast<long double>(two_to_digits);

    if (!std::isfinite(wide_number) || std::floor(wide_number) != wide_number ||
        wide_number < minimum || wide_number >= maximum) {
        return false;
    }

    const lua_Integer integer = static_cast<lua_Integer>(number);
    if (static_cast<long double>(integer) != wide_number) {
        return false;
    }
    value = integer;
    return true;
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
    return tdlua_lua_integral_number_value(L, index, value);
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
    return tdlua_lua_integral_number_value(L, index, value);
#else
    return false;
#endif
}

/* Whether a binary floating-point format with `digits` significand bits and a
 * largest exponent `max_exponent` represents the int64 exactly. A nonzero
 * magnitude is 2^trailing * odd, so `odd` is the significand and the format
 * holds the value when `odd` fits the significand and the magnitude stays
 * inside the exponent range. Zero is exact. Only integer arithmetic is used,
 * so the answer does not depend on `long double`, which is binary64 on MSVC
 * x64 and macOS arm64 and made the former floating-point round-trip test
 * tautological there. */
inline bool tdlua_int64_fits_binary_number(std::int64_t value, int digits,
                                           int max_exponent)
{
    if (value == 0) {
        return true;
    }
    // Negating INT64_MIN would overflow: -(-9223372036854775808) is not an
    // int64. Negate value + 1 and add one to recover the magnitude in uint64.
    std::uint64_t odd = value < 0
        ? static_cast<std::uint64_t>(-(value + 1)) + 1u
        : static_cast<std::uint64_t>(value);
    int trailing = 0;
    while ((odd & 1u) == 0) {
        odd >>= 1;
        ++trailing;
    }
    int significant_bits = 0;
    for (std::uint64_t rest = odd; rest != 0; rest >>= 1) {
        ++significant_bits;
    }
    // The magnitude lies in [2^(total_bits - 1), 2^total_bits), so total_bits
    // is its frexp-style exponent and must fit the format's exponent range.
    const int total_bits = significant_bits + trailing;
    return significant_bits <= digits && total_bits <= max_exponent;
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
    // Lua 5.1, Lua 5.2 and LuaJIT store every number as lua_Number: push the
    // exact value only when the significand holds it. A non-binary lua_Number
    // is outside the rule below, so use the exact decimal string instead of
    // rounding silently.
    if (std::numeric_limits<lua_Number>::radix != 2) {
        return false;
    }
    return tdlua_int64_fits_binary_number(
        value, std::numeric_limits<lua_Number>::digits,
        std::numeric_limits<lua_Number>::max_exponent);
#endif
}

/* Push exactly one value. Use a Lua integer where available, an exact number
 * on older Lua versions, or a decimal string rather than rounding an int64. */
inline void tdlua_lua_push_integer(lua_State *L, std::int64_t value)
{
    if (!tdlua_can_push_integer(value)) {
        char text[32];
        const int length = std::snprintf(text, sizeof(text), "%" PRId64, value);
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(text)) {
            throw std::runtime_error("tdlua: integer conversion failed");
        }
        lua_pushlstring(L, text, static_cast<std::size_t>(length));
        return;
    }
#if LUA_VERSION_NUM >= 503
    lua_pushinteger(L, static_cast<lua_Integer>(value));
#else
    lua_pushnumber(L, static_cast<lua_Number>(value));
#endif
}

/* Execute a bounded Lua frame so interpreter debug hooks, including its
 * ordinary SIGINT hook, can run while a managed driver otherwise stays in C++.
 * Protect the hook error so C++ scopes unwind before the binding raises it. */
inline void tdlua_lua_vm_checkpoint(lua_State *L)
{
    static const char key = 0;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, "return", 6, "=tdlua checkpoint") != LUA_OK) {
            const std::string error = lua_tostring(L, -1);
            lua_pop(L, 1);
            throw std::runtime_error(error);
        }
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &key);
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        const std::string error = message ? message : "tdlua: VM checkpoint failed";
        lua_pop(L, 1);
        throw std::runtime_error(error);
    }
}

// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "tdlua/lua_compat.h"
#include <exception>
#include <new>
#include <stdexcept>

// The Lua error object stays on the stack until the outer binding rethrows it.
struct TDLuaLuaError {};

namespace tdlua {
namespace lua_protected_detail {
struct Call {
    void (*function)(lua_State *, void *);
    void *context;
    std::exception_ptr exception;
    int results;
};
inline int invoke(lua_State *L)
{
    auto *call = static_cast<Call *>(lua_touserdata(L, 1));
    bool has_lua_error = false;
    try {
        call->function(L, call->context);
    } catch (const TDLuaLuaError &) {
        has_lua_error = true;
    } catch (const std::exception &) {
        call->exception = std::current_exception();
    } catch (...) {
        call->exception = std::current_exception();
    }
    if (has_lua_error)
        return ::lua_error(L);
    return call->exception ? 0 : call->results;
}
inline const void *invokeKey()
{
    static const char key = 0;
    return &key;
}
} // namespace lua_protected_detail
// Only the callback uses fallible Lua APIs. No owning C++ automatic object is
// placed in that frame. pcall/cpcall contains Lua errors; invoke captures C++
// exceptions so they can be rethrown after the protected Lua frame returns.
inline void protectedLua(lua_State *L, void (*function)(lua_State *, void *), void *context)
{
#if LUA_VERSION_NUM >= 502
    if (!lua_checkstack(L, 8))
        throw std::bad_alloc();
#endif
    lua_protected_detail::Call call{function, context, std::exception_ptr(), 0};
#if LUA_VERSION_NUM < 502
    // Stock 5.1 can longjmp while growing the stack as well as allocating
    // the C closure. Let cpcall protect both, before any fallible Lua API.
    const int status = lua_cpcall(L, lua_protected_detail::invoke, &call);
#else
    lua_pushcfunction(L, lua_protected_detail::invoke);
    lua_pushlightuserdata(L, &call);
    const int status = lua_pcall(L, 1, 0, 0);
#endif
    if (call.exception)
        std::rethrow_exception(call.exception);
    if (status != LUA_OK)
        throw TDLuaLuaError{};
}
// Copy a range of borrowed caller values into the protected frame. Stack space
// is reserved first, so the copies cannot allocate outside protection. The
// protected callback receives context as argument 1 and values after it.
inline void protectedLuaValues(lua_State *L,
                               void (*function)(lua_State *, void *),
                               void *context, int first_index,
                               int value_count, int results)
{
    if (value_count < 0) throw std::invalid_argument("negative Lua argument count");
    if (value_count) first_index = lua_absindex(L, first_index);
    struct Preparation { int capacity; } preparation = {
        lua_gettop(L) + value_count + 8};
    protectedLua(L, [](lua_State *state, void *pointer) {
        const auto *preparation = static_cast<Preparation *>(pointer);
        if (!lua_checkstack(state, preparation->capacity)) throw std::bad_alloc();
        lua_rawgetp(state, LUA_REGISTRYINDEX, lua_protected_detail::invokeKey());
        const bool missing = lua_isnil(state, -1);
        lua_pop(state, 1);
        if (missing) {
            lua_pushcfunction(state, lua_protected_detail::invoke);
            lua_rawsetp(state, LUA_REGISTRYINDEX, lua_protected_detail::invokeKey());
        }
    }, &preparation);
    if (!lua_checkstack(L, value_count + 2)) throw std::bad_alloc();
    const int top = lua_gettop(L);
    lua_protected_detail::Call call = {function, context, std::exception_ptr(), results};
    lua_rawgetp(L, LUA_REGISTRYINDEX, lua_protected_detail::invokeKey());
    lua_pushlightuserdata(L, &call);
    for (int i = 0; i < value_count; ++i) {
        lua_pushvalue(L, first_index + i);
    }
    const int status = lua_pcall(L, value_count + 1, results, 0);
    if (call.exception) {
        lua_settop(L, top);
        std::rethrow_exception(call.exception);
    }
    if (status != LUA_OK) throw TDLuaLuaError{};
}

inline void protectedLua(lua_State *L, void (*function)(lua_State *, void *),
                         void *context, int value_index, int results)
{
    protectedLuaValues(L, function, context, value_index,
                       value_index ? 1 : 0, results);
}
} // namespace tdlua

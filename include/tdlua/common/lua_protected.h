// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "tdlua/lua_compat.h"
#include <exception>
#include <new>

// The Lua error object stays on the stack until the outer binding rethrows it.
struct TDLuaLuaError {};

namespace tdlua {
namespace lua_protected_detail {
struct Call {
    void (*function)(lua_State *, void *);
    void *context;
    std::exception_ptr exception;
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
    }
    if (has_lua_error)
        return ::lua_error(L);
    return 0;
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
    lua_protected_detail::Call call{function, context, std::exception_ptr()};
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
} // namespace tdlua

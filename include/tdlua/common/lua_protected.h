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
inline int invoke(lua_State *L) {
    auto *call = static_cast<Call *>(lua_touserdata(L, 1));
    bool lua_error = false;
    try { call->function(L, call->context); }
    catch (const TDLuaLuaError &) { lua_error = true; }
    catch (const std::exception &) {
        call->exception = std::current_exception();
    }
    if (lua_error) return ::lua_error(L);
    return 0;
}
}
// Only the callback uses fallible Lua APIs. No owning C++ automatic object is
// placed in that frame. Standard C++ exceptions return to this owner; Lua's
// longjmp or foreign unwind is handled by Lua, never by a C++ catch-all.
inline void protectedLua(lua_State *L, void (*function)(lua_State *, void *), void *context) {
    if (!lua_checkstack(L, 8)) throw std::bad_alloc();
    lua_protected_detail::Call call{function, context, std::exception_ptr()};
#if LUA_VERSION_NUM < 502
    // pushcfunction itself allocates on stock 5.1; cpcall protects that too.
    const int status = lua_cpcall(L, lua_protected_detail::invoke, &call);
#else
    lua_pushcfunction(L, lua_protected_detail::invoke);
    lua_pushlightuserdata(L, &call);
    const int status = lua_pcall(L, 1, 0, 0);
#endif
    if (call.exception) std::rethrow_exception(call.exception);
    if (status != LUA_OK) throw TDLuaLuaError{};
}
}

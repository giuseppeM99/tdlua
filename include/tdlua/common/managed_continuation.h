// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "tdlua/lua_compat.h"

namespace tdlua {

// Normalize every immediate branch of a managed binding at the same boundary.
// Only the private LuaJIT trampoline consumes the leading boolean.
inline int finishManagedBinding(lua_State *L, int results)
{
#ifdef TDLUA_USE_LUAJIT_CONTINUATION
    lua_pushboolean(L, false);
    lua_insert(L, lua_gettop(L) - results);
    return results + 1;
#else
    (void)L;
    return results;
#endif
}

#ifdef TDLUA_USE_LUAJIT_CONTINUATION
inline int suspendedManagedBinding(lua_State *L, int context)
{
    lua_pushboolean(L, true);
    lua_pushvalue(L, context);
    return 2;
}

inline int managedTrampolineYield(lua_State *L) { return lua_yield(L, 0); }

// Convert Lua failures to C++ exceptions while caller-owned objects can unwind.
// The public binding catches the exception before raising the final Lua error.
[[noreturn]] inline void throwManagedFactoryError(lua_State *L)
{
    const char *message = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) :
        "tdlua: managed factory raised a non-string Lua error";
    const std::runtime_error error(message);
    lua_pop(L, 1);
    throw error;
}

inline void callManagedFactory(lua_State *L, int arguments, int results)
{
    if (lua_pcall(L, arguments, results, 0) != LUA_OK)
        throwManagedFactoryError(L);
}

#ifdef TDLUA_TESTING
inline const void *managedFactoryCounterKey()
{
    static const char counter_key = 0;
    return &counter_key;
}

inline int managedFactoryCompilationCount(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, managedFactoryCounterKey());
    const int count = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return count;
}
#endif

// One registry-owned cache per VM, shared by clients and their coroutines.
// Cached wrappers capture C functions and method names, never a client or Task.
inline void pushManagedContinuationCache(lua_State *L)
{
    static const char cache_key = 0;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &cache_key);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    const char factory[] = R"lua(
        return function(prepare, complete, yield, field_index)
            local function dispatch(pending, ...)
                if not pending then return ... end
                local context = ...
                return complete(context, yield())
            end
            if field_index then
                return function(self, key)
                    if key == 'wait' or key == 'ready' or key == '_request_id' then
                        return prepare(self, key)
                    end
                    return dispatch(prepare(self, key))
                end
            end
            return function(...) return dispatch(prepare(...)) end
        end
    )lua";
#ifdef TDLUA_TESTING
    lua_pushinteger(L, managedFactoryCompilationCount(L) + 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, managedFactoryCounterKey());
#endif
    if (luaL_loadbuffer(L, factory, sizeof(factory) - 1, "=tdlua managed continuation") != LUA_OK)
        throwManagedFactoryError(L);
    callManagedFactory(L, 0, 1);
    lua_setfield(L, -2, "factory");
    for (const char *name : {"functions", "fields", "helpers"}) {
        lua_newtable(L);
        lua_setfield(L, -2, name);
    }
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &cache_key);
}

inline void initializeManagedContinuations(lua_State *L)
{
    // Use the registry's loaded module, not mutable globals such as _G.jit.
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "jit");
    else lua_pushnil(L);
    if (lua_istable(L, -1)) lua_getfield(L, -1, "version_num");
    else lua_pushnil(L);
    const lua_Number version = lua_tonumber(L, -1);
    lua_pop(L, 3);
    if (version < 20100 || version >= 20200)
        throw std::runtime_error("tdlua: this module requires a LuaJIT 2.1 VM with its jit library loaded");
    tdlua_lua_main_thread(L);
    pushManagedContinuationCache(L);
    lua_pop(L, 1);
}

// Create one wrapper for the C function at the top of the stack.
inline void createManagedWrapper(lua_State *L, lua_CFunction complete, bool field_index)
{
    const int function = lua_absindex(L, -1);
    pushManagedContinuationCache(L);
    lua_getfield(L, -1, "factory");
    lua_pushvalue(L, function);
    lua_pushcfunction(L, complete);
    lua_pushcfunction(L, managedTrampolineYield);
    lua_pushboolean(L, field_index);
    callManagedFactory(L, 4, 1);
    lua_replace(L, function);
    lua_settop(L, function);
}

// LuaJIT allocates a fresh zero-upvalue C closure on lua_pushcfunction. Key the
// cache by the C entry-point representation, rather than by that closure.
inline void pushManagedFunctionKey(lua_State *L, lua_CFunction function)
{
    lua_pushlstring(L, reinterpret_cast<const char *>(&function), sizeof(function));
}

inline void pushCachedManagedFunction(lua_State *L, lua_CFunction prepare,
                                      lua_CFunction complete, bool field_index)
{
    const int base = lua_gettop(L);
    pushManagedContinuationCache(L);
    lua_getfield(L, -1, field_index ? "fields" : "functions");
    const int wrappers = lua_gettop(L);
    pushManagedFunctionKey(L, prepare);
    lua_rawget(L, wrappers);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushcfunction(L, prepare);
        createManagedWrapper(L, complete, field_index);
        pushManagedFunctionKey(L, prepare);
        lua_pushvalue(L, -2);
        lua_rawset(L, wrappers);
    }
    lua_replace(L, base + 1);
    lua_settop(L, base + 1);
}

inline void wrapManagedFunction(lua_State *L, lua_CFunction complete,
                                bool field_index = false)
{
    const int function = lua_absindex(L, -1);
    pushCachedManagedFunction(L, lua_tocfunction(L, function), complete, field_index);
    lua_replace(L, function);
}

inline void pushManagedHelper(lua_State *L, const char *name,
                              lua_CFunction helper, lua_CFunction complete)
{
    const int base = lua_gettop(L);
    pushManagedContinuationCache(L);
    lua_getfield(L, -1, "helpers");
    pushManagedFunctionKey(L, helper);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        // Dynamic names are unrestricted. Retain a wrapper only while Lua code
        // holds it; the finite entry-point and field caches remain strong.
        lua_newtable(L);
        lua_pushliteral(L, "v");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        pushManagedFunctionKey(L, helper);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_getfield(L, -1, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, name);
        lua_pushcclosure(L, helper, 1);
        createManagedWrapper(L, complete, false);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name);
    }
    lua_replace(L, base + 1);
    lua_settop(L, base + 1);
}

#ifdef TDLUA_TESTING
inline int managedHelperCacheSize(lua_State *L)
{
    const int base = lua_gettop(L);
    pushManagedContinuationCache(L);
    lua_getfield(L, -1, "helpers");
    const int helpers = lua_gettop(L);
    int entries = 0;
    lua_pushnil(L);
    while (lua_next(L, helpers)) {
        const int names = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, names)) {
            ++entries;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_settop(L, base);
    return entries;
}
#endif

#else
inline void initializeManagedContinuations(lua_State *) {}
inline void wrapManagedFunction(lua_State *, lua_CFunction, bool = false) {}
inline void pushManagedHelper(lua_State *L, const char *name,
                              lua_CFunction helper, lua_CFunction)
{
    lua_pushstring(L, name);
    lua_pushcclosure(L, helper, 1);
}
#endif

inline void pushManagedFunction(lua_State *L, lua_CFunction prepare,
                                lua_CFunction complete, bool field_index = false)
{
#ifdef TDLUA_USE_LUAJIT_CONTINUATION
    pushCachedManagedFunction(L, prepare, complete, field_index);
#else
    (void)complete;
    (void)field_index;
    lua_pushcfunction(L, prepare);
#endif
}
} // namespace tdlua

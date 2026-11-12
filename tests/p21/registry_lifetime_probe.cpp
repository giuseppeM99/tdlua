// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic registry-lifetime probe for P2.0-R2. Not production code.
#include "tdlua/common/request_router.h"

#include <iostream>
#include <memory>

namespace {
int nothing(lua_State *) { return 0; }
void collect(lua_State *L)
{
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
}
} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) return 2;
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);

    // weak[1] = sentinel, with weak values.
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "weak");
    lua_newtable(L); // sentinel
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, 1);
    lua_remove(L, -2);
    // Stack: sentinel.

    std::weak_ptr<tdlua::SchedulerCore> core_weak;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        core_weak = core;
        auto state = router.future();
        lua_pushcclosure(L, nothing, 1); // callback with sentinel upvalue
        const int callback = lua_gettop(L);
        core->watchCompletion(L, state, callback);
        lua_pop(L, 1);
    }

    collect(L);
    lua_getglobal(L, "weak");
    lua_rawgeti(L, -1, 1);
    const bool sentinel_alive = !lua_isnil(L, -1);
    lua_pop(L, 2);
    std::cout << "core_expired=" << core_weak.expired()
              << " sentinel_alive_after_router=" << sentinel_alive << '\n';

    lua_close(L);
    std::cout << "done\n";
    return sentinel_alive ? 3 : 0;
}

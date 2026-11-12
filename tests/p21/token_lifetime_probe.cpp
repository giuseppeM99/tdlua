// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// WP1: a token still reachable from Lua keeps the core alive after the router
// is destroyed, while teardown has already dropped the observer's state edge.
// The token must remain cancellable and must release its observer and anchor
// when Lua drops it.
#include "tdlua/common/request_router.h"

#include <iostream>
#include <memory>

namespace {
int nothing(lua_State *) { return 0; }

void collect(lua_State *L)
{
    for (int i = 0; i < 3; ++i) lua_gc(L, LUA_GCCOLLECT, 0);
}

bool callCancel(lua_State *L, const char *global)
{
    lua_getglobal(L, global);
    lua_getfield(L, -1, "cancel");
    lua_pushvalue(L, -2);
    const int status = lua_pcall(L, 1, 1, 0);
    const bool cancelled = status == 0 && lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    return cancelled;
}
} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) return 2;
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);

    std::weak_ptr<tdlua::SchedulerCore> core_weak;
    std::weak_ptr<tdlua::ManagedState> state_weak;
    std::weak_ptr<tdlua::CompletionObserver> observer_weak;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        core_weak = core;
        auto state = router.future();
        state_weak = state;
        lua_pushcfunction(L, nothing);
        const int callback = lua_gettop(L);
        auto observer = core->watchCompletion(L, state, callback);
        lua_pop(L, 1);
        observer_weak = observer;
        tdlua::pushCompletionTokenProtected(L, core.get(), observer);
        lua_setglobal(L, "tok");
        state.reset();
        observer.reset();
    }

    collect(L);
    const bool core_alive = !core_weak.expired();
    const bool observer_alive = !observer_weak.expired();
    std::cout << "after_router core_alive=" << core_alive
              << " state_expired=" << state_weak.expired()
              << " observer_alive=" << observer_alive << '\n';

    const bool cancelled = callCancel(L, "tok");
    std::cout << "cancel=" << cancelled << '\n';

    lua_pushnil(L);
    lua_setglobal(L, "tok");
    collect(L);
    const bool core_expired = core_weak.expired();
    const bool observer_expired = observer_weak.expired();
    const bool state_expired = state_weak.expired();
    std::cout << "after_token core_expired=" << core_expired
              << " observer_expired=" << observer_expired
              << " state_expired=" << state_weak.expired() << '\n';

    lua_close(L);
    // The token owns the core anchor, and the core still owns its state maps:
    // the state may legitimately outlive the router while the token is alive.
    if (!core_alive || !observer_alive) return 3;
    if (cancelled) return 4;
    if (!core_expired || !observer_expired) return 5;
    if (!state_expired) return 6;
    std::cout << "TOKEN_LIFETIME_OK\n";
    return 0;
}

// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic ownership probe for P2.0 review. Not production code.
#include "tdlua/common/request_router.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
int nothing(lua_State *) { return 0; }
void collect(lua_State *L)
{
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

    std::weak_ptr<tdlua::ManagedState> state_weak;
    std::weak_ptr<tdlua::CompletionObserver> observer_weak;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        auto state = router.future();
        state_weak = state;
        lua_pushcfunction(L, nothing);
        auto observer = core->watchCompletion(L, state, lua_gettop(L));
        lua_pop(L, 1);
        observer_weak = observer;
        require(!state_weak.expired(), "state alive before scope end");
        require(!observer_weak.expired(), "observer alive before scope end");
        state.reset();
        observer.reset();
    }

    collect(L);
    const bool core_expired = tdlua::liveSchedulerCores() == 0;
    std::cout << "core_expired=" << core_expired
              << " state_expired=" << state_weak.expired()
              << " observer_expired=" << observer_weak.expired() << '\n';
    lua_close(L);
    std::cout << "after_close state_expired=" << state_weak.expired()
              << " observer_expired=" << observer_weak.expired() << '\n';
    if (!state_weak.expired() || !observer_weak.expired()) {
        std::cout << "OWNERSHIP_LEAK\n";
        return 3;
    }
    std::cout << "OWNERSHIP_OK\n";
    return 0;
}

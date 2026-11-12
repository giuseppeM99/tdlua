// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic probe for the completion-error publication failure path.
#include "tdlua/common/request_router.h"

#include <iostream>
#include <new>
#include <stdexcept>

namespace {
int errorCallback(lua_State *L) { return luaL_error(L, "observer boom"); }
int recovered = 0;
int countCallback(lua_State *) { return 0; }
int recoveryCallback(lua_State *) { ++recovered; return 0; }
void throwBadAlloc() { throw std::bad_alloc(); }
} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) return 2;
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    bool stuck = false;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        auto state = router.future();
        lua_pushcfunction(L, errorCallback);
        core->watchCompletion(L, state, lua_gettop(L));
        lua_pop(L, 1);
        lua_pushcfunction(L, countCallback);
        core->watchCompletion(L, state, lua_gettop(L));
        lua_pop(L, 1);
        core->dispatchRoute(state->request_id, [](lua_State *L) {
            lua_pushboolean(L, 1);
        });
        tdlua::setObserverErrorHookForTesting(throwBadAlloc);
        try {
            core->tick();
        } catch (const std::bad_alloc &) {
            std::cout << "tick threw bad_alloc\n";
        } catch (const std::exception &error) {
            std::cout << "tick threw " << error.what() << '\n';
        }
        tdlua::setObserverErrorHookForTesting(nullptr);
        stuck = core->dispatchingCompletionsForTesting();
        auto recovery = core->future();
        lua_pushcfunction(L, recoveryCallback);
        core->watchCompletion(L, recovery, lua_gettop(L));
        lua_pop(L, 1);
        core->dispatchRoute(recovery->request_id, [](lua_State *L) {
            lua_pushboolean(L, 1);
        });
        core->tick();
        std::cout << "dispatching_completions=" << stuck
                  << " recovered=" << recovered << '\n';
        if (recovered != 1) stuck = true;
    }
    lua_close(L);
    return stuck ? 3 : 0;
}

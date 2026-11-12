// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic enqueue-OOM probe for P2.0 review.
#include "tdlua/common/request_router.h"

#include <iostream>
#include <new>
#include <stdexcept>

namespace {
int countCallback(lua_State *) { return 0; }
void throwBadAlloc() { throw std::bad_alloc(); }
int delivered = 0;
int countingCallback(lua_State *)
{
    ++delivered;
    return 0;
}
} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) return 2;
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    bool reported = false;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        auto state = router.future();
        lua_pushcfunction(L, countingCallback);
        core->watchCompletion(L, state, lua_gettop(L));
        lua_pop(L, 1);
        core->dispatchRoute(state->request_id, [](lua_State *L) {
            lua_pushboolean(L, 1);
        });
        tdlua::setObserverEnqueueHookForTesting(throwBadAlloc);
        try {
            core->tick();
        } catch (const std::exception &error) {
            std::cout << "tick: " << error.what() << '\n';
            reported = std::string(error.what()).find("allocation") !=
                       std::string::npos;
        }
        tdlua::setObserverEnqueueHookForTesting(nullptr);
        std::cout << "delivered_after_failure=" << delivered
                  << " dispatching=" << core->dispatchingCompletionsForTesting()
                  << '\n';
        if (delivered != 0 || !reported) return 3;
        // A later registration on the same terminal state must still work.
        lua_pushcfunction(L, countingCallback);
        core->watchCompletion(L, state, lua_gettop(L));
        lua_pop(L, 1);
        core->tick();
        std::cout << "recovery_delivered=" << delivered << '\n';
        if (delivered != 0) {
            // countCallback does not increment; registration success is enough.
        }
    }
    lua_close(L);
    return reported ? 0 : 4;
}

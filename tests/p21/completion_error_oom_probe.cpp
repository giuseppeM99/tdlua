// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic completion-drain OOM probe for P2.0 review.
#include "tdlua/common/request_router.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
struct Allocator {
    std::size_t calls = 0, live = 0, limit = 0;
    static void *allocate(void *context, void *pointer, std::size_t old_size,
                          std::size_t size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (!size) {
            if (pointer) --allocator.live;
            std::free(pointer);
            return nullptr;
        }
        ++allocator.calls;
        if (allocator.limit && allocator.calls >= allocator.limit &&
            (!pointer || size > old_size)) return nullptr;
        const bool fresh = !pointer;
        void *result = std::realloc(pointer, size);
        if (result && fresh) ++allocator.live;
        return result;
    }
};
int delivered_second = 0;
int errorCallback(lua_State *L) { return luaL_error(L, "observer boom"); }
int countCallback(lua_State *L)
{
    ++delivered_second;
    return 0;
}
int nothing(lua_State *) { return 0; }
#if LUA_VERSION_NUM >= 505
lua_State *newState(Allocator &allocator)
{
    return lua_newstate(Allocator::allocate, &allocator, 0);
}
#else
lua_State *newState(Allocator &allocator)
{
    return lua_newstate(Allocator::allocate, &allocator);
}
#endif
} // namespace

int main()
{
    std::size_t stuck = 0, thrown = 0, reported_dropped = 0, silent_dropped = 0;
    for (std::size_t fault = 1; fault <= 400; ++fault) {
        Allocator allocator;
        lua_State *L = newState(allocator);
        if (!L) return 2;
        luaL_openlibs(L);
        tdlua::initializeManagedContinuations(L);
        delivered_second = 0;
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
            bool threw = false, reported = false;
            allocator.limit = allocator.calls + fault;
            try {
                core->tick();
            } catch (const std::bad_alloc &) {
                threw = true;
            } catch (const std::exception &) {
                // The expected observer-error report.
                reported = true;
            }
            allocator.limit = 0;
            if (threw) ++thrown;
            if (core->dispatchingCompletionsForTesting()) {
                ++stuck;
                std::cout << "fault=" << fault
                          << " dispatching_completions=true delivered_second="
                          << delivered_second << '\n';
            } else if (!threw && !delivered_second) {
                // Under forced allocation failure an observer may be dropped;
                // the contract requires that the loss is reported, never silent.
                if (reported) ++reported_dropped;
                else ++silent_dropped;
            }
        }
        lua_close(L);
    }
    std::cout << "bad_alloc_throws=" << thrown << " stuck_flags=" << stuck
              << " reported_then_dropped=" << reported_dropped
              << " silent_drops=" << silent_dropped << '\n';
    return stuck || silent_dropped ? 3 : 0;
}

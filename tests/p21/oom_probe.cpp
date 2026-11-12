// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic OOM probe for P2.0 review. Not production code.
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
    std::size_t observed_failures = 0, leaked = 0, oom_errors = 0;
    for (std::size_t fault = 1; fault <= 300; ++fault) {
        Allocator allocator;
        lua_State *L = newState(allocator);
        require(L, "newstate failed");
        luaL_openlibs(L);
        tdlua::initializeManagedContinuations(L);
        std::weak_ptr<tdlua::ManagedState> state_weak;
        std::weak_ptr<tdlua::SchedulerCore> core_weak;
        bool registered = false;
        int status = LUA_OK;
        {
            tdlua::RequestRouter router(L);
            const auto core = router.core();
            core_weak = core;
            auto state = router.future();
            state_weak = state;
            tdlua::pushManagedHandle(L, state, "tdlua.future");
            lua_setglobal(L, "f");
            lua_pushcfunction(L, nothing);
            lua_setglobal(L, "cb");

            // Build the call before arming the allocator limit.
            lua_getglobal(L, "f");
            lua_getfield(L, -1, "on_complete");
            lua_pushvalue(L, -2);
            lua_getglobal(L, "cb");
            allocator.limit = allocator.calls + fault;
            status = lua_pcall(L, 2, 1, 0);
            allocator.limit = 0;
            registered = state->observer_count != 0;
            if (status != LUA_OK) {
                ++oom_errors;
                if (oom_errors <= 8) {
                    const char *message = lua_tostring(L, -1);
                    std::cout << "fault=" << fault << " status=" << status
                              << " registered=" << registered
                              << " error=" << (message ? message : "(non-string)")
                              << '\n';
                }
                lua_pop(L, 1);
            } else {
                lua_pop(L, 1);
            }
            if (status != LUA_OK && registered) ++observed_failures;
            lua_pushnil(L);
            lua_setglobal(L, "f");
            lua_settop(L, 0);
        }
        collect(L);
        const bool expired = state_weak.expired();
        if (status != LUA_OK && registered && !expired) ++leaked;
        if (!core_weak.expired() && status != LUA_OK)
            std::cout << "core_alive_after_oom fault=" << fault << '\n';
        lua_close(L);
        require(!allocator.live, "Lua allocator leak");
    }
    std::cout << "oom_errors=" << oom_errors
              << " registered_then_oom=" << observed_failures
              << " leaked_states=" << leaked << '\n';
    return leaked ? 3 : 0;
}

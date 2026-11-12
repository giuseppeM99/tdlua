// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic probe: a partial token metatable loses the token finalizer.
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
void callOnComplete(lua_State *L, const char *handle_name, Allocator *allocator,
                    std::size_t fault)
{
    lua_getglobal(L, handle_name);
    lua_getfield(L, -1, "on_complete");
    lua_pushvalue(L, -2);
    lua_getglobal(L, "cb");
    if (allocator) allocator->limit = allocator->calls + fault;
    const int status = lua_pcall(L, 2, 1, 0);
    if (allocator) allocator->limit = 0;
    lua_pop(L, 1);
    (void)status;
}
} // namespace

int main()
{
    Allocator allocator;
    lua_State *L = newState(allocator);
    if (!L) return 2;
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    lua_pushcfunction(L, nothing);
    lua_setglobal(L, "cb");
    std::weak_ptr<tdlua::SchedulerCore> core_weak;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        core_weak = core;
        auto first = router.future();
        tdlua::pushManagedHandle(L, first, "tdlua.future");
        lua_setglobal(L, "f");
        // Deterministic fault used by the metatable probe: __index is
        // installed, __gc is not.
        callOnComplete(L, "f", &allocator, 6);
        lua_pushnil(L);
        lua_setglobal(L, "f");

        auto second = router.future();
        tdlua::pushManagedHandle(L, second, "tdlua.future");
        lua_setglobal(L, "g");
        callOnComplete(L, "g", nullptr, 0);
        lua_pushnil(L);
        lua_setglobal(L, "g");
    }
    std::cout << "before_close live_cores=" << tdlua::liveSchedulerCores()
              << " core_expired=" << core_weak.expired()
              << " anchor_finalizers=" << tdlua::coreAnchorFinalizerCountForTesting()
              << '\n';
    lua_close(L);
    std::cout << "after_close live_cores=" << tdlua::liveSchedulerCores()
              << " anchor_finalizers=" << tdlua::coreAnchorFinalizerCountForTesting()
              << '\n';
    std::cout << "metatable_leak_probe done\n";
    return 0;
}
